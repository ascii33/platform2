// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/service.h"

#include <stdio.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <base/memory/scoped_ptr.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/stringprintf.h>
#include <chromeos/dbus/service_constants.h>

#include "shill/connection.h"
#include "shill/control_interface.h"
#include "shill/diagnostics_reporter.h"
#include "shill/eap_credentials.h"
#include "shill/error.h"
#include "shill/http_proxy.h"
#include "shill/logging.h"
#include "shill/manager.h"
#include "shill/metrics.h"
#include "shill/profile.h"
#include "shill/property_accessor.h"
#include "shill/refptr_types.h"
#include "shill/service_dbus_adaptor.h"
#include "shill/service_property_change_notifier.h"
#include "shill/sockets.h"
#include "shill/store_interface.h"

using base::Bind;
using std::deque;
using std::map;
using std::string;
using std::vector;

namespace shill {

const char Service::kAutoConnBusy[] = "busy";
const char Service::kAutoConnConnected[] = "connected";
const char Service::kAutoConnConnecting[] = "connecting";
const char Service::kAutoConnExplicitDisconnect[] = "explicitly disconnected";
const char Service::kAutoConnNotConnectable[] = "not connectable";
const char Service::kAutoConnOffline[] = "offline";
const char Service::kAutoConnThrottled[] = "throttled";

const size_t Service::kEAPMaxCertificationElements = 10;

const char Service::kCheckPortalAuto[] = "auto";
const char Service::kCheckPortalFalse[] = "false";
const char Service::kCheckPortalTrue[] = "true";

const char Service::kErrorDetailsNone[] = "";

const int Service::kPriorityNone = 0;

const char Service::kServiceSortAutoConnect[] = "AutoConnect";
const char Service::kServiceSortConnectable[] = "Connectable";
const char Service::kServiceSortDependency[] = "Dependency";
const char Service::kServiceSortHasEverConnected[] = "HasEverConnected";
const char Service::kServiceSortIsConnected[] = "IsConnected";
const char Service::kServiceSortIsConnecting[] = "IsConnecting";
const char Service::kServiceSortIsFailed[] = "IsFailed";
const char Service::kServiceSortIsPortalled[] = "IsPortal";
const char Service::kServiceSortPriority[] = "Priority";
const char Service::kServiceSortSecurity[] = "Security";
const char Service::kServiceSortProfileOrder[] = "ProfileOrder";
const char Service::kServiceSortEtc[] = "Etc";
const char Service::kServiceSortSerialNumber[] = "SerialNumber";
const char Service::kServiceSortTechnology[] = "Technology";

const char Service::kStorageAutoConnect[] = "AutoConnect";
const char Service::kStorageCheckPortal[] = "CheckPortal";
const char Service::kStorageDNSAutoFallback[] = "DNSAutoFallback";
const char Service::kStorageError[] = "Error";
const char Service::kStorageFavorite[] = "Favorite";
const char Service::kStorageGUID[] = "GUID";
const char Service::kStorageHasEverConnected[] = "HasEverConnected";
const char Service::kStorageName[] = "Name";
const char Service::kStoragePriority[] = "Priority";
const char Service::kStorageProxyConfig[] = "ProxyConfig";
const char Service::kStorageSaveCredentials[] = "SaveCredentials";
const char Service::kStorageType[] = "Type";
const char Service::kStorageUIData[] = "UIData";
const char Service::kStorageConnectionId[] = "ConnectionId";

const uint8_t Service::kStrengthMax = 100;
const uint8_t Service::kStrengthMin = 0;

const uint64_t Service::kMaxAutoConnectCooldownTimeMilliseconds =
    30 * 60 * 1000;
const uint64_t Service::kMinAutoConnectCooldownTimeMilliseconds = 1000;
const uint64_t Service::kAutoConnectCooldownBackoffFactor = 2;

const int Service::kDisconnectsMonitorSeconds = 5 * 60;
const int Service::kMisconnectsMonitorSeconds = 5 * 60;
const int Service::kReportDisconnectsThreshold = 2;
const int Service::kReportMisconnectsThreshold = 3;
const int Service::kMaxDisconnectEventHistory = 20;

// static
unsigned int Service::next_serial_number_ = 0;

Service::Service(ControlInterface *control_interface,
                 EventDispatcher *dispatcher,
                 Metrics *metrics,
                 Manager *manager,
                 Technology::Identifier technology)
    : weak_ptr_factory_(this),
      state_(kStateIdle),
      previous_state_(kStateIdle),
      failure_(kFailureUnknown),
      auto_connect_(false),
      retain_auto_connect_(false),
      check_portal_(kCheckPortalAuto),
      connectable_(false),
      error_(ConnectFailureToString(failure_)),
      error_details_(kErrorDetailsNone),
      previous_error_serial_number_(0),
      explicitly_disconnected_(false),
      is_in_user_connect_(false),
      priority_(kPriorityNone),
      crypto_algorithm_(kCryptoNone),
      key_rotation_(false),
      endpoint_auth_(false),
      strength_(0),
      save_credentials_(true),
      technology_(technology),
      failed_time_(0),
      has_ever_connected_(false),
      auto_connect_cooldown_milliseconds_(0),
      store_(PropertyStore::PropertyChangeCallback(
          base::Bind(&Service::OnPropertyChanged,
                     weak_ptr_factory_.GetWeakPtr()))),
      dispatcher_(dispatcher),
      serial_number_(next_serial_number_++),
      unique_name_(base::UintToString(serial_number_)),
      friendly_name_(unique_name_),
      adaptor_(control_interface->CreateServiceAdaptor(this)),
      property_change_notifier_(
          new ServicePropertyChangeNotifier(adaptor_.get())),
      metrics_(metrics),
      manager_(manager),
      sockets_(new Sockets()),
      time_(Time::GetInstance()),
      diagnostics_reporter_(DiagnosticsReporter::GetInstance()),
      connection_id_(0),
      is_dns_auto_fallback_allowed_(false) {
  HelpRegisterDerivedBool(kAutoConnectProperty,
                          &Service::GetAutoConnect,
                          &Service::SetAutoConnectFull,
                          &Service::ClearAutoConnect);

  // kActivationTypeProperty: Registered in CellularService
  // kActivationStateProperty: Registered in CellularService
  // kCellularApnProperty: Registered in CellularService
  // kCellularLastGoodApnProperty: Registered in CellularService
  // kNetworkTechnologyProperty: Registered in CellularService
  // kOutOfCreditsProperty: Registered in CellularService
  // kPaymentPortalProperty: Registered in CellularService
  // kRoamingStateProperty: Registered in CellularService
  // kServingOperatorProperty: Registered in CellularService
  // kUsageURLProperty: Registered in CellularService
  // kCellularPPPUsernameProperty: Registered in CellularService
  // kCellularPPPPasswordProperty: Registered in CellularService

  // kNetworkIdProperty: Registered in WiMaxService

  HelpRegisterDerivedString(kCheckPortalProperty,
                            &Service::GetCheckPortal,
                            &Service::SetCheckPortal);
  store_.RegisterConstBool(kConnectableProperty, &connectable_);
  HelpRegisterConstDerivedRpcIdentifier(kDeviceProperty,
                                        &Service::GetDeviceRpcId);
  store_.RegisterConstStrings(kEapRemoteCertificationProperty,
                              &remote_certification_);
  HelpRegisterDerivedString(kGuidProperty,
                            &Service::GetGuid,
                            &Service::SetGuid);

  // TODO(ers): in flimflam clearing Error has the side-effect of
  // setting the service state to IDLE. Is this important? I could
  // see an autotest depending on it.
  store_.RegisterConstString(kErrorProperty, &error_);
  store_.RegisterConstString(kErrorDetailsProperty, &error_details_);
  HelpRegisterConstDerivedUint16(kHTTPProxyPortProperty,
                                 &Service::GetHTTPProxyPort);
  HelpRegisterConstDerivedRpcIdentifier(kIPConfigProperty,
                                        &Service::GetIPConfigRpcIdentifier);
  HelpRegisterDerivedBool(kIsActiveProperty, &Service::IsActive, NULL, NULL);
  // kModeProperty: Registered in WiFiService

  HelpRegisterDerivedString(kNameProperty,
                            &Service::GetNameProperty,
                            &Service::SetNameProperty);
  // kPassphraseProperty: Registered in WiFiService
  // kPassphraseRequiredProperty: Registered in WiFiService, WiMaxService
  store_.RegisterConstString(kPreviousErrorProperty,
                             &previous_error_);
  store_.RegisterConstInt32(kPreviousErrorSerialNumberProperty,
                            &previous_error_serial_number_);
  HelpRegisterDerivedInt32(kPriorityProperty,
                           &Service::GetPriority,
                           &Service::SetPriority);
  HelpRegisterDerivedString(kProfileProperty,
                            &Service::GetProfileRpcId,
                            &Service::SetProfileRpcId);
  HelpRegisterDerivedString(kProxyConfigProperty,
                            &Service::GetProxyConfig,
                            &Service::SetProxyConfig);
  store_.RegisterBool(kSaveCredentialsProperty, &save_credentials_);
  HelpRegisterConstDerivedString(kTetheringProperty,
                                 &Service::GetTethering);
  HelpRegisterDerivedString(kTypeProperty,
                            &Service::CalculateTechnology,
                            NULL);
  // kSecurityProperty: Registered in WiFiService
  HelpRegisterDerivedString(kStateProperty,
                            &Service::CalculateState,
                            NULL);
  store_.RegisterConstUint8(kSignalStrengthProperty, &strength_);
  store_.RegisterString(kUIDataProperty, &ui_data_);
  HelpRegisterConstDerivedStrings(kDiagnosticsDisconnectsProperty,
                                  &Service::GetDisconnectsProperty);
  HelpRegisterConstDerivedStrings(kDiagnosticsMisconnectsProperty,
                                  &Service::GetMisconnectsProperty);
  store_.RegisterConstInt32(kConnectionIdProperty, &connection_id_);
  store_.RegisterBool(kDnsAutoFallbackProperty, &is_dns_auto_fallback_allowed_);

  HelpRegisterObservedDerivedBool(kVisibleProperty,
                                  &Service::GetVisibleProperty,
                                  NULL,
                                  NULL);

  store_.RegisterConstString(kPortalDetectionFailedPhaseProperty,
                             &portal_detection_failure_phase_);
  store_.RegisterConstString(kPortalDetectionFailedStatusProperty,
                             &portal_detection_failure_status_);

  metrics_->RegisterService(*this);

  static_ip_parameters_.PlumbPropertyStore(&store_);

  IgnoreParameterForConfigure(kTypeProperty);
  IgnoreParameterForConfigure(kProfileProperty);

  LOG(INFO) << Technology::NameFromIdentifier(technology) << " service "
            << unique_name_ << " constructed.";
}

Service::~Service() {
  metrics_->DeregisterService(*this);
  LOG(INFO) << "Service " << unique_name_ << " destroyed.";
}

void Service::AutoConnect() {
  const char *reason = NULL;
  if (IsAutoConnectable(&reason)) {
    Error error;
    LOG(INFO) << "Auto-connecting to service " << unique_name_;
    ThrottleFutureAutoConnects();
    Connect(&error, __func__);
  } else {
    if (reason == kAutoConnConnected || reason == kAutoConnBusy) {
      SLOG(Service, 1)
          << "Suppressed autoconnect to service " << unique_name_ << " "
          << "(" << reason << ")";
    } else {
      LOG(INFO) << "Suppressed autoconnect to service " << unique_name_ << " "
                << "(" << reason << ")";
    }
  }
}

void Service::Connect(Error */*error*/, const char *reason) {
  LOG(INFO) << "Connect to service " << unique_name() <<": " << reason;
  ClearExplicitlyDisconnected();
  // Clear any failure state from a previous connect attempt.
  if (state() == kStateFailure)
    SetState(kStateIdle);
}

void Service::Disconnect(Error */*error*/, const char *reason) {
  LOG(INFO) << "Disconnecting from service " << unique_name_ << ": " << reason;
}

void Service::DisconnectWithFailure(ConnectFailure failure,
                                    Error *error,
                                    const char *reason) {
  Disconnect(error, reason);
  SetFailure(failure);
}

void Service::UserInitiatedDisconnect(Error *error) {
  Disconnect(error, "D-Bus RPC");
  explicitly_disconnected_ = true;
}

void Service::UserInitiatedConnect(Error *error) {
  Connect(error, "D-Bus RPC");
  is_in_user_connect_ = true;
}

void Service::ActivateCellularModem(const string &/*carrier*/,
                                    Error *error,
                                    const ResultCallback &/*callback*/) {
  Error::PopulateAndLog(error, Error::kNotSupported,
                        "Service doesn't support cellular modem activation.");
}

void Service::CompleteCellularActivation(Error *error) {
  Error::PopulateAndLog(
      error, Error::kNotSupported,
      "Service doesn't support cellular activation completion.");
}

bool Service::IsActive(Error */*error*/) {
  return state() != kStateUnknown &&
    state() != kStateIdle &&
    state() != kStateFailure;
}

// static
bool Service::IsConnectedState(ConnectState state) {
  return (state == kStateConnected ||
          state == kStatePortal ||
          state == kStateOnline);
}

// static
bool Service::IsConnectingState(ConnectState state) {
  return (state == kStateAssociating ||
          state == kStateConfiguring);
}

bool Service::IsConnected() const {
  return IsConnectedState(state());
}

bool Service::IsConnecting() const {
  return IsConnectingState(state());
}

void Service::SetState(ConnectState state) {
  if (state == state_) {
    return;
  }

  LOG(INFO) << "Service " << unique_name_ << ": state "
            << ConnectStateToString(state_) << " -> "
            << ConnectStateToString(state);

  // Metric reporting for result of user-initiated connection attempt.
  if (is_in_user_connect_ && ((state == kStateConnected) ||
      (state == kStateFailure) || (state == kStateIdle))) {
    ReportUserInitiatedConnectionResult(state);
    is_in_user_connect_ = false;
  }

  if (state == kStateFailure) {
    NoteDisconnectEvent();
  }

  previous_state_ = state_;
  state_ = state;
  if (state != kStateFailure) {
    failure_ = kFailureUnknown;
    SetErrorDetails(kErrorDetailsNone);
  }
  if (state == kStateConnected) {
    failed_time_ = 0;
    has_ever_connected_ = true;
    SaveToProfile();
    // When we succeed in connecting, forget that connects failed in the past.
    // Give services one chance at a fast autoconnect retry by resetting the
    // cooldown to 0 to indicate that the last connect was successful.
    auto_connect_cooldown_milliseconds_  = 0;
    reenable_auto_connect_task_.Cancel();
  }
  UpdateErrorProperty();
  manager_->UpdateService(this);
  metrics_->NotifyServiceStateChanged(*this, state);
  adaptor_->EmitStringChanged(kStateProperty, GetStateString());
}

void Service::SetPortalDetectionFailure(const string &phase,
                                        const string &status) {
  portal_detection_failure_phase_ = phase;
  portal_detection_failure_status_ = status;
  adaptor_->EmitStringChanged(kPortalDetectionFailedPhaseProperty, phase);
  adaptor_->EmitStringChanged(kPortalDetectionFailedStatusProperty, status);
}

void Service::ReEnableAutoConnectTask() {
  // Kill the thing blocking AutoConnect().
  reenable_auto_connect_task_.Cancel();
  // Post to the manager, giving it an opportunity to AutoConnect again.
  manager_->UpdateService(this);
}

void Service::ThrottleFutureAutoConnects() {
  if (auto_connect_cooldown_milliseconds_ > 0) {
    LOG(INFO) << "Throttling future autoconnects to service " << unique_name_
              << ". Next autoconnect in "
              << auto_connect_cooldown_milliseconds_ << " milliseconds.";
    reenable_auto_connect_task_.Reset(Bind(&Service::ReEnableAutoConnectTask,
                                           weak_ptr_factory_.GetWeakPtr()));
    dispatcher_->PostDelayedTask(reenable_auto_connect_task_.callback(),
                                 auto_connect_cooldown_milliseconds_);
  }
  auto_connect_cooldown_milliseconds_ =
      std::min(kMaxAutoConnectCooldownTimeMilliseconds,
               std::max(kMinAutoConnectCooldownTimeMilliseconds,
                        auto_connect_cooldown_milliseconds_ *
                        kAutoConnectCooldownBackoffFactor));
}

void Service::SaveFailure() {
  previous_error_ = ConnectFailureToString(failure_);
  ++previous_error_serial_number_;
}

void Service::SetFailure(ConnectFailure failure) {
  failure_ = failure;
  SaveFailure();
  failed_time_ = time(NULL);
  UpdateErrorProperty();
  SetState(kStateFailure);
}

void Service::SetFailureSilent(ConnectFailure failure) {
  NoteDisconnectEvent();
  // Note that order matters here, since SetState modifies |failure_| and
  // |failed_time_|.
  SetState(kStateIdle);
  failure_ = failure;
  SaveFailure();
  UpdateErrorProperty();
  failed_time_ = time(NULL);
}

string Service::GetRpcIdentifier() const {
  return adaptor_->GetRpcIdentifier();
}

string Service::GetLoadableStorageIdentifier(
    const StoreInterface &storage) const {
  return IsLoadableFrom(storage) ? GetStorageIdentifier() : "";
}

bool Service::IsLoadableFrom(const StoreInterface &storage) const {
  return storage.ContainsGroup(GetStorageIdentifier());
}

bool Service::Load(StoreInterface *storage) {
  const string id = GetStorageIdentifier();
  SLOG(Service, 3) << "Loading service profile with ID " << id;
  if (!storage->ContainsGroup(id)) {
    LOG(WARNING) << "Service is not available in the persistent store: " << id;
    return false;
  }

  auto_connect_ = IsAutoConnectByDefault();
  SLOG(Service, 3) << "  Autoconnect(default): " << auto_connect_;
  retain_auto_connect_ =
      storage->GetBool(id, kStorageAutoConnect, &auto_connect_);
  SLOG(Service, 3) << "  Autoconnect(from profile): " << auto_connect_;
  SLOG(Service, 3) << "  RetainAutoconnect: " << retain_auto_connect_;
  // The legacy "Favorite" flag will override retain_auto_connect_ if present.
  storage->GetBool(id, kStorageFavorite, &retain_auto_connect_);

  LoadString(storage, id, kStorageCheckPortal, kCheckPortalAuto,
             &check_portal_);
  LoadString(storage, id, kStorageGUID, "", &guid_);
  if (!storage->GetInt(id, kStoragePriority, &priority_)) {
    priority_ = kPriorityNone;
  }
  LoadString(storage, id, kStorageProxyConfig, "", &proxy_config_);
  storage->GetBool(id, kStorageSaveCredentials, &save_credentials_);
  LoadString(storage, id, kStorageUIData, "", &ui_data_);

  storage->GetInt(id, kStorageConnectionId, &connection_id_);
  storage->GetBool(id, kStorageDNSAutoFallback, &is_dns_auto_fallback_allowed_);

  static_ip_parameters_.Load(storage, id);

  // The OnEapCredentialsChanged() call below will call
  // SetHasEverConnected(false) for some Service subclasses.  To
  // avoid the side-effects of this call, reset has_ever_connected_
  // first, and load the new value from the profile later.
  if (mutable_eap()) {
    has_ever_connected_ = false;
    mutable_eap()->Load(storage, id);
    OnEapCredentialsChanged();
  }

  ClearExplicitlyDisconnected();

  // Read has_ever_connected_ value from stored profile
  // now that the credentials have been loaded.
  storage->GetBool(id, kStorageHasEverConnected, &has_ever_connected_);

  return true;
}

bool Service::Unload() {
  auto_connect_ = IsAutoConnectByDefault();
  retain_auto_connect_ = false;
  check_portal_ = kCheckPortalAuto;
  ClearExplicitlyDisconnected();
  guid_ = "";
  has_ever_connected_ = false;
  priority_ = kPriorityNone;
  proxy_config_ = "";
  save_credentials_ = true;
  ui_data_ = "";
  connection_id_ = 0;
  is_dns_auto_fallback_allowed_ = false;
  if (mutable_eap()) {
    mutable_eap()->Reset();
  }
  ClearEAPCertification();

  Error error;  // Ignored.
  Disconnect(&error, __func__);
  return false;
}

void Service::Remove(Error */*error*/) {
  manager()->RemoveService(this);
  // |this| may no longer be valid now.
}

bool Service::Save(StoreInterface *storage) {
  const string id = GetStorageIdentifier();
  SLOG(Service, 3) << "Saving service profile with ID " << id;

  storage->SetString(id, kStorageType, GetTechnologyString());
  SLOG(Service, 3) << "  StorageType: " << GetTechnologyString();

  if (retain_auto_connect_) {
    storage->SetBool(id, kStorageAutoConnect, auto_connect_);
    SLOG(Service, 3) << "  AutoConnect: " << auto_connect_;
  } else {
    storage->DeleteKey(id, kStorageAutoConnect);
    SLOG(Service, 3) << "  Autoconnect cleared.";
  }

  // Remove this legacy flag.
  storage->DeleteKey(id, kStorageFavorite);

  if (check_portal_ == kCheckPortalAuto) {
    storage->DeleteKey(id, kStorageCheckPortal);
  } else {
    storage->SetString(id, kStorageCheckPortal, check_portal_);
  }

  SaveString(storage, id, kStorageGUID, guid_, false, true);
  storage->SetBool(id, kStorageHasEverConnected, has_ever_connected_);
  storage->SetString(id, kStorageName, friendly_name_);
  SLOG(Service, 3) << "  Service Friendly Name: " << friendly_name_;
  if (priority_ != kPriorityNone) {
    storage->SetInt(id, kStoragePriority, priority_);
  } else {
    storage->DeleteKey(id, kStoragePriority);
  }
  SaveString(storage, id, kStorageProxyConfig, proxy_config_, false, true);
  storage->SetBool(id, kStorageSaveCredentials, save_credentials_);
  SaveString(storage, id, kStorageUIData, ui_data_, false, true);

  storage->SetInt(id, kStorageConnectionId, connection_id_);
  storage->SetBool(id, kStorageDNSAutoFallback, is_dns_auto_fallback_allowed_);

  static_ip_parameters_.Save(storage, id);
  if (eap()) {
    eap()->Save(storage, id, save_credentials_);
  }
  return true;
}

void Service::SaveToCurrentProfile() {
  // Some unittests do not specify a manager.
  if (manager()) {
    manager()->SaveServiceToProfile(this);
  }
}

void Service::Configure(const KeyValueStore &args, Error *error) {
  SLOG(Service, 5) << "Configuring bool properties:";
  for (const auto &bool_it : args.bool_properties()) {
    if (ContainsKey(parameters_ignored_for_configure_, bool_it.first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << bool_it.first;
    Error set_error;
    store_.SetBoolProperty(bool_it.first, bool_it.second, &set_error);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
  SLOG(Service, 5) << "Configuring int32_t properties:";
  for (const auto &int_it : args.int_properties()) {
    if (ContainsKey(parameters_ignored_for_configure_, int_it.first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << int_it.first;
    Error set_error;
    store_.SetInt32Property(int_it.first, int_it.second, &set_error);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
  SLOG(Service, 5) << "Configuring string properties:";
  for (const auto &string_it : args.string_properties()) {
    if (ContainsKey(parameters_ignored_for_configure_, string_it.first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << string_it.first;
    Error set_error;
    store_.SetStringProperty(string_it.first, string_it.second, &set_error);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
  SLOG(Service, 5) << "Configuring string array properties:";
  for (const auto &strings_it : args.strings_properties()) {
    if (ContainsKey(parameters_ignored_for_configure_, strings_it.first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << strings_it.first;
    Error set_error;
    store_.SetStringsProperty(strings_it.first, strings_it.second, &set_error);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
  SLOG(Service, 5) << "Configuring string map properties:";
  for (const auto &stringmap_it : args.stringmap_properties()) {
    if (ContainsKey(parameters_ignored_for_configure_, stringmap_it.first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << stringmap_it.first;
    Error set_error;
    store_.SetStringmapProperty(
        stringmap_it.first, stringmap_it.second, &set_error);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
}

bool Service::DoPropertiesMatch(const KeyValueStore &args) const {
  SLOG(Service, 5) << "Checking bool properties:";
  for (const auto &bool_it : args.bool_properties()) {
    SLOG(Service, 5) << "   " << bool_it.first;
    Error get_error;
    bool value;
    if (!store_.GetBoolProperty(bool_it.first, &value, &get_error) ||
        value != bool_it.second) {
      return false;
    }
  }
  SLOG(Service, 5) << "Checking int32_t properties:";
  for (const auto &int_it : args.int_properties()) {
    SLOG(Service, 5) << "   " << int_it.first;
    Error get_error;
    int32_t value;
    if (!store_.GetInt32Property(int_it.first, &value, &get_error) ||
        value != int_it.second) {
      return false;
    }
  }
  SLOG(Service, 5) << "Checking string properties:";
  for (const auto &string_it : args.string_properties()) {
    SLOG(Service, 5) << "   " << string_it.first;
    Error get_error;
    string value;
    if (!store_.GetStringProperty(string_it.first, &value, &get_error) ||
        value != string_it.second) {
      return false;
    }
  }
  SLOG(Service, 5) << "Checking string array properties:";
  for (const auto &strings_it : args.strings_properties()) {
    SLOG(Service, 5) << "   " << strings_it.first;
    Error get_error;
    vector<string> value;
    if (!store_.GetStringsProperty(strings_it.first, &value, &get_error) ||
        value != strings_it.second) {
      return false;
    }
  }
  return true;
}

bool Service::IsRemembered() const {
  return profile_ && !manager_->IsServiceEphemeral(this);
}

bool Service::IsDependentOn(const ServiceRefPtr &b) const {
  if (!connection_ || !b || !b->connection()) {
    return false;
  }
  return connection_->GetLowerConnection() == b->connection();
}

void Service::EnableAndRetainAutoConnect() {
  if (retain_auto_connect_) {
    // We do not want to clobber the value of auto_connect_ (it may
    // be user-set). So return early.
    return;
  }

  SetAutoConnect(true);
  RetainAutoConnect();
}

void Service::SetConnection(const ConnectionRefPtr &connection) {
  if (connection.get()) {
    // TODO(pstew): Make this function testable by using a factory here.
    // http://crbug.com/216664
    http_proxy_.reset(new HTTPProxy(connection));
    http_proxy_->Start(dispatcher_, sockets_.get());
    Error unused_error;
    connection->set_tethering(GetTethering(&unused_error));
  } else {
    http_proxy_.reset();
    static_ip_parameters_.ClearSavedParameters();
  }
  connection_ = connection;
  NotifyIPConfigChanges();
}

void Service::NotifyIPConfigChanges() {
  Error error;
  string ipconfig = GetIPConfigRpcIdentifier(&error);
  if (error.IsSuccess()) {
    adaptor_->EmitRpcIdentifierChanged(kIPConfigProperty, ipconfig);
  }
}

bool Service::Is8021xConnectable() const {
  return eap() && eap()->IsConnectable();
}

bool Service::AddEAPCertification(const string &name, size_t depth) {
  if (depth >= kEAPMaxCertificationElements) {
    LOG(WARNING) << "Ignoring certification " << name
                 << " because depth " << depth
                 << " exceeds our maximum of "
                 << kEAPMaxCertificationElements;
    return false;
  }

  if (depth >= remote_certification_.size()) {
    remote_certification_.resize(depth + 1);
  } else if (name == remote_certification_[depth]) {
    return true;
  }

  remote_certification_[depth] = name;
  LOG(INFO) << "Received certification for "
            << name
            << " at depth "
            << depth;
  return true;
}

void Service::ClearEAPCertification() {
  remote_certification_.clear();
}

bool Service::HasStaticIPAddress() const {
  return static_ip_parameters().ContainsAddress();
}

void Service::SetAutoConnect(bool connect) {
  if (auto_connect() == connect) {
    return;
  }
  auto_connect_ = connect;
  adaptor_->EmitBoolChanged(kAutoConnectProperty, auto_connect());
}

void Service::SetEapCredentials(EapCredentials *eap) {
  // This operation must be done at most once for the lifetime of the service.
  CHECK(eap && !eap_);

  eap_.reset(eap);
  eap_->InitPropertyStore(mutable_store());
}

// static
const char *Service::ConnectFailureToString(const ConnectFailure &state) {
  switch (state) {
    case kFailureUnknown:
      return "Unknown";
    case kFailureAAA:
      return kErrorAaaFailed;
    case kFailureActivation:
      return kErrorActivationFailed;
    case kFailureBadPassphrase:
      return kErrorBadPassphrase;
    case kFailureBadWEPKey:
      return kErrorBadWEPKey;
    case kFailureConnect:
      return kErrorConnectFailed;
    case kFailureDNSLookup:
      return kErrorDNSLookupFailed;
    case kFailureDHCP:
      return kErrorDhcpFailed;
    case kFailureEAPAuthentication:
      return kErrorEapAuthenticationFailed;
    case kFailureEAPLocalTLS:
      return kErrorEapLocalTlsFailed;
    case kFailureEAPRemoteTLS:
      return kErrorEapRemoteTlsFailed;
    case kFailureHTTPGet:
      return kErrorHTTPGetFailed;
    case kFailureInternal:
      return kErrorInternal;
    case kFailureIPSecCertAuth:
      return kErrorIpsecCertAuthFailed;
    case kFailureIPSecPSKAuth:
      return kErrorIpsecPskAuthFailed;
    case kFailureNeedEVDO:
      return kErrorNeedEvdo;
    case kFailureNeedHomeNetwork:
      return kErrorNeedHomeNetwork;
    case kFailureOTASP:
      return kErrorOtaspFailed;
    case kFailureOutOfRange:
      return kErrorOutOfRange;
    case kFailurePinMissing:
      return kErrorPinMissing;
    case kFailurePPPAuth:
      return kErrorPppAuthFailed;
    case kFailureMax:
      NOTREACHED();
  }
  return "Invalid";
}

// static
const char *Service::ConnectStateToString(const ConnectState &state) {
  switch (state) {
    case kStateUnknown:
      return "Unknown";
    case kStateIdle:
      return "Idle";
    case kStateAssociating:
      return "Associating";
    case kStateConfiguring:
      return "Configuring";
    case kStateConnected:
      return "Connected";
    case kStatePortal:
      return "Portal";
    case kStateFailure:
      return "Failure";
    case kStateOnline:
      return "Online";
  }
  return "Invalid";
}

string Service::GetTechnologyString() const {
  return Technology::NameFromIdentifier(technology());
}

string Service::CalculateTechnology(Error */*error*/) {
  return GetTechnologyString();
}

// static
void Service::ExpireEventsBefore(
  int seconds_ago, const Timestamp &now, std::deque<Timestamp> *events) {
  struct timeval period = (const struct timeval){ seconds_ago };
  while (!events->empty()) {
    if (events->size() < static_cast<size_t>(kMaxDisconnectEventHistory)) {
      struct timeval elapsed = {0, 0};
      timersub(&now.monotonic, &events->front().monotonic, &elapsed);
      if (timercmp(&elapsed, &period, <)) {
        break;
      }
    }
    events->pop_front();
  }
}

void Service::NoteDisconnectEvent() {
  SLOG(Service, 2) << __func__;
  // Ignore the event if it's user-initiated explicit disconnect.
  if (explicitly_disconnected_) {
    SLOG(Service, 2) << "Explicit disconnect ignored.";
    return;
  }
  // Ignore the event if manager is not running (e.g., service disconnects on
  // shutdown).
  if (!manager_->running()) {
    SLOG(Service, 2) << "Disconnect while manager stopped ignored.";
    return;
  }
  // Ignore the event if the system is suspending.
  PowerManager *power_manager = manager_->power_manager();
  if (!power_manager || power_manager->suspending()) {
    SLOG(Service, 2) << "Disconnect in transitional power state ignored.";
    return;
  }
  int period = 0;
  size_t threshold = 0;
  deque<Timestamp> *events = NULL;
  // Sometimes services transition to Idle before going into a failed state so
  // take into account the last non-idle state.
  ConnectState state = state_ == kStateIdle ? previous_state_ : state_;
  if (IsConnectedState(state)) {
    LOG(INFO) << "Noting an unexpected connection drop.";
    period = kDisconnectsMonitorSeconds;
    threshold = kReportDisconnectsThreshold;
    events = &disconnects_;
  } else if (IsConnectingState(state)) {
    LOG(INFO) << "Noting an unexpected failure to connect.";
    period = kMisconnectsMonitorSeconds;
    threshold = kReportMisconnectsThreshold;
    events = &misconnects_;
  } else {
    SLOG(Service, 2)
        << "Not connected or connecting, state transition ignored.";
    return;
  }
  Timestamp now = time_->GetNow();
  // Discard old events first.
  ExpireEventsBefore(period, now, events);
  events->push_back(now);
  if (events->size() >= threshold) {
    diagnostics_reporter_->OnConnectivityEvent();
  }
}

void Service::ReportUserInitiatedConnectionResult(ConnectState state) {
  // Report stats for wifi only for now.
  if (technology_ != Technology::kWifi)
    return;

  int result;
  switch (state) {
    case kStateConnected:
      result = Metrics::kUserInitiatedConnectionResultSuccess;
      break;
    case kStateFailure:
      result = Metrics::kUserInitiatedConnectionResultFailure;
      metrics_->NotifyUserInitiatedConnectionFailureReason(
          Metrics::kMetricWifiUserInitiatedConnectionFailureReason, failure_);
      break;
    case kStateIdle:
      // This assumes the device specific class (wifi, cellular) will advance
      // the service's state from idle to other state after connection attempt
      // is initiated for the given service.
      result = Metrics::kUserInitiatedConnectionResultAborted;
      break;
    default:
      return;
  }

  metrics_->NotifyUserInitiatedConnectionResult(
      Metrics::kMetricWifiUserInitiatedConnectionResult, result);
}

bool Service::HasRecentConnectionIssues() {
  Timestamp now = time_->GetNow();
  ExpireEventsBefore(kDisconnectsMonitorSeconds, now, &disconnects_);
  ExpireEventsBefore(kMisconnectsMonitorSeconds, now, &misconnects_);
  return !disconnects_.empty() || !misconnects_.empty();
}

// static
bool Service::DecideBetween(int a, int b, bool *decision) {
  if (a == b)
    return false;
  *decision = (a > b);
  return true;
}

uint16_t Service::SecurityLevel() {
  return (crypto_algorithm_ << 2) | (key_rotation_ << 1) | endpoint_auth_;
}

// static
bool Service::Compare(Manager *manager,
                      ServiceRefPtr a,
                      ServiceRefPtr b,
                      bool compare_connectivity_state,
                      const vector<Technology::Identifier> &tech_order,
                      const char **reason) {
  bool ret;

  if (compare_connectivity_state && a->state() != b->state()) {
    if (DecideBetween(a->IsConnected(), b->IsConnected(), &ret)) {
      *reason = kServiceSortIsConnected;
      return ret;
    }

    if (DecideBetween(!a->IsPortalled(), !b->IsPortalled(), &ret)) {
      *reason = kServiceSortIsPortalled;
      return ret;
    }

    if (DecideBetween(a->IsConnecting(), b->IsConnecting(), &ret)) {
      *reason = kServiceSortIsConnecting;
      return ret;
    }

    if (DecideBetween(!a->IsFailed(), !b->IsFailed(), &ret)) {
      *reason = kServiceSortIsFailed;
      return ret;
    }
  }

  if (DecideBetween(a->connectable(), b->connectable(), &ret)) {
    *reason = kServiceSortConnectable;
    return ret;
  }

  if (DecideBetween(a->IsDependentOn(b), b->IsDependentOn(a), &ret)) {
    *reason = kServiceSortDependency;
    return ret;
  }

  // Ignore the auto-connect property if both services are connected
  // already. This allows connected non-autoconnectable VPN services to be
  // sorted higher than other connected services based on technology order.
  if (!a->IsConnected() &&
      DecideBetween(a->auto_connect(), b->auto_connect(), &ret)) {
    *reason = kServiceSortAutoConnect;
    return ret;
  }

  if (DecideBetween(a->has_ever_connected(), b->has_ever_connected(), &ret)) {
    *reason = kServiceSortHasEverConnected;
    return ret;
  }

  if (DecideBetween(a->priority(), b->priority(), &ret)) {
    *reason = kServiceSortPriority;
    return ret;
  }

  // TODO(pstew): Below this point we are making value judgements on
  // services that are not related to anything intrinsic or
  // user-specified.  These heuristics should be richer (contain
  // historical information, for example) and be subject to user
  // customization.
  for (vector<Technology::Identifier>::const_iterator it = tech_order.begin();
       it != tech_order.end();
       ++it) {
    if (DecideBetween(a->technology() == *it, b->technology() == *it, &ret)) {
      *reason = kServiceSortTechnology;
      return ret;
    }
  }

  if (DecideBetween(a->SecurityLevel(), b->SecurityLevel(), &ret)) {
    *reason = kServiceSortSecurity;
    return ret;
  }

  // If the profiles for the two services are different,
  // we want to pick the highest priority one.  The
  // ephemeral profile is explicitly tested for since it is not
  // listed in the manager profiles_ list.
  if (a->profile() != b->profile()) {
    *reason = kServiceSortProfileOrder;
    if (manager->IsServiceEphemeral(b)) {
      return true;
    } else if (manager->IsServiceEphemeral(a)) {
      return false;
    } else if (manager->IsProfileBefore(b->profile(), a->profile())) {
      return true;
    } else {
      return false;
    }
  }

  if (DecideBetween(a->strength(), b->strength(), &ret)) {
    *reason = kServiceSortEtc;
    return ret;
  }

  *reason = kServiceSortSerialNumber;
  return a->serial_number_ < b->serial_number_;
}

const ProfileRefPtr &Service::profile() const { return profile_; }

void Service::set_profile(const ProfileRefPtr &p) { profile_ = p; }

void Service::SetProfile(const ProfileRefPtr &p) {
  SLOG(Service, 2) << "SetProfile from "
                   << (profile_ ? profile_->GetFriendlyName() : "(none)")
                   << " to " << (p ? p->GetFriendlyName() : "(none)")
                   << ".";
  if (profile_ == p) {
    return;
  }
  profile_ = p;
  Error error;
  string profile_rpc_id = GetProfileRpcId(&error);
  if (!error.IsSuccess()) {
    return;
  }
  adaptor_->EmitStringChanged(kProfileProperty, profile_rpc_id);
}

void Service::OnPropertyChanged(const string &property) {
  SLOG(Service, 1) << __func__ << " " << property;
  if (Is8021x() && EapCredentials::IsEapAuthenticationProperty(property)) {
    OnEapCredentialsChanged();
  }
  SaveToProfile();
  if ((property == kCheckPortalProperty ||
       property == kProxyConfigProperty) &&
      (state_ == kStateConnected ||
       state_ == kStatePortal ||
       state_ == kStateOnline)) {
    manager_->RecheckPortalOnService(this);
  }
}

void Service::OnAfterResume() {
  // Forget old autoconnect failures across suspend/resume.
  auto_connect_cooldown_milliseconds_  = 0;
  reenable_auto_connect_task_.Cancel();
  // Forget if the user disconnected us, we might be able to connect now.
  ClearExplicitlyDisconnected();
}

string Service::GetIPConfigRpcIdentifier(Error *error) const {
  if (!connection_) {
    error->Populate(Error::kNotFound);
    return DBusAdaptor::kNullPath;
  }

  string id = connection_->ipconfig_rpc_identifier();

  if (id.empty()) {
    // Do not return an empty IPConfig.
    error->Populate(Error::kNotFound);
    return DBusAdaptor::kNullPath;
  }

  return id;
}

void Service::SetConnectable(bool connectable) {
  if (connectable_ == connectable)
    return;
  connectable_ = connectable;
  adaptor_->EmitBoolChanged(kConnectableProperty, connectable_);
}

void Service::SetConnectableFull(bool connectable) {
  if (connectable_ == connectable) {
    return;
  }
  SetConnectable(connectable);
  if (manager_->HasService(this)) {
    manager_->UpdateService(this);
  }
}

string Service::GetStateString() const {
  // TODO(benchan): We may want to rename shill::kState* to avoid name clashing
  // with Service::kState*.
  switch (state()) {
    case kStateIdle:
      return shill::kStateIdle;
    case kStateAssociating:
      return shill::kStateAssociation;
    case kStateConfiguring:
      return shill::kStateConfiguration;
    case kStateConnected:
      return shill::kStateReady;
    case kStateFailure:
      return shill::kStateFailure;
    case kStatePortal:
      return shill::kStatePortal;
    case kStateOnline:
      return shill::kStateOnline;
    case kStateUnknown:
    default:
      return "";
  }
}

string Service::CalculateState(Error */*error*/) {
  return GetStateString();
}

bool Service::IsAutoConnectable(const char **reason) const {
  if (!connectable()) {
    *reason = kAutoConnNotConnectable;
    return false;
  }

  if (IsConnected()) {
    *reason = kAutoConnConnected;
    return false;
  }

  if (IsConnecting()) {
    *reason = kAutoConnConnecting;
    return false;
  }

  if (explicitly_disconnected_) {
    *reason = kAutoConnExplicitDisconnect;
    return false;
  }

  if (!reenable_auto_connect_task_.IsCancelled()) {
    *reason = kAutoConnThrottled;
    return false;
  }

  if (!Technology::IsPrimaryConnectivityTechnology(technology_) &&
      !manager_->IsConnected()) {
    *reason = kAutoConnOffline;
    return false;
  }

  return true;
}

bool Service::IsPortalDetectionDisabled() const {
  return check_portal_ == kCheckPortalFalse;
}

bool Service::IsPortalDetectionAuto() const {
  return check_portal_ == kCheckPortalAuto;
}

void Service::HelpRegisterDerivedBool(
    const string &name,
    bool(Service::*get)(Error *error),
    bool(Service::*set)(const bool&, Error *),
    void(Service::*clear)(Error *)) {
  store_.RegisterDerivedBool(
      name,
      BoolAccessor(new CustomAccessor<Service, bool>(this, get, set, clear)));
}

void Service::HelpRegisterDerivedInt32(
    const string &name,
    int32_t(Service::*get)(Error *error),
    bool(Service::*set)(const int32_t&, Error *)) {
  store_.RegisterDerivedInt32(
      name,
      Int32Accessor(new CustomAccessor<Service, int32_t>(this, get, set)));
}

void Service::HelpRegisterDerivedString(
    const string &name,
    string(Service::*get)(Error *error),
    bool(Service::*set)(const string&, Error *)) {
  store_.RegisterDerivedString(
      name,
      StringAccessor(new CustomAccessor<Service, string>(this, get, set)));
}

void Service::HelpRegisterConstDerivedRpcIdentifier(
    const string &name,
    RpcIdentifier(Service::*get)(Error *) const) {
  store_.RegisterDerivedRpcIdentifier(
      name,
      RpcIdentifierAccessor(new CustomReadOnlyAccessor<Service, RpcIdentifier>(
          this, get)));
}

void Service::HelpRegisterConstDerivedUint16(
    const string &name,
    uint16_t(Service::*get)(Error *) const) {
  store_.RegisterDerivedUint16(
      name,
      Uint16Accessor(new CustomReadOnlyAccessor<Service, uint16_t>(this, get)));
}

void Service::HelpRegisterConstDerivedStrings(
    const string &name, Strings(Service::*get)(Error *error) const) {
  store_.RegisterDerivedStrings(
      name,
      StringsAccessor(new CustomReadOnlyAccessor<Service, Strings>(this, get)));
}

void Service::HelpRegisterConstDerivedString(
    const string &name, string(Service::*get)(Error *error) const) {
  store_.RegisterDerivedString(
      name,
      StringAccessor(new CustomReadOnlyAccessor<Service, string>(this, get)));
}

void Service::HelpRegisterObservedDerivedBool(
    const string &name,
    bool(Service::*get)(Error *error),
    bool(Service::*set)(const bool&, Error *),
    void(Service::*clear)(Error *)) {
  BoolAccessor accessor(
      new CustomAccessor<Service, bool>(this, get, set, clear));
  store_.RegisterDerivedBool(name, accessor);
  property_change_notifier_->AddBoolPropertyObserver(name, accessor);
}

// static
void Service::LoadString(StoreInterface *storage,
                         const string &id,
                         const string &key,
                         const string &default_value,
                         string *value) {
  if (!storage->GetString(id, key, value)) {
    *value = default_value;
  }
}

// static
void Service::SaveString(StoreInterface *storage,
                         const string &id,
                         const string &key,
                         const string &value,
                         bool crypted,
                         bool save) {
  if (value.empty() || !save) {
    storage->DeleteKey(id, key);
    return;
  }
  if (crypted) {
    storage->SetCryptedString(id, key, value);
    return;
  }
  storage->SetString(id, key, value);
}

map<string, string> Service::GetLoadableProfileEntries() {
  return manager_->GetLoadableProfileEntriesForService(this);
}

void Service::IgnoreParameterForConfigure(const string &parameter) {
  parameters_ignored_for_configure_.insert(parameter);
}

const string &Service::GetEAPKeyManagement() const {
  CHECK(eap());
  return eap()->key_management();
}

void Service::SetEAPKeyManagement(const string &key_management) {
  CHECK(mutable_eap());
  mutable_eap()->SetKeyManagement(key_management, NULL);
}

bool Service::GetAutoConnect(Error */*error*/) {
  return auto_connect();
}

bool Service::SetAutoConnectFull(const bool &connect, Error */*error*/) {
  LOG(INFO) << "Service " << unique_name() << ": AutoConnect="
            << auto_connect() << "->" << connect;
  if (!retain_auto_connect_) {
    RetainAutoConnect();
    // Irrespective of an actual change in the |kAutoConnectPropety|, we must
    // flush the current value of the property to the profile.
    if (IsRemembered()) {
      SaveToProfile();
    }
  }

  if (auto_connect() == connect) {
    return false;
  }

  SetAutoConnect(connect);
  manager_->UpdateService(this);
  return true;
}

void Service::ClearAutoConnect(Error */*error*/) {
  if (auto_connect()) {
    SetAutoConnect(false);
    manager_->UpdateService(this);
  }

  retain_auto_connect_ = false;
}

string Service::GetCheckPortal(Error *error) {
  return check_portal_;
}

bool Service::SetCheckPortal(const string &check_portal, Error *error) {
  if (check_portal != kCheckPortalFalse &&
      check_portal != kCheckPortalTrue &&
      check_portal != kCheckPortalAuto) {
    Error::PopulateAndLog(error, Error::kInvalidArguments,
                          base::StringPrintf(
                              "Invalid Service CheckPortal property value: %s",
                              check_portal.c_str()));
    return false;
  }
  if (check_portal == check_portal_) {
    return false;
  }
  check_portal_ = check_portal;
  return true;
}

string Service::GetGuid(Error *error) {
  return guid_;
}

bool Service::SetGuid(const string &guid, Error */*error*/) {
  if (guid_ == guid) {
    return false;
  }
  guid_ = guid;
  adaptor_->EmitStringChanged(kGuidProperty, guid_);
  return true;
}

void Service::RetainAutoConnect() {
  retain_auto_connect_ = true;
}

void Service::SetSecurity(CryptoAlgorithm crypto_algorithm, bool key_rotation,
                          bool endpoint_auth) {
  crypto_algorithm_ = crypto_algorithm;
  key_rotation_ = key_rotation;
  endpoint_auth_ = endpoint_auth;
}

string Service::GetNameProperty(Error */*error*/) {
  return friendly_name_;
}

bool Service::SetNameProperty(const string &name, Error *error) {
  if (name != friendly_name_) {
    Error::PopulateAndLog(error, Error::kInvalidArguments,
                          base::StringPrintf(
                              "Service %s Name property cannot be modified.",
                              unique_name_.c_str()));
    return false;
  }
  return false;
}

void Service::SetHasEverConnected(bool has_ever_connected) {
  if (has_ever_connected_ == has_ever_connected)
    return;
  has_ever_connected_ = has_ever_connected;
  SaveToProfile();
}

int32_t Service::GetPriority(Error *error) {
  return priority_;
}

bool Service::SetPriority(const int32_t &priority, Error *error) {
  if (priority_ == priority) {
    return false;
  }
  priority_ = priority;
  adaptor_->EmitIntChanged(kPriorityProperty, priority_);
  return true;
}

string Service::GetProfileRpcId(Error *error) {
  if (!profile_) {
    // This happens in some unit tests where profile_ is not set.
    error->Populate(Error::kNotFound);
    return "";
  }
  return profile_->GetRpcIdentifier();
}

bool Service::SetProfileRpcId(const string &profile, Error *error) {
  if (profile_ && profile_->GetRpcIdentifier() == profile) {
    return false;
  }
  ProfileConstRefPtr old_profile = profile_;
  // No need to Emit afterwards, since SetProfileForService will call
  // into SetProfile (if the profile actually changes).
  manager_->SetProfileForService(this, profile, error);
  // Can't just use error.IsSuccess(), because that also requires saving
  // the profile to succeed. (See Profile::AdoptService)
  return (profile_ != old_profile);
}

uint16_t Service::GetHTTPProxyPort(Error */*error*/) const {
  if (http_proxy_.get()) {
    return static_cast<uint16_t>(http_proxy_->proxy_port());
  }
  return 0;
}

string Service::GetProxyConfig(Error *error) {
  return proxy_config_;
}

bool Service::SetProxyConfig(const string &proxy_config, Error *error) {
  if (proxy_config_ == proxy_config)
    return false;
  proxy_config_ = proxy_config;
  adaptor_->EmitStringChanged(kProxyConfigProperty, proxy_config_);
  return true;
}

string Service::GetTethering(Error *error) const {
  // The "Tethering" property isn't supported by the Service base class, and
  // therefore should not be listed in the properties returned by
  // the GetProperties() RPC method.
  error->Populate(Error::kNotSupported);
  return "";
}


void Service::NotifyPropertyChanges() {
  property_change_notifier_->UpdatePropertyObservers();
}

// static
Strings Service::ExtractWallClockToStrings(
    const deque<Timestamp> &timestamps) {
  Strings strings;
  for (deque<Timestamp>::const_iterator it = timestamps.begin();
       it != timestamps.end(); ++it) {
    strings.push_back(it->wall_clock);
  }
  return strings;
}

Strings Service::GetDisconnectsProperty(Error */*error*/) const {
  return ExtractWallClockToStrings(disconnects_);
}

Strings Service::GetMisconnectsProperty(Error */*error*/) const {
  return ExtractWallClockToStrings(misconnects_);
}

bool Service::GetVisibleProperty(Error */*error*/) {
  return IsVisible();
}

void Service::SaveToProfile() {
  if (profile_.get() && profile_->GetConstStorage()) {
    profile_->UpdateService(this);
  }
}

void Service::SetFriendlyName(const string &friendly_name) {
  if (friendly_name == friendly_name_)
    return;
  friendly_name_ = friendly_name;
  adaptor()->EmitStringChanged(kNameProperty, friendly_name_);
}

void Service::SetStrength(uint8_t strength) {
  if (strength == strength_) {
    return;
  }
  strength_ = strength;
  adaptor_->EmitUint8Changed(kSignalStrengthProperty, strength);
}

void Service::SetErrorDetails(const string &details) {
  if (error_details_ == details) {
    return;
  }
  error_details_ = details;
  adaptor_->EmitStringChanged(kErrorDetailsProperty, error_details_);
}

void Service::UpdateErrorProperty() {
  const string error(ConnectFailureToString(failure_));
  if (error == error_) {
    return;
  }
  error_ = error;
  adaptor_->EmitStringChanged(kErrorProperty, error);
}

void Service::ClearExplicitlyDisconnected() {
  if (explicitly_disconnected_) {
    explicitly_disconnected_ = false;
    manager_->UpdateService(this);
  }
}

}  // namespace shill
