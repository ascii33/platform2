//
// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <iostream>

#include <base/command_line.h>
#include <base/logging.h>

#include "proxy_daemon.h"
#include "proxy_dbus_client.h"
#include "proxy_rpc_server.h"

static const int kXmlRpcLibVerbosity = 5;

namespace {
namespace switches {
static const char kHelp[] = "help";
static const char kPort[] = "port";
static const char kHelpMessage[] = "\n"
    "Available Switches: \n"
    "  --port=<port>\n"
    "    Set the RPC server to listen on this TCP port(mandatory).\n";
}  // namespace switches
}  // namespace

int main(int argc, char* argv[]) {
  base::CommandLine::Init(argc, argv);
  base::CommandLine* cl = base::CommandLine::ForCurrentProcess();

  if (cl->HasSwitch(switches::kHelp)) {
    LOG(INFO) << switches::kHelpMessage;
    return EXIT_SUCCESS;
  }

  if (!cl->HasSwitch(switches::kPort)) {
    LOG(ERROR) << "port switch is mandatory.";
    LOG(ERROR) << switches::kHelpMessage;
    return EXIT_FAILURE;
  }

  int port = std::stoi(cl->GetSwitchValueASCII(switches::kPort));

  // Create the dbus daemon
  ProxyDaemon proxy_daemon(port, kXmlRpcLibVerbosity);

  // Run indefinitely
  proxy_daemon.Run();

  return 0;
}

