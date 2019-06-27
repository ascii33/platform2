// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vm_tools/concierge/vm_util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utility>

#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/stringprintf.h>
#include <base/sys_info.h>
#include <brillo/process.h>

namespace vm_tools {
namespace concierge {
namespace {

// Examples of the format of the given string can be seen at the enum
// UsbControlResponseType definition.
bool ParseUsbControlResponse(base::StringPiece s,
                             UsbControlResponse* response) {
  s = base::TrimString(s, base::kWhitespaceASCII, base::TRIM_ALL);

  if (s.starts_with("ok ")) {
    response->type = OK;
    unsigned port;
    if (!base::StringToUint(s.substr(3), &port))
      return false;
    if (port > UINT8_MAX) {
      return false;
    }
    response->port = port;
    return true;
  }

  if (s.starts_with("no_available_port")) {
    response->type = NO_AVAILABLE_PORT;
    response->reason = "No available ports in guest's host controller.";
    return true;
  }
  if (s.starts_with("no_such_device")) {
    response->type = NO_SUCH_DEVICE;
    response->reason = "No such host device.";
    return true;
  }
  if (s.starts_with("no_such_port")) {
    response->type = NO_SUCH_PORT;
    response->reason = "No such port in guest's host controller.";
    return true;
  }
  if (s.starts_with("fail_to_open_device")) {
    response->type = FAIL_TO_OPEN_DEVICE;
    response->reason = "Failed to open host device.";
    return true;
  }
  if (s.starts_with("devices")) {
    std::vector<base::StringPiece> device_parts = base::SplitStringPiece(
        s.substr(7), " \t", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if ((device_parts.size() % 3) != 0) {
      return false;
    }
    response->type = DEVICES;
    for (size_t i = 0; i < device_parts.size(); i += 3) {
      unsigned port;
      unsigned vid;
      unsigned pid;
      if (!(base::StringToUint(device_parts[0], &port) &&
            base::HexStringToUInt(device_parts[1], &vid) &&
            base::HexStringToUInt(device_parts[2], &pid))) {
        return false;
      }
      if (port > UINT8_MAX || vid > UINT16_MAX || pid > UINT16_MAX) {
        return false;
      }
      UsbDevice device;
      device.port = port;
      device.vid = vid;
      device.pid = pid;
      response->devices.push_back(device);
    }
    return true;
  }
  if (s.starts_with("error ")) {
    response->type = ERROR;
    response->reason = s.substr(6).as_string();
    return true;
  }

  return false;
}

bool CallUsbControl(brillo::ProcessImpl crosvm, UsbControlResponse* response) {
  crosvm.RedirectUsingPipe(STDOUT_FILENO, false /* is_input */);
  int ret = crosvm.Run();
  if (ret != 0)
    LOG(ERROR) << "Failed crosvm call returned code " << ret;

  base::ScopedFD read_fd(crosvm.GetPipe(STDOUT_FILENO));
  std::string crosvm_response;
  crosvm_response.resize(2048);

  ssize_t response_size =
      read(read_fd.get(), &crosvm_response[0], crosvm_response.size());
  if (response_size < 0) {
    response->reason = "Failed to read USB response from crosvm";
    return false;
  }
  if (response_size == 0) {
    response->reason = "Empty USB response from crosvm";
    return false;
  }
  crosvm_response.resize(response_size);

  if (!ParseUsbControlResponse(crosvm_response, response)) {
    response->reason =
        "Failed to parse USB response from crosvm: " + crosvm_response;
    return false;
  }
  return true;
}

}  // namespace

std::string GetVmMemoryMiB() {
  int64_t vm_memory_mb = base::SysInfo::AmountOfPhysicalMemoryMB();
  vm_memory_mb /= 4;
  vm_memory_mb *= 3;

  return std::to_string(vm_memory_mb);
}

bool SetPgid() {
  if (setpgid(0, 0) != 0) {
    PLOG(ERROR) << "Failed to change process group id";
    return false;
  }

  return true;
}

bool WaitForChild(pid_t child, base::TimeDelta timeout) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGCHLD);

  const base::Time deadline = base::Time::Now() + timeout;
  while (true) {
    pid_t ret = waitpid(child, nullptr, WNOHANG);
    if (ret == child || (ret < 0 && errno == ECHILD)) {
      // Either the child exited or it doesn't exist anymore.
      return true;
    }

    // ret == 0 means that the child is still alive
    if (ret < 0) {
      PLOG(ERROR) << "Failed to wait for child process";
      return false;
    }

    base::Time now = base::Time::Now();
    if (deadline <= now) {
      // Timed out.
      return false;
    }

    const struct timespec ts = (deadline - now).ToTimeSpec();
    if (sigtimedwait(&set, nullptr, &ts) < 0 && errno == EAGAIN) {
      // Timed out.
      return false;
    }
  }
}

bool CheckProcessExists(pid_t pid) {
  // kill() with a signal value of 0 is explicitly documented as a way to
  // check for the existence of a process.
  return pid != 0 && (kill(pid, 0) >= 0 || errno != ESRCH);
}

void RunCrosvmCommand(std::string command, std::string socket_path) {
  constexpr char kCrosvmBin[] = "/usr/bin/crosvm";

  brillo::ProcessImpl crosvm;
  crosvm.AddArg(kCrosvmBin);
  crosvm.AddArg(std::move(command));
  crosvm.AddArg(std::move(socket_path));

  // This must be synchronous as we may do things after calling this function
  // that depend on the crosvm command being completed (like suspending the
  // device).
  crosvm.Run();
}

bool AttachUsbDevice(std::string socket_path,
                     uint8_t bus,
                     uint8_t addr,
                     uint16_t vid,
                     uint16_t pid,
                     int fd,
                     UsbControlResponse* response) {
  brillo::ProcessImpl crosvm;
  crosvm.AddArg(kCrosvmBin);
  crosvm.AddArg("usb");
  crosvm.AddArg("attach");
  crosvm.AddArg(base::StringPrintf("%d:%d:%x:%x", bus, addr, vid, pid));
  crosvm.AddArg("/proc/self/fd/" + std::to_string(fd));
  crosvm.AddArg(std::move(socket_path));
  crosvm.BindFd(fd, fd);
  fcntl(fd, F_SETFD, 0);  // Remove the CLOEXEC

  CallUsbControl(std::move(crosvm), response);

  return response->type == OK;
}

bool DetachUsbDevice(std::string socket_path,
                     uint8_t port,
                     UsbControlResponse* response) {
  brillo::ProcessImpl crosvm;
  crosvm.AddArg(kCrosvmBin);
  crosvm.AddArg("usb");
  crosvm.AddArg("detach");
  crosvm.AddArg(std::to_string(port));
  crosvm.AddArg(std::move(socket_path));

  CallUsbControl(std::move(crosvm), response);

  return response->type == OK;
}

bool ListUsbDevice(std::string socket_path, std::vector<UsbDevice>* device) {
  brillo::ProcessImpl crosvm;
  crosvm.AddArg(kCrosvmBin);
  crosvm.AddArg("usb");
  crosvm.AddArg("list");
  crosvm.AddArg(std::move(socket_path));

  UsbControlResponse response;
  CallUsbControl(std::move(crosvm), &response);

  if (response.type != DEVICES)
    return false;

  *device = std::move(response.devices);

  return true;
}

}  // namespace concierge
}  // namespace vm_tools
