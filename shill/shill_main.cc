// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <base/bind.h>
#include <base/command_line.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <brillo/minijail/minijail.h>
#include <brillo/syslog_logging.h>

#include "shill/daemon_task.h"
#include "shill/error.h"
#include "shill/logging.h"
#include "shill/shill_config.h"
#include "shill/shill_daemon.h"
#include "shill/technology.h"

using std::string;
using std::vector;

namespace switches {

// Don't daemon()ize; run in foreground.
static const char kForeground[] = "foreground";
// Don't attempt to manage these devices.
static const char kDeviceBlackList[] = "device-black-list";
// Manage only these devices.
static const char kDeviceWhiteList[] = "device-white-list";
// ARC network device.
static const char kArcDevice[] = "arc-device";
// Ignore Ethernet-like devices that don't have any driver information.
static const char kIgnoreUnknownEthernet[] = "ignore-unknown-ethernet";
// Technologies to enable for portal check at startup.
static const char kPortalList[] = "portal-list";
// When in passive mode, Shill will not manage any devices by default.
// Remote service can instruct Shill to manage/unmanage devices through
// org.chromium.flimflam.Manager's ClaimInterface/ReleaseInterface APIs.
static const char kPassiveMode[] = "passive-mode";
// Default priority order of the technologies.
static const char kTechnologyOrder[] = "default-technology-order";
// Comma-separated list of DNS servers to prepend to the resolver list.
static const char kPrependDNSServers[] = "prepend-dns-servers";
// The minimum MTU value that will be respected in DHCP responses.
static const char kMinimumMTU[] = "minimum-mtu";
// Accept hostname from the DHCP server for the specified devices.
// eg. eth0 or eth*
static const char kAcceptHostnameFrom[] = "accept-hostname-from";
// Flag that causes shill to run third party VPN client programs in a minijail.
static const char kJailVpnClients[] = "jail-vpn-clients";
#ifndef DISABLE_DHCPV6
// List of devices to enable DHCPv6.
static const char kDhcpv6EnabledDevices[] = "dhcpv6-enabled-devices";
#endif  // DISABLE_DHCPV6
// Flag that causes shill to show the help message and exit.
static const char kHelp[] = "help";

// The help message shown if help flag is passed to the program.
static const char kHelpMessage[] = "\n"
    "Available Switches: \n"
    "  --foreground\n"
    "    Don\'t daemon()ize; run in foreground.\n"
    "  --device-black-list=device1,device2\n"
    "    Do not manage devices named device1 or device2\n"
    "  --device-white-list=device1,device2\n"
    "    Manage only devices named device1 and device2\n"
    "  --ignore-unknown-ethernet\n"
    "    Ignore Ethernet-like devices that do not report a driver\n"
    "  --log-level=N\n"
    "    Logging level:\n"
    "      0 = LOG(INFO), 1 = LOG(WARNING), 2 = LOG(ERROR),\n"
    "      -1 = SLOG(..., 1), -2 = SLOG(..., 2), etc.\n"
    "  --log-scopes=\"*scope1+scope2\".\n"
    "    Scopes to enable for SLOG()-based logging.\n"
    "  --portal-list=technology1,technology2\n"
    "    Specify technologies to perform portal detection on at startup.\n"
    "  --passive-mode\n"
    "    Do not manage any devices by default\n"
    "  --default-technology-order=technology1,technology2\n"
    "    Specify the default priority order of the technologies.\n"
    "  --prepend-dns-servers=server1,server2,...\n"
    "    Prepend the provided DNS servers to the resolver list.\n"
    "  --accept-hostname-from=eth0 or --accept-hostname-from=eth*\n"
    "    Accept a hostname from the DHCP server for the matching devices.\n"
#ifndef DISABLE_DHCPV6
    "  --dhcpv6-enabled-devices=device1,device2\n"
    "    Enable DHCPv6 for devices named device1 and device2\n"
#endif  // DISABLE_DHCPV6
    "  --minimum-mtu=mtu\n"
    "    Set the minimum value to respect as the MTU from DHCP responses.\n"
    "  --jail-vpn-clients\n"
    "    Spawn third party VPN client programs in a minijail.\n";
}  // namespace switches

namespace {

const char kLoggerCommand[] = "/usr/bin/logger";
const char kLoggerUser[] = "syslog";
const char kDefaultTechnologyOrder[] = "vpn,ethernet,wifi,wimax,cellular";

}  // namespace

