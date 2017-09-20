// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEBUGD_SRC_SHILL_SCRIPTS_TOOL_H_
#define DEBUGD_SRC_SHILL_SCRIPTS_TOOL_H_

#include <string>
#include <vector>

#include <base/macros.h>
#include <brillo/errors/error.h>
#include <dbus/file_descriptor.h>

#include "debugd/src/subprocess_tool.h"

namespace debugd {

class ShillScriptsTool : public SubprocessTool {
 public:
  ShillScriptsTool() = default;
  ~ShillScriptsTool() override = default;

  bool Run(const dbus::FileDescriptor& outfd,
           const std::string& script,
           const std::vector<std::string>& script_args,
           std::string* out_id,
           brillo::ErrorPtr* error);

 private:
  DISALLOW_COPY_AND_ASSIGN(ShillScriptsTool);
};

}  // namespace debugd

#endif  // DEBUGD_SRC_SHILL_SCRIPTS_TOOL_H_
