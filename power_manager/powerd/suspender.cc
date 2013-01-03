// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power_manager/powerd/suspender.h"

#include <stdlib.h>
#include <sys/wait.h>

#include <algorithm>

#include "base/bind.h"
#include "base/file_path.h"
#include "base/logging.h"
#include "base/string_util.h"
#include "base/stringprintf.h"
#include "base/time.h"
#include "chromeos/dbus/dbus.h"
#include "chromeos/dbus/service_constants.h"
#include "power_manager/common/dbus_sender.h"
#include "power_manager/common/power_constants.h"
#include "power_manager/common/power_prefs.h"
#include "power_manager/common/util.h"
#include "power_manager/common/util_dbus.h"
#include "power_manager/powerd/file_tagger.h"
#include "power_manager/powerd/powerd.h"
#include "power_manager/powerd/suspend_delay_controller.h"
#include "power_manager/powerd/system/input.h"
#include "power_manager/suspend.pb.h"

using base::TimeTicks;
using std::max;
using std::min;

namespace {

const unsigned int kScreenLockerTimeoutMS = 3000;
const unsigned int kMaximumDelayTimeoutMS = 10000;

const char kError[] = ".Error";

}  // namespace

namespace power_manager {

Suspender::Suspender(Daemon* daemon,
                     ScreenLocker* locker,
                     FileTagger* file_tagger,
                     DBusSenderInterface* dbus_sender,
                     system::Input* input,
                     const FilePath& run_dir)
    : daemon_(daemon),
      locker_(locker),
      file_tagger_(file_tagger),
      dbus_sender_(dbus_sender),
      input_(input),
      suspend_delay_controller_(new SuspendDelayController(dbus_sender)),
      suspend_delay_timeout_ms_(0),
      suspend_delays_outstanding_(0),
      suspend_requested_(false),
      suspend_sequence_number_(0),
      check_suspend_timeout_id_(0),
      cancel_suspend_if_lid_open_(true),
      wait_for_screen_lock_(false),
      user_active_file_(run_dir.Append(kUserActiveFile)),
      wakeup_count_valid_(false),
      num_retries_(0),
      suspend_pid_(0),
      retry_suspend_timeout_id_(0) {
  suspend_delay_controller_->AddObserver(this);
}

Suspender::~Suspender() {
  suspend_delay_controller_->RemoveObserver(this);
  util::RemoveTimeout(&check_suspend_timeout_id_);
  util::RemoveTimeout(&retry_suspend_timeout_id_);
}

void Suspender::NameOwnerChangedHandler(DBusGProxy*,
                                        const gchar* name,
                                        const gchar* /*old_owner*/,
                                        const gchar* new_owner,
                                        gpointer data) {
  Suspender* suspender = static_cast<Suspender*>(data);
  if (!name || !new_owner) {
    LOG(ERROR) << "NameOwnerChanged with Null name or new owner.";
    return;
  }
  if (strlen(new_owner) == 0) {
    suspender->suspend_delay_controller_->HandleDBusClientDisconnected(name);
    if (suspender->CleanUpSuspendDelay(name))
      LOG(INFO) << name << " deleted for dbus name change.";
  }
}

void Suspender::Init(PowerPrefs* prefs) {
  int64 retry_delay_ms = 0;
  CHECK(prefs->GetInt64(kRetrySuspendMsPref, &retry_delay_ms));
  retry_delay_ = base::TimeDelta::FromMilliseconds(retry_delay_ms);

  CHECK(prefs->GetInt64(kRetrySuspendAttemptsPref, &max_retries_));
}

void Suspender::RequestSuspend(bool cancel_if_lid_open) {
  unsigned int timeout_ms = 0;
  suspend_requested_ = true;
  suspend_delays_outstanding_ = suspend_delays_.size();
  cancel_suspend_if_lid_open_ = cancel_if_lid_open;
  wakeup_count_ = 0;
  wakeup_count_valid_ = false;
  if (util::GetWakeupCount(&wakeup_count_)) {
    wakeup_count_valid_ = true;
  } else {
    LOG(ERROR) << "Could not get wakeup_count prior to suspend.";
    wakeup_count_valid_ = false;
  }

  suspend_sequence_number_++;
  suspend_delay_controller_->PrepareForSuspend(suspend_sequence_number_);
  BroadcastSignalToClients(kSuspendDelay, suspend_sequence_number_);

  // TODO(derat): Make Chrome just register a suspend delay and lock the screen
  // itself if lock-on-suspend is enabled instead of setting a powerd pref.
  wait_for_screen_lock_ = locker_->lock_on_suspend_enabled();
  if (wait_for_screen_lock_) {
    locker_->LockScreen();
    timeout_ms = max(kScreenLockerTimeoutMS, suspend_delay_timeout_ms_);
  } else {
    timeout_ms = suspend_delay_timeout_ms_;
  }

  timeout_ms = min(kMaximumDelayTimeoutMS, timeout_ms);
  LOG(INFO) << "Request Suspend #" << suspend_sequence_number_
            << " Delay Timeout = " << timeout_ms;

  util::RemoveTimeout(&check_suspend_timeout_id_);
  if (timeout_ms > 0) {
    check_suspend_timeout_id_ = g_timeout_add(
        timeout_ms, CheckSuspendTimeoutThunk, this);
  }
}

void Suspender::CheckSuspend() {
  if (suspend_requested_ &&
      suspend_delays_outstanding_ == 0 &&
      suspend_delay_controller_->ready_for_suspend() &&
      (!wait_for_screen_lock_ || locker_->is_locked())) {
    util::RemoveTimeout(&check_suspend_timeout_id_);
    suspend_requested_ = false;
    LOG(INFO) << "All suspend delays accounted for. Suspending.";
    Suspend();
  }
}

void Suspender::CancelSuspend() {
  if (suspend_requested_) {
    LOG(INFO) << "Suspend canceled mid flight.";
    daemon_->ResumePollPowerSupply();

    // Send a PowerStateChanged "on" signal when suspend is canceled.
    //
    // TODO(benchan): Refactor this code and the code in the powerd_suspend
    // script.
    chromeos::dbus::Proxy proxy(chromeos::dbus::GetSystemBusConnection(),
                                kPowerManagerServicePath,
                                kPowerManagerInterface);
    DBusMessage* signal = dbus_message_new_signal(kPowerManagerServicePath,
                                                  kPowerManagerInterface,
                                                  kPowerStateChanged);
    const char* power_state = "on";
    int32 suspend_rc = -1;
    dbus_message_append_args(signal, DBUS_TYPE_STRING, &power_state,
                             DBUS_TYPE_INT32, &suspend_rc,
                             DBUS_TYPE_INVALID);
    dbus_g_proxy_send(proxy.gproxy(), signal, NULL);
    dbus_message_unref(signal);
  }

  suspend_requested_ = false;
  suspend_delays_outstanding_ = 0;
  util::RemoveTimeout(&check_suspend_timeout_id_);
}

DBusMessage* Suspender::RegisterSuspendDelay(DBusMessage* message) {
  RegisterSuspendDelayRequest request;
  if (util::ParseProtocolBufferFromDBusMessage(message, &request)) {
    RegisterSuspendDelayReply reply_proto;
    suspend_delay_controller_->RegisterSuspendDelay(
        request, util::GetDBusSender(message), &reply_proto);
    return util::CreateDBusProtocolBufferReply(message, reply_proto);
  }

  // TODO(derat); Remove everything after this after clients are updated to use
  // the protocol-buffer-based version above: http://crosbug.com/36980
  DBusMessage* reply = util::CreateEmptyDBusReply(message);
  CHECK(reply);

  uint32 delay_ms = 0;
  DBusError error;
  dbus_error_init(&error);
  if (!dbus_message_get_args(message, &error,
                             DBUS_TYPE_UINT32, &delay_ms,
                             DBUS_TYPE_INVALID)) {
    LOG(WARNING) << "Couldn't read args for RegisterSuspendDelay request";
    const std::string errmsg =
        StringPrintf("%s%s", kPowerManagerInterface, kError);
    dbus_message_set_error_name(reply, errmsg.c_str());
    if (dbus_error_is_set(&error))
      dbus_error_free(&error);
    return reply;
  }

  const char* client_name = dbus_message_get_sender(message);
  if (!client_name) {
    LOG(ERROR) << "dbus_message_get_sender returned NULL name.";
    return reply;
  }

  LOG(INFO) << "register-suspend-delay, client: " << client_name
            << " delay_ms: " << delay_ms;
  if (delay_ms > 0) {
    suspend_delays_[client_name] = delay_ms;
    if (delay_ms > suspend_delay_timeout_ms_)
      suspend_delay_timeout_ms_ = delay_ms;
  }
  return reply;
}

DBusMessage* Suspender::UnregisterSuspendDelay(DBusMessage* message) {
  UnregisterSuspendDelayRequest request;
  if (util::ParseProtocolBufferFromDBusMessage(message, &request)) {
    suspend_delay_controller_->UnregisterSuspendDelay(
        request, util::GetDBusSender(message));
    return NULL;
  }

  // TODO(derat): Remove everything after this after clients are updated to use
  // the protocol-buffer-based version above: http://crosbug.com/36980
  DBusMessage* reply = util::CreateEmptyDBusReply(message);
  CHECK(reply);

  const char* client_name = dbus_message_get_sender(message);
  if (!client_name) {
    LOG(ERROR) << "dbus_message_get_sender returned NULL name.";
    return reply;
  }

  LOG(INFO) << "unregister-suspend-delay, client: " << client_name;
  if (!CleanUpSuspendDelay(client_name)) {
    const std::string errmsg =
        StringPrintf("%s%s", kPowerManagerInterface, kError);
    dbus_message_set_error_name(reply, errmsg.c_str());
  }
  return reply;
}

DBusMessage* Suspender::HandleSuspendReadiness(DBusMessage* message) {
  SuspendReadinessInfo info;
  if (!util::ParseProtocolBufferFromDBusMessage(message, &info)) {
    LOG(ERROR) << "Unable to parse HandleSuspendReadiness request";
    return util::CreateDBusInvalidArgsErrorReply(message);
  }
  suspend_delay_controller_->HandleSuspendReadiness(
      info, util::GetDBusSender(message));
  return NULL;
}

bool Suspender::SuspendReady(DBusMessage* message) {
  const char* client_name = dbus_message_get_sender(message);
  if (!client_name) {
    LOG(ERROR) << "dbus_message_get_sender returned NULL name.";
    return true;
  }
  LOG(INFO) << "SuspendReady, client : " << client_name;
  SuspendList::iterator iter = suspend_delays_.find(client_name);
  if (suspend_delays_.end() == iter) {
    LOG(WARNING) << "Unregistered client attempting to ack SuspendReady!";
    return true;
  }
  DBusError error;
  dbus_error_init(&error);
  unsigned int sequence_num;
  if (!dbus_message_get_args(message, &error, DBUS_TYPE_UINT32, &sequence_num,
                             DBUS_TYPE_INVALID)) {
    LOG(ERROR) << "Could not get args from SuspendReady signal!";
    if (dbus_error_is_set(&error))
      dbus_error_free(&error);
    return true;
  }
  if (static_cast<int>(sequence_num) == suspend_sequence_number_) {
    LOG(INFO) << "Suspend sequence number match! " << sequence_num;
    suspend_delays_outstanding_--;
    LOG(INFO) << "suspend delays outstanding = " << suspend_delays_outstanding_;
    CheckSuspend();
  } else {
    LOG(INFO) << "Out of sequence SuspendReady ack!";
  }

  return true;
}

void Suspender::HandlePowerStateChanged(const char* state, int power_rc) {
  // on == resume via powerd_suspend
  if (strcmp(state, "on") == 0) {
    LOG(INFO) << "Resuming has commenced";
    if (power_rc == 0) {
      util::RemoveTimeout(&retry_suspend_timeout_id_);
      daemon_->GenerateRetrySuspendMetric(num_retries_, max_retries_);
      num_retries_ = 0;
    } else {
      LOG(INFO) << "Suspend attempt failed";
    }
#ifdef SUSPEND_LOCK_VT
    // Allow virtual terminal switching again.
    input_->SetVTSwitchingState(true);
#endif
    SendSuspendStateChangedSignal(
        SuspendState_Type_RESUME, base::Time::Now());
  } else if (strcmp(state, "mem") == 0) {
    SendSuspendStateChangedSignal(
        SuspendState_Type_SUSPEND_TO_MEMORY, last_suspend_wall_time_);
  } else {
    DLOG(INFO) << "Saw arg:" << state << " for " << kPowerStateChanged;
  }
}

void Suspender::OnReadyForSuspend(int suspend_id) {
  if (suspend_id == suspend_sequence_number_)
    CheckSuspend();
}

void Suspender::Suspend() {
  LOG(INFO) << "Launching Suspend";
  if ((suspend_pid_ > 0) && !kill(-suspend_pid_, 0)) {
    LOG(ERROR) << "Previous retry suspend pid:"
               << suspend_pid_ << " is still running";
  }

  daemon_->HaltPollPowerSupply();
  daemon_->MarkPowerStatusStale();
  util::RemoveStatusFile(user_active_file_);
  file_tagger_->HandleSuspendEvent();

  util::RemoveTimeout(&retry_suspend_timeout_id_);
  retry_suspend_timeout_id_ =
      g_timeout_add(retry_delay_.InMilliseconds(), RetrySuspendThunk, this);

#ifdef SUSPEND_LOCK_VT
  // Do not let suspend change the console terminal.
  input_->SetVTSwitchingState(false);
#endif

  // Cache the current time so we can include it in the SuspendStateChanged
  // signal that we emit from HandlePowerStateChangedSignal() -- we might not
  // send it until after the system has already resumed.
  last_suspend_wall_time_ = base::Time::Now();

  std::string suspend_command = "powerd_setuid_helper --action=suspend";
  if (wakeup_count_valid_) {
    suspend_command +=
        StringPrintf(" --suspend_wakeup_count %d", wakeup_count_);
  }
  if (cancel_suspend_if_lid_open_)
    suspend_command += " --suspend_cancel_if_lid_open";
  LOG(INFO) << "Running \"" << suspend_command << "\"";

  // Detach to allow suspend to be retried and metrics gathered
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    if (fork() == 0) {
      wait(NULL);
      exit(::system(suspend_command.c_str()));
    } else {
      exit(0);
    }
  } else if (pid > 0) {
    suspend_pid_ = pid;
    waitpid(pid, NULL, 0);
  } else {
    LOG(ERROR) << "Fork for suspend failed";
  }
}

