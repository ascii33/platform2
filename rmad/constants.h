// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_CONSTANTS_H_
#define RMAD_CONSTANTS_H_

#include <array>
#include <limits>

#include "rmad/proto_bindings/rmad.pb.h"

namespace rmad {

// JsonStore rmad_interface keys.
inline constexpr char kStateHistory[] = "state_history";
inline constexpr char kStateMap[] = "state_map";
inline constexpr char kNetworkConnected[] = "network_connected";
inline constexpr char kReplacedComponentNames[] = "replaced_component_names";
inline constexpr char kSameOwner[] = "same_owner";
inline constexpr char kCalibrationMap[] = "calibration_map";

// Component traits.
inline constexpr std::array<RmadComponent, 3> kComponentsNeedManualCalibration =
    {RMAD_COMPONENT_GYROSCOPE, RMAD_COMPONENT_ACCELEROMETER,
     RMAD_COMPONENT_MAINBOARD_REWORK};
inline constexpr std::array<RmadComponent, 3> kComponentsNeedAutoCalibration = {
    RMAD_COMPONENT_AUDIO_CODEC,
    RMAD_COMPONENT_TOUCHSCREEN,
    RMAD_COMPONENT_MAINBOARD_REWORK,
};

inline constexpr std::array<std::array<int, 2>, 5>
    kComponentsCalibrationPriority = {
        {{static_cast<int>(CheckCalibrationState::CalibrationStatus::
                               RMAD_CALIBRATION_COMPONENT_ACCELEROMETER),
          1},
         {static_cast<int>(CheckCalibrationState::CalibrationStatus::
                               RMAD_CALIBRATION_COMPONENT_GYROSCOPE),
          1},
         {static_cast<int>(CheckCalibrationState::CalibrationStatus::
                               RMAD_CALIBRATION_COMPONENT_BASE_ACCELEROMETER),
          1},
         {static_cast<int>(CheckCalibrationState::CalibrationStatus::
                               RMAD_CALIBRATION_COMPONENT_LID_ACCELEROMETER),
          2},
         {static_cast<int>(CheckCalibrationState::CalibrationStatus::
                               RMAD_CALIBRATION_COMPONENT_UNKNOWN),
          std::numeric_limits<int>::max()}}};

}  // namespace rmad

#endif  // RMAD_CONSTANTS_H_
