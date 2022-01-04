// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "diagnostics/cros_healthd/fetchers/display_fetcher.h"
#include "diagnostics/cros_healthd/utils/error_utils.h"

namespace diagnostics {

namespace {

namespace mojo_ipc = ::chromeos::cros_healthd::mojom;

void FillDisplaySize(const std::unique_ptr<LibdrmUtil>& libdrm_util,
                     const uint32_t connector_id,
                     mojo_ipc::NullableUint32Ptr* out_width,
                     mojo_ipc::NullableUint32Ptr* out_height) {
  uint32_t width;
  uint32_t height;
  libdrm_util->FillDisplaySize(connector_id, &width, &height);

  *out_width = mojo_ipc::NullableUint32::New(width);
  *out_height = mojo_ipc::NullableUint32::New(height);
}

mojo_ipc::EmbeddedDisplayInfoPtr FetchEmbeddedDisplayInfo(
    const std::unique_ptr<LibdrmUtil>& libdrm_util) {
  auto edp_info = mojo_ipc::EmbeddedDisplayInfo::New();
  auto edp_connector_id = libdrm_util->GetEmbeddedDisplayConnectorID();
  libdrm_util->FillPrivacyScreenInfo(edp_connector_id,
                                     &edp_info->privacy_screen_supported,
                                     &edp_info->privacy_screen_enabled);

  FillDisplaySize(libdrm_util, edp_connector_id, &edp_info->display_width,
                  &edp_info->display_height);

  return edp_info;
}

}  // namespace

void DisplayFetcher::FetchDisplayInfo(
    DisplayFetcher::FetchDisplayInfoCallback&& callback) {
  auto libdrm_util = context_->CreateLibdrmUtil();
  if (!libdrm_util->Initialize()) {
    std::move(callback).Run(mojo_ipc::DisplayResult::NewError(
        CreateAndLogProbeError(mojo_ipc::ErrorType::kSystemUtilityError,
                               "Failed to initialize libdrm_util object.")));
    return;
  }

  auto display_info = mojo_ipc::DisplayInfo::New();
  auto edp_info = FetchEmbeddedDisplayInfo(libdrm_util);
  display_info->edp_info = std::move(edp_info);

  std::move(callback).Run(
      mojo_ipc::DisplayResult::NewDisplayInfo(std::move(display_info)));
}

}  // namespace diagnostics