gboolean Suspender::RetrySuspend() {
  retry_suspend_timeout_id_ = 0;

  if (num_retries_ >= max_retries_) {
    LOG(ERROR) << "Retried suspend " << num_retries_ << " times; shutting down";
    daemon_->ShutdownForFailedSuspend();
    return FALSE;
  }

  num_retries_++;
  LOG(WARNING) << "Retry suspend attempt #" << num_retries_;
  wakeup_count_valid_ = util::GetWakeupCount(&wakeup_count_);
  Suspend();
  return FALSE;
}

void Suspender::SendSuspendStateChangedSignal(SuspendState_Type type,
                                              const base::Time& wall_time) {
  SuspendState proto;
  proto.set_type(type);
  proto.set_wall_time(wall_time.ToInternalValue());
  dbus_sender_->EmitSignalWithProtocolBuffer(kSuspendStateChangedSignal, proto);
}

gboolean Suspender::CheckSuspendTimeout() {
  LOG(ERROR) << "Suspend delay timed out. Seq num = "
             << suspend_sequence_number_;
  check_suspend_timeout_id_ = 0;
  suspend_delays_outstanding_ = 0;
  // Give up on waiting for the screen to be locked if it isn't already.
  wait_for_screen_lock_ = false;
  CheckSuspend();
  return FALSE;
}

