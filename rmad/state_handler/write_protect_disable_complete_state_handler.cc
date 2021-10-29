// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rmad/state_handler/write_protect_disable_complete_state_handler.h"

#include <memory>
#include <utility>

#include <base/logging.h>

#include "rmad/constants.h"
#include "rmad/proto_bindings/rmad.pb.h"

namespace rmad {

namespace fake {

FakeWriteProtectDisableCompleteStateHandler::
    FakeWriteProtectDisableCompleteStateHandler(
        scoped_refptr<JsonStore> json_store)
    : WriteProtectDisableCompleteStateHandler(json_store) {}

}  // namespace fake

WriteProtectDisableCompleteStateHandler::
    WriteProtectDisableCompleteStateHandler(scoped_refptr<JsonStore> json_store)
    : BaseStateHandler(json_store) {}

RmadErrorCode WriteProtectDisableCompleteStateHandler::InitializeState() {
  // Always check again when entering the state.
  auto wp_disable_complete =
      std::make_unique<WriteProtectDisableCompleteState>();
  bool wp_disable_skipped = false;
  json_store_->GetValue(kWpDisableSkipped, &wp_disable_skipped);
  bool keep_device_open = false;
  json_store_->GetValue(kKeepDeviceOpen, &keep_device_open);

  if (wp_disable_skipped) {
    wp_disable_complete->set_action(
        WriteProtectDisableCompleteState::
            RMAD_WP_DISABLE_SKIPPED_ASSEMBLE_DEVICE);
  } else if (keep_device_open) {
    wp_disable_complete->set_action(
        WriteProtectDisableCompleteState::
            RMAD_WP_DISABLE_COMPLETE_KEEP_DEVICE_OPEN);
  } else {
    wp_disable_complete->set_action(
        WriteProtectDisableCompleteState::
            RMAD_WP_DISABLE_COMPLETE_ASSEMBLE_DEVICE);
  }

  state_.set_allocated_wp_disable_complete(wp_disable_complete.release());
  return RMAD_ERROR_OK;
}

BaseStateHandler::GetNextStateCaseReply
WriteProtectDisableCompleteStateHandler::GetNextStateCase(
    const RmadState& state) {
  if (!state.has_wp_disable_complete()) {
    LOG(ERROR) << "RmadState missing |WP disable complete| state.";
    return NextStateCaseWrapper(RMAD_ERROR_REQUEST_INVALID);
  }

  return NextStateCaseWrapper(RmadState::StateCase::kUpdateRoFirmware);
}

}  // namespace rmad
