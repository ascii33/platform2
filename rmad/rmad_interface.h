// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_RMAD_INTERFACE_H_
#define RMAD_RMAD_INTERFACE_H_

#include <memory>
#include <string>

#include <base/callback.h>
#include <rmad/proto_bindings/rmad.pb.h>

namespace rmad {

class RmadInterface {
 public:
  RmadInterface() = default;
  virtual ~RmadInterface() = default;

  // Fully set up the interface. To minimize unnecessary initialization when RMA
  // is not required, the D-Bus APIs might be called when the class is
  // initialized by the constructor but not fully set up.
  virtual bool SetUp() = 0;

  // Register a callback for requesting to quit the daemon.
  virtual void RegisterRequestQuitDaemonCallback(
      std::unique_ptr<base::RepeatingCallback<void()>> callback) = 0;

  // Register a signal sender for specific states. Virtual functions cannot be
  // declared as template so we need to declare them one by one.
  virtual void RegisterSignalSender(
      RmadState::StateCase state_case,
      std::unique_ptr<base::RepeatingCallback<bool(bool)>> callback) = 0;

  using HardwareVerificationResultSignalCallback =
      base::RepeatingCallback<bool(const HardwareVerificationResult&)>;
  virtual void RegisterSignalSender(
      RmadState::StateCase state_case,
      std::unique_ptr<HardwareVerificationResultSignalCallback> callback) = 0;

  using UpdateRoFirmwareStatusSignalCallback =
      base::RepeatingCallback<bool(UpdateRoFirmwareStatus)>;
  virtual void RegisterSignalSender(
      RmadState::StateCase state_case,
      std::unique_ptr<UpdateRoFirmwareStatusSignalCallback> callback) = 0;

  using CalibrationOverallSignalCallback =
      base::RepeatingCallback<bool(CalibrationOverallStatus)>;
  virtual void RegisterSignalSender(
      RmadState::StateCase state_case,
      std::unique_ptr<CalibrationOverallSignalCallback> callback) = 0;

  using CalibrationComponentSignalCallback =
      base::RepeatingCallback<bool(CalibrationComponentStatus)>;
  virtual void RegisterSignalSender(
      RmadState::StateCase state_case,
      std::unique_ptr<CalibrationComponentSignalCallback> callback) = 0;

  using ProvisionSignalCallback =
      base::RepeatingCallback<bool(const ProvisionStatus&)>;
  virtual void RegisterSignalSender(
      RmadState::StateCase state_case,
      std::unique_ptr<ProvisionSignalCallback> callback) = 0;

  using FinalizeSignalCallback =
      base::RepeatingCallback<bool(const FinalizeStatus&)>;
  virtual void RegisterSignalSender(
      RmadState::StateCase state_case,
      std::unique_ptr<FinalizeSignalCallback> callback) = 0;

  // Get the current state_case.
  virtual RmadState::StateCase GetCurrentStateCase() = 0;

  // Try to transition to the next state using the current state without
  // additional user input.
  virtual void TryTransitionNextStateFromCurrentState() = 0;

  // Callback used by all state functions to return the current state to the
  // dbus service.
  using GetStateCallback = base::OnceCallback<void(const GetStateReply&)>;

  // Get the initialized current RmadState proto.
  virtual void GetCurrentState(GetStateCallback callback) = 0;
  // Update the state using the RmadState proto in the request and return the
  // resulting state after all work is done.
  virtual void TransitionNextState(const TransitionNextStateRequest& request,
                                   GetStateCallback callback) = 0;
  // Go back to the previous state if possible and return the RmadState proto.
  virtual void TransitionPreviousState(GetStateCallback callback) = 0;

  using AbortRmaCallback = base::OnceCallback<void(const AbortRmaReply&)>;
  // Cancel the RMA process if possible and reboot.
  virtual void AbortRma(AbortRmaCallback callback) = 0;

  // Returns whether it's allowed to abort RMA now.
  virtual bool CanAbort() const = 0;
};

}  // namespace rmad

#endif  // RMAD_RMAD_INTERFACE_H_
