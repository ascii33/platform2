// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VTPM_BACKENDS_MOCK_TPM_HANDLE_MANAGER_H_
#define VTPM_BACKENDS_MOCK_TPM_HANDLE_MANAGER_H_

#include "vtpm/backends/tpm_handle_manager.h"

#include <vector>

#include <gmock/gmock.h>

namespace vtpm {

class MockTpmHandleManager : public TpmHandleManager {
 public:
  ~MockTpmHandleManager() override = default;

  MOCK_METHOD(bool,
              IsHandleTypeSuppoerted,
              (trunks::TPM_HANDLE handle),
              (override));

  MOCK_METHOD(trunks::TPM_RC,
              GetHandleList,
              (trunks::TPM_HANDLE, std::vector<trunks::TPM_HANDLE>*),
              (override));
};

}  // namespace vtpm

#endif  // VTPM_BACKENDS_MOCK_TPM_HANDLE_MANAGER_H_
