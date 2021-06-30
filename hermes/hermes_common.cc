// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>

#include "base/logging.h"
#include "hermes/hermes_common.h"

namespace hermes {

std::string GetTrailingChars(const std::string& pii, int num_chars) {
  DCHECK_GE(num_chars, 0);
  if (num_chars > pii.length())
    return pii;
  return pii.substr(pii.length() - num_chars);
}

std::string GetObjectPathForLog(const dbus::ObjectPath& dbus_path) {
  const std::string kPrefix = "dbus_path(Last 3 chars): ";
  const int kDbusPathPrintLen = 3;
  return kPrefix + GetTrailingChars(dbus_path.value(), kDbusPathPrintLen);
}

void IgnoreErrorRunClosure(base::OnceCallback<void()> cb, int err) {
  VLOG(2) << "Modem message processed with code:" << err;
  std::move(cb).Run();
}

void PrintMsgProcessingResult(int err) {
  VLOG(2) << "Modem processed message processed with code:" << err;
}

}  // namespace hermes
