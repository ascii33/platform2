// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apmanager/shill_proxy.h"

#include <chromeos/dbus/service_constants.h>
#include <chromeos/errors/error.h>

using std::string;

namespace apmanager {

// static.
const char ShillProxy::kManagerPath[] = "/";

ShillProxy::ShillProxy() {}

ShillProxy::~ShillProxy() {}

void ShillProxy::Init(const scoped_refptr<dbus::Bus>& bus) {
  CHECK(!manager_proxy_) << "Already init";
  manager_proxy_.reset(
      new org::chromium::flimflam::ManagerProxy(
          bus, shill::kFlimflamServiceName, dbus::ObjectPath(kManagerPath)));
}

void ShillProxy::ClaimInterface(const string& interface_name) {
  CHECK(manager_proxy_) << "Proxy not initialize yet";
  chromeos::ErrorPtr error;
  if (!manager_proxy_->ClaimInterface(kServiceName, interface_name, &error)) {
    // Ignore unknown object error (when shill is not running). Only report
    // internal error from shill.
    if (error->GetCode() != DBUS_ERROR_UNKNOWN_OBJECT) {
      LOG(ERROR) << "Failed to claim interface from shill: "
                 << error->GetCode() << " " << error->GetMessage();
    }
  }
  claimed_interfaces_.insert(interface_name);
}

void ShillProxy::ReleaseInterface(const string& interface_name) {
  CHECK(manager_proxy_) << "Proxy not initialize yet";
  chromeos::ErrorPtr error;
  if (!manager_proxy_->ReleaseInterface(interface_name, &error)) {
    // Ignore unknown object error (when shill is not running). Only report
    // internal error from shill.
    if (error->GetCode() != DBUS_ERROR_UNKNOWN_OBJECT) {
      LOG(ERROR) << "Failed to release interface from shill: "
                 << error->GetCode() << " " << error->GetMessage();
    }
  }
  claimed_interfaces_.erase(interface_name);
}

}  // namespace apmanager