// Remove |client_name| from list of suspend delay callback clients.
bool Suspender::CleanUpSuspendDelay(const char* client_name) {
  if (!client_name) {
    LOG(ERROR) << "NULL client_name.";
    return false;
  }
  if (suspend_delays_.empty()) {
    return false;
  }
  SuspendList::iterator iter = suspend_delays_.find(client_name);
  if (suspend_delays_.end() == iter) {
    // not a client
    return false;
  }
  LOG(INFO) << "Client " << client_name << " unregistered.";
  unsigned int timeout_ms = iter->second;
  suspend_delays_.erase(iter);
  if (timeout_ms == suspend_delay_timeout_ms_) {
    // find highest timeout value.
    suspend_delay_timeout_ms_ = 0;
    for (iter = suspend_delays_.begin();
         iter != suspend_delays_.end(); ++iter) {
      if (iter->second > suspend_delay_timeout_ms_)
        suspend_delay_timeout_ms_ = iter->second;
    }
  }
  return true;
}

// Broadcast signal, with sequence number payload
void Suspender::BroadcastSignalToClients(const char* signal_name,
                                         int sequence_num) {
  dbus_uint32_t payload = static_cast<dbus_uint32_t>(sequence_num);
  if (!signal_name) {
    LOG(ERROR) << "signal_name NULL pointer!";
    return;
  }
  LOG(INFO) << "Sending Broadcast '" << signal_name << "' to PowerManager:";
  chromeos::dbus::Proxy proxy(chromeos::dbus::GetSystemBusConnection(),
                              power_manager::kPowerManagerServicePath,
                              power_manager::kPowerManagerInterface);
  DBusMessage* signal = ::dbus_message_new_signal(
      "/",
      power_manager::kPowerManagerInterface,
      signal_name);
  CHECK(signal);
  dbus_message_append_args(signal,
                           DBUS_TYPE_UINT32, &payload,
                           DBUS_TYPE_INVALID);
  ::dbus_g_proxy_send(proxy.gproxy(), signal, NULL);
  ::dbus_message_unref(signal);
}

}  // namespace power_manager
