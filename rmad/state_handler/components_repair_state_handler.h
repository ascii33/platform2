// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_STATE_HANDLER_COMPONENTS_REPAIR_STATE_HANDLER_H_
#define RMAD_STATE_HANDLER_COMPONENTS_REPAIR_STATE_HANDLER_H_

#include "rmad/state_handler/base_state_handler.h"

#include <memory>
#include <unordered_map>

#include "rmad/utils/dbus_utils.h"

namespace rmad {

class ComponentsRepairStateHandler : public BaseStateHandler {
 public:
  explicit ComponentsRepairStateHandler(scoped_refptr<JsonStore> json_store);
  // Used to inject mocked |dbus_utils_| for testing.
  ComponentsRepairStateHandler(scoped_refptr<JsonStore> json_store,
                               std::unique_ptr<DBusUtils> dbus_utils);
  ~ComponentsRepairStateHandler() override = default;

  ASSIGN_STATE(RmadState::StateCase::kComponentsRepair);
  SET_REPEATABLE;

  RmadErrorCode InitializeState() override;
  GetNextStateCaseReply GetNextStateCase(const RmadState& state) override;

 private:
  // Check that the provided state properly updates every component.
  bool ValidateUserSelection(const RmadState& state) const;
  // Store variables that can be used by other state handlers to make decisions.
  bool StoreVars() const;

  std::unique_ptr<DBusUtils> dbus_utils_;
};

}  // namespace rmad

#endif  // RMAD_STATE_HANDLER_COMPONENTS_REPAIR_STATE_HANDLER_H_
