// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/supplicant/mock_supplicant_bss_proxy.h"

namespace shill {

MockSupplicantBSSProxy::MockSupplicantBSSProxy() {}

MockSupplicantBSSProxy::~MockSupplicantBSSProxy() {
  Die();
}

}  // namespace shill