// Always logs to the syslog and logs to stderr if
// we are running in the foreground.
void SetupLogging(bool foreground, const char* daemon_name) {
  int log_flags = 0;
  log_flags |= brillo::kLogToSyslog;
  log_flags |= brillo::kLogHeader;
  if (foreground) {
    log_flags |= brillo::kLogToStderr;
  }
  brillo::InitLog(log_flags);

  if (!foreground) {
    vector<char*> logger_command_line;
    int logger_stdin_fd;
    logger_command_line.push_back(const_cast<char*>(kLoggerCommand));
    logger_command_line.push_back(const_cast<char*>("--priority"));
    logger_command_line.push_back(const_cast<char*>("daemon.err"));
    logger_command_line.push_back(const_cast<char*>("--tag"));
    logger_command_line.push_back(const_cast<char*>(daemon_name));
    logger_command_line.push_back(nullptr);

    brillo::Minijail* minijail = brillo::Minijail::GetInstance();
    struct minijail* jail = minijail->New();
    minijail->DropRoot(jail, kLoggerUser, kLoggerUser);

    if (!minijail->RunPipeAndDestroy(jail, logger_command_line, nullptr,
                                     &logger_stdin_fd)) {
      LOG(ERROR) << "Unable to spawn logger. "
                 << "Writes to stderr will be discarded.";
      return;
    }

    // Note that we don't set O_CLOEXEC here. This means that stderr
    // from any child processes will, by default, be logged to syslog.
    if (dup2(logger_stdin_fd, fileno(stderr)) != fileno(stderr)) {
      PLOG(ERROR) << "Failed to redirect stderr to syslog";
    }
    close(logger_stdin_fd);
  }
}

void OnStartup(const char *daemon_name, base::CommandLine* cl) {
  SetupLogging(cl->HasSwitch(switches::kForeground), daemon_name);
  shill::SetLogLevelFromCommandLine(cl);
}

int main(int argc, char** argv) {
  base::CommandLine::Init(argc, argv);
  base::CommandLine* cl = base::CommandLine::ForCurrentProcess();

  if (cl->HasSwitch(switches::kHelp)) {
    LOG(INFO) << switches::kHelpMessage;
    return 0;
  }

  shill::DaemonTask::Settings settings;
  if (cl->HasSwitch(switches::kTechnologyOrder)) {
    shill::Error error;
    string order_flag = cl->GetSwitchValueASCII(
        switches::kTechnologyOrder);
    vector<shill::Technology::Identifier> test_order_vector;
    if (shill::Technology::GetTechnologyVectorFromString(
        order_flag, &test_order_vector, &error)) {
      settings.default_technology_order = order_flag;
    }  else {
      LOG(ERROR) << "Invalid default technology order: [" << order_flag
                 << "] Error: " << error.message();
    }
  }
  if (settings.default_technology_order.empty()) {
    settings.default_technology_order = kDefaultTechnologyOrder;
  }

  if (cl->HasSwitch(switches::kDeviceBlackList)) {
    settings.device_blacklist = base::SplitString(
        cl->GetSwitchValueASCII(switches::kDeviceBlackList), ",",
        base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  }

  if (cl->HasSwitch(switches::kDeviceWhiteList)) {
    settings.device_whitelist = base::SplitString(
        cl->GetSwitchValueASCII(switches::kDeviceWhiteList), ",",
        base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  }

  if (cl->HasSwitch(switches::kArcDevice)) {
    settings.arc_device = cl->GetSwitchValueASCII(switches::kArcDevice);
  }

  settings.ignore_unknown_ethernet =
      cl->HasSwitch(switches::kIgnoreUnknownEthernet);

  if (cl->HasSwitch(switches::kPortalList)) {
    settings.use_portal_list = true;
    settings.portal_list = cl->GetSwitchValueASCII(switches::kPortalList);
  }

  settings.passive_mode = cl->HasSwitch(switches::kPassiveMode);

  if (cl->HasSwitch(switches::kPrependDNSServers)) {
    settings.prepend_dns_servers =
        cl->GetSwitchValueASCII(switches::kPrependDNSServers);
  }

  if (cl->HasSwitch(switches::kMinimumMTU)) {
    int mtu;
    std::string value = cl->GetSwitchValueASCII(switches::kMinimumMTU);
    if (!base::StringToInt(value, &mtu)) {
      LOG(FATAL) << "Could not convert '" << value << "' to integer.";
    }
    settings.minimum_mtu = mtu;
  }

  if (cl->HasSwitch(switches::kAcceptHostnameFrom)) {
    settings.accept_hostname_from =
        cl->GetSwitchValueASCII(switches::kAcceptHostnameFrom);
  }

  if (cl->HasSwitch(switches::kJailVpnClients)) {
    settings.jail_vpn_clients = true;
  } else {
    settings.jail_vpn_clients = false;
  }

#ifndef DISABLE_DHCPV6
  if (cl->HasSwitch(switches::kDhcpv6EnabledDevices)) {
    settings.dhcpv6_enabled_devices = base::SplitString(
        cl->GetSwitchValueASCII(switches::kDhcpv6EnabledDevices), ",",
        base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  }
#endif  // DISABLE_DHCPV6

  shill::Config config;

  shill::ShillDaemon daemon(base::Bind(&OnStartup, argv[0], cl), settings,
                            &config);
  daemon.Run();

  LOG(INFO) << "Process exiting.";

  return 0;
}
