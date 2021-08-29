// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_UTILS_MOCK_VPD_UTILS_H_
#define RMAD_UTILS_MOCK_VPD_UTILS_H_

#include "rmad/utils/vpd_utils.h"

#include <string>

#include <gmock/gmock.h>

namespace rmad {

class MockVpdUtils : public VpdUtils {
 public:
  MockVpdUtils() = default;
  ~MockVpdUtils() override = default;

  MOCK_METHOD(bool,
              SetRoVpd,
              (const std::string&, const std::string&),
              (override));
  MOCK_METHOD(bool,
              GetRoVpd,
              (const std::string&, std::string*),
              (const, override));
  MOCK_METHOD(bool,
              SetRwVpd,
              (const std::string&, const std::string&),
              (override));
  MOCK_METHOD(bool,
              GetRwVpd,
              (const std::string&, std::string*),
              (const, override));
};

}  // namespace rmad

#endif  // RMAD_UTILS_MOCK_VPD_UTILS_H_