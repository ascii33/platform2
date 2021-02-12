// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sysexits.h>

#include "typecd/daemon.h"

namespace typecd {

Daemon::Daemon()
    : udev_monitor_(new UdevMonitor()),
      port_manager_(new PortManager()),
      weak_factory_(this) {}

Daemon::~Daemon() {}

int Daemon::OnInit() {
  int exit_code = DBusDaemon::OnInit();
  if (exit_code != EX_OK)
    return exit_code;

  LOG(INFO) << "Daemon started.";
  if (!udev_monitor_->InitUdev()) {
    LOG(ERROR) << "udev init failed.";
    return -1;
  }

  // Register the session_manager proxy.
  session_manager_proxy_ = std::make_unique<SessionManagerProxy>(bus_);

  cros_ec_util_ = std::make_unique<CrosECUtil>(bus_);
  port_manager_->SetECUtil(cros_ec_util_.get());

  // Stash whether mode entry is supported at init, instead of querying it
  // repeatedly.
  bool mode_entry_supported = cros_ec_util_->ModeEntrySupported();
  if (!mode_entry_supported)
    LOG(INFO) << "Mode entry not supported on this device.";
  port_manager_->SetModeEntrySupported(mode_entry_supported);

  InitUserActiveState();
  session_manager_proxy_->AddObserver(port_manager_.get());

  // Add any observers to |udev_monitor_| here.
  udev_monitor_->AddObserver(port_manager_.get());

  udev_monitor_->ScanDevices();
  udev_monitor_->BeginMonitoring();

  return 0;
}

void Daemon::InitUserActiveState() {
  bool active = !session_manager_proxy_->IsScreenLocked() &&
                session_manager_proxy_->IsSessionStarted();

  port_manager_->SetUserActive(active);
}

}  // namespace typecd
