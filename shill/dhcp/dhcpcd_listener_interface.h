// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_DHCP_DHCPCD_LISTENER_INTERFACE_H_
#define SHILL_DHCP_DHCPCD_LISTENER_INTERFACE_H_


namespace shill {

class DHCPCDListenerInterface {
 public:
  virtual ~DHCPCDListenerInterface() {}
};

}  // namespace shill

#endif  // SHILL_DHCP_DHCPCD_LISTENER_INTERFACE_H_
