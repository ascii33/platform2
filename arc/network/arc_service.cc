// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "arc/network/arc_service.h"

#include <linux/rtnetlink.h>
#include <net/if.h>

#include <memory>
#include <string>
#include <utility>

#include <base/bind.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <shill/net/rtnl_message.h>

#include "arc/network/datapath.h"
#include "arc/network/ipc.pb.h"
#include "arc/network/mac_address_generator.h"
#include "arc/network/net_util.h"
#include "arc/network/scoped_ns.h"

namespace arc_networkd {
namespace {
constexpr pid_t kInvalidPID = -1;
constexpr pid_t kTestPID = -2;
constexpr int kInvalidTableID = -1;
constexpr int kMaxTableRetries = 10;  // Based on 1 second delay.
constexpr base::TimeDelta kTableRetryDelay = base::TimeDelta::FromSeconds(1);
// Android adds a constant to the interface index to derive the table id.
// This is defined in system/netd/server/RouteController.h
constexpr int kRouteControllerRouteTableOffsetFromIndex = 1000;

// This wrapper is required since the base class is a singleton that hides its
// constructor. It is necessary here because the message loop thread has to be
// reassociated to the container's network namespace; and since the container
// can be repeatedly created and destroyed, the handler must be as well.
class RTNetlinkHandler : public shill::RTNLHandler {
 public:
  RTNetlinkHandler() = default;
  ~RTNetlinkHandler() = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(RTNetlinkHandler);
};

int GetAndroidRoutingTableId(const std::string& ifname, pid_t pid) {
  base::FilePath ifindex_path(base::StringPrintf(
      "/proc/%d/root/sys/class/net/%s/ifindex", pid, ifname.c_str()));
  std::string contents;
  if (!base::ReadFileToString(ifindex_path, &contents)) {
    PLOG(WARNING) << "Could not read " << ifindex_path.value();
    return kInvalidTableID;
  }

  base::TrimWhitespaceASCII(contents, base::TRIM_TRAILING, &contents);
  int table_id = kInvalidTableID;
  if (!base::StringToInt(contents, &table_id)) {
    LOG(ERROR) << "Could not parse ifindex from " << ifindex_path.value()
               << ": " << contents;
    return kInvalidTableID;
  }
  table_id += kRouteControllerRouteTableOffsetFromIndex;

  LOG(INFO) << "Found table id " << table_id << " for container interface "
            << ifname;
  return table_id;
}

// TODO(garrick): Remove this workaround ASAP.
int GetContainerPID() {
  const base::FilePath path("/run/containers/android-run_oci/container.pid");
  std::string pid_str;
  if (!base::ReadFileToStringWithMaxSize(path, &pid_str, 16 /* max size */)) {
    LOG(ERROR) << "Failed to read pid file";
    return kInvalidPID;
  }
  int pid;
  if (!base::StringToInt(base::TrimWhitespaceASCII(pid_str, base::TRIM_ALL),
                         &pid)) {
    LOG(ERROR) << "Failed to convert container pid string";
    return kInvalidPID;
  }
  LOG(INFO) << "Read container pid as " << pid;
  return pid;
}

}  // namespace

ArcService::ArcService(DeviceManagerBase* dev_mgr,
                       bool is_legacy,
                       std::unique_ptr<Datapath> datapath)
    : GuestService(is_legacy ? GuestMessage::ARC_LEGACY : GuestMessage::ARC,
                   dev_mgr),
      pid_(kInvalidPID) {
  if (!datapath) {
    runner_ = std::make_unique<MinijailedProcessRunner>();
    datapath = std::make_unique<Datapath>(runner_.get());
  }

  datapath_ = std::move(datapath);
  dev_mgr_->RegisterDeviceIPv6AddressFoundHandler(
      base::Bind(&ArcService::SetupIPv6, weak_factory_.GetWeakPtr()));
}

void ArcService::OnStart() {
  LOG(INFO) << "ARC++ network service starting";
  pid_ = GetContainerPID();
  if (pid_ == kInvalidPID) {
    LOG(ERROR) << "Cannot start service - invalid container PID";
    return;
  }

  // Start listening for RTNetlink messages in the container's net namespace
  // to be notified whenever it brings up an interface.
  {
    ScopedNS ns(pid_);
    if (ns.IsValid()) {
      rtnl_handler_ = std::make_unique<RTNetlinkHandler>();
      rtnl_handler_->Start(RTMGRP_LINK);
      link_listener_ = std::make_unique<shill::RTNLListener>(
          shill::RTNLHandler::kRequestLink,
          Bind(&ArcService::LinkMsgHandler, weak_factory_.GetWeakPtr()),
          rtnl_handler_.get());
    } else {
      // This is bad - it means we won't ever be able to tell when the container
      // brings up an interface.
      LOG(ERROR)
          << "Cannot start netlink listener - invalid container namespace?";
    }
  }

  // Start known host devices, any new ones will be setup in the process.
  dev_mgr_->ProcessDevices(
      base::Bind(&ArcService::StartDevice, weak_factory_.GetWeakPtr()));

  // If this is the first time the service is starting this will create the
  // Android bridge device; otherwise it does nothing. Do this after processing
  // the existing devices so it doesn't get started twice.
  dev_mgr_->Add(guest_ == GuestMessage::ARC_LEGACY ? kAndroidLegacyDevice
                                                   : kAndroidDevice);

  GuestMessage msg;
  msg.set_event(GuestMessage::START);
  msg.set_arc_pid(pid_);
  msg.set_type(guest_);
  DispatchMessage(msg);

  // Finally, call the base implementation.
  GuestService::OnStart();
}

void ArcService::OnStop() {
  LOG(INFO) << "ARC++ network service stopping";
  // Call the base implementation.
  GuestService::OnStop();

  // Stop known host devices. Note that this does not teardown any existing
  // devices.
  dev_mgr_->ProcessDevices(
      base::Bind(&ArcService::StopDevice, weak_factory_.GetWeakPtr()));

  rtnl_handler_->RemoveListener(link_listener_.get());
  link_listener_.reset();
  rtnl_handler_.reset();

  GuestMessage msg;
  msg.set_event(GuestMessage::STOP);
  msg.set_type(guest_);
  DispatchMessage(msg);

  pid_ = kInvalidPID;
}

void ArcService::OnDeviceAdded(Device* device) {
  if (!ShouldProcessDevice(*device))
    return;

  const auto& config = device->config();

  LOG(INFO) << "Adding device " << device->ifname()
            << " bridge: " << config.host_ifname()
            << " guest_iface: " << config.guest_ifname()
            << " for container pid " << pid_;

  // Create the bridge.
  if (!datapath_->AddBridge(config.host_ifname(),
                            IPv4AddressToString(config.host_ipv4_addr()))) {
    LOG(ERROR) << "Failed to setup arc bridge: " << config.host_ifname();
    return;
  }

  // Setup the iptables.
  if (device->IsLegacyAndroid()) {
    if (!datapath_->AddLegacyIPv4DNAT(
            IPv4AddressToString(config.guest_ipv4_addr())))
      LOG(ERROR) << "Failed to configure ARC traffic rules";

    if (!datapath_->AddOutboundIPv4(config.host_ifname()))
      LOG(ERROR) << "Failed to configure egress traffic rules";
  } else if (!device->IsAndroid()) {
    if (!datapath_->AddInboundIPv4DNAT(
            device->ifname(), IPv4AddressToString(config.guest_ipv4_addr())))
      LOG(ERROR) << "Failed to configure ingress traffic rules for "
                 << device->ifname();

    if (!datapath_->AddOutboundIPv4(config.host_ifname()))
      LOG(ERROR) << "Failed to configure egress traffic rules";
  }

  devices_.insert(config.guest_ifname());

  StartDevice(device);
}

void ArcService::StartDevice(Device* device) {
  if (!ShouldProcessDevice(*device))
    return;

  const auto& config = device->config();

  // If the device is new then it needs to run through the full setup process.
  if (devices_.find(config.guest_ifname()) == devices_.end())
    return OnDeviceAdded(device);

  LOG(INFO) << "Starting device " << device->ifname()
            << " bridge: " << config.host_ifname()
            << " guest_iface: " << config.guest_ifname()
            << " for container pid " << pid_;

  std::string veth_ifname = datapath_->AddVirtualBridgedInterface(
      device->ifname(), MacAddressToString(config.guest_mac_addr()),
      config.host_ifname());
  if (veth_ifname.empty()) {
    LOG(ERROR) << "Failed to create virtual interface for container";
    return;
  }

  if (!datapath_->AddInterfaceToContainer(
          pid_, veth_ifname, config.guest_ifname(),
          IPv4AddressToString(config.guest_ipv4_addr()),
          device->options().fwd_multicast)) {
    LOG(ERROR) << "Failed to create container interface.";
    datapath_->RemoveInterface(veth_ifname);
    datapath_->RemoveBridge(config.host_ifname());
    return;
  }

  // Signal the container that the network device is ready.
  // This is only applicable for arc0.
  if (device->IsAndroid() || device->IsLegacyAndroid()) {
    datapath_->runner().WriteSentinelToContainer(base::IntToString(pid_));
  }
}

void ArcService::OnDeviceRemoved(Device* device) {
  if (!ShouldProcessDevice(*device))
    return;

  StopDevice(device);

  const auto& config = device->config();

  LOG(INFO) << "Removing device " << device->ifname()
            << " bridge: " << config.host_ifname()
            << " guest_iface: " << config.guest_ifname();

  device->Disable();
  if (device->IsLegacyAndroid()) {
    datapath_->RemoveOutboundIPv4(config.host_ifname());
    datapath_->RemoveLegacyIPv4DNAT();
  } else if (!device->IsAndroid()) {
    datapath_->RemoveOutboundIPv4(config.host_ifname());
    datapath_->RemoveInboundIPv4DNAT(
        device->ifname(), IPv4AddressToString(config.guest_ipv4_addr()));
  }

  datapath_->RemoveBridge(config.host_ifname());

  devices_.erase(config.guest_ifname());
}

void ArcService::StopDevice(Device* device) {
  const auto& config = device->config();

  // If the device isn't known then there is nothing to do...
  if (devices_.find(config.guest_ifname()) == devices_.end())
    return;

  if (!ShouldProcessDevice(*device))
    return;

  LOG(INFO) << "Stopping device " << device->ifname()
            << " bridge: " << config.host_ifname()
            << " guest_iface: " << config.guest_ifname();

  device->Disable();
  if (!device->IsAndroid()) {
    datapath_->RemoveInterface(ArcVethHostName(device->ifname()));
  }
}

bool ArcService::ShouldProcessDevice(const Device& device) const {
  // ARC N uses legacy single networking and only requires the arcbr0/arc0
  // configuration. Any other device can be safely ignored.
  if (guest_ == GuestMessage::ARC_LEGACY && !device.IsLegacyAndroid())
    return false;

  // If ARC isn't running, there is nothing to do. This call must have been
  // triggered by a device hot-plug event or something similar in DeviceManager.
  // It's OK to ignore because if the container is gone there is nothing to do.
  if (pid_ == kInvalidPID)
    return false;

  return true;
}

void ArcService::OnDefaultInterfaceChanged(const std::string& ifname) {
  if (pid_ == kInvalidPID || guest_ != GuestMessage::ARC_LEGACY)
    return;

  datapath_->RemoveLegacyIPv4InboundDNAT();

  auto* device = dev_mgr_->FindByGuestInterface("arc0");
  if (!device) {
    LOG(DFATAL) << "Expected legacy Android device missing";
    return;
  }

  device->Disable();
  if (!ifname.empty()) {
    datapath_->AddLegacyIPv4InboundDNAT(ifname);
    device->Enable(ifname);
  }
}

void ArcService::LinkMsgHandler(const shill::RTNLMessage& msg) {
  if (!msg.HasAttribute(IFLA_IFNAME)) {
    LOG(ERROR) << "Link event message does not have IFLA_IFNAME";
    return;
  }
  bool link_up = msg.link_status().flags & IFF_UP;
  shill::ByteString b(msg.GetAttribute(IFLA_IFNAME));
  std::string ifname(reinterpret_cast<const char*>(
      b.GetSubstring(0, IFNAMSIZ).GetConstData()));

  auto* device = dev_mgr_->FindByGuestInterface(ifname);
  if (!device || !device->LinkUp(ifname, link_up))
    return;

  if (!link_up) {
    LOG(INFO) << ifname << " is now down";
    return;
  }
  LOG(INFO) << ifname << " is now up";

  if (device->IsAndroid())
    return;

  if (device->IsLegacyAndroid()) {
    OnDefaultInterfaceChanged(dev_mgr_->DefaultInterface());
    return;
  }

  device->Enable(ifname);
}

void ArcService::SetupIPv6(Device* device) {
  device->RegisterIPv6TeardownHandler(
      base::Bind(&ArcService::TeardownIPv6, weak_factory_.GetWeakPtr()));

  auto& ipv6_config = device->ipv6_config();
  if (ipv6_config.ifname.empty())
    return;

  LOG(INFO) << "Setting up IPv6 for " << ipv6_config.ifname;

  ipv6_config.routing_table_id =
      GetAndroidRoutingTableId(device->config().guest_ifname(), pid_);
  if (ipv6_config.routing_table_id == kInvalidTableID) {
    if (ipv6_config.routing_table_attempts++ < kMaxTableRetries) {
      LOG(INFO) << "Could not look up routing table ID for container interface "
                << device->config().guest_ifname() << " - trying again...";
      base::MessageLoop::current()->task_runner()->PostDelayedTask(
          FROM_HERE,
          base::Bind(&ArcService::SetupIPv6, weak_factory_.GetWeakPtr(),
                     device),
          kTableRetryDelay);
    } else {
      LOG(DFATAL)
          << "Could not look up routing table ID for container interface "
          << device->config().guest_ifname();
    }
    return;
  }

  LOG(INFO) << "Setting IPv6 address " << ipv6_config.addr
            << "/128, gateway=" << ipv6_config.router << " on "
            << ipv6_config.ifname;

  char buf[INET6_ADDRSTRLEN] = {0};
  if (!inet_ntop(AF_INET6, &ipv6_config.addr, buf, sizeof(buf))) {
    LOG(DFATAL) << "Invalid address: " << ipv6_config.addr;
    return;
  }
  std::string addr = buf;

  if (!inet_ntop(AF_INET6, &ipv6_config.router, buf, sizeof(buf))) {
    LOG(DFATAL) << "Invalid router address: " << ipv6_config.router;
    return;
  }
  std::string router = buf;

  const auto& config = device->config();
  {
    ScopedNS ns(pid_);
    if (!ns.IsValid()) {
      LOG(ERROR) << "Invalid container namespace (" << pid_
                 << ") - cannot configure IPv6.";
      return;
    }
    if (!datapath_->AddIPv6GatewayRoutes(config.guest_ifname(), addr, router,
                                         ipv6_config.prefix_len,
                                         ipv6_config.routing_table_id)) {
      LOG(ERROR) << "Failed to setup IPv6 routes in the container";
      return;
    }
  }

  if (!datapath_->AddIPv6HostRoute(config.host_ifname(), addr,
                                   ipv6_config.prefix_len)) {
    LOG(ERROR) << "Failed to setup the IPv6 route for interface "
               << config.host_ifname();
    return;
  }

  if (!datapath_->AddIPv6Neighbor(ipv6_config.ifname, addr)) {
    LOG(ERROR) << "Failed to setup the IPv6 neighbor proxy";
    datapath_->RemoveIPv6HostRoute(config.host_ifname(), addr,
                                   ipv6_config.prefix_len);
    return;
  }

  if (!datapath_->AddIPv6Forwarding(ipv6_config.ifname,
                                    device->config().host_ifname())) {
    LOG(ERROR) << "Failed to setup iptables for IPv6";
    datapath_->RemoveIPv6Neighbor(ipv6_config.ifname, addr);
    datapath_->RemoveIPv6HostRoute(config.host_ifname(), addr,
                                   ipv6_config.prefix_len);
    return;
  }

  ipv6_config.is_setup = true;
}

void ArcService::TeardownIPv6(Device* device) {
  auto& ipv6_config = device->ipv6_config();
  if (!ipv6_config.is_setup)
    return;

  LOG(INFO) << "Clearing IPv6 for " << ipv6_config.ifname;
  ipv6_config.is_setup = false;

  char buf[INET6_ADDRSTRLEN] = {0};
  if (!inet_ntop(AF_INET6, &ipv6_config.addr, buf, sizeof(buf))) {
    LOG(DFATAL) << "Invalid address: " << ipv6_config.addr;
    return;
  }
  std::string addr = buf;

  if (!inet_ntop(AF_INET6, &ipv6_config.router, buf, sizeof(buf))) {
    LOG(DFATAL) << "Invalid router address: " << ipv6_config.router;
    return;
  }
  std::string router = buf;

  const auto& config = device->config();
  datapath_->RemoveIPv6Forwarding(ipv6_config.ifname, config.host_ifname());
  datapath_->RemoveIPv6Neighbor(ipv6_config.ifname, addr);
  datapath_->RemoveIPv6HostRoute(config.host_ifname(), addr,
                                 ipv6_config.prefix_len);

  ScopedNS ns(pid_);
  if (ns.IsValid()) {
    datapath_->RemoveIPv6GatewayRoutes(config.guest_ifname(), addr, router,
                                       ipv6_config.prefix_len,
                                       ipv6_config.routing_table_id);
  } else {
    LOG(ERROR) << "Invalid container namespace (" << pid_
               << ") - cannot cleanup IPv6.";
  }
}

void ArcService::SetPIDForTestingOnly() {
  pid_ = kTestPID;
}

}  // namespace arc_networkd
