// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_BACKLIGHT_CONTROLLER_H_
#define POWER_MANAGER_BACKLIGHT_CONTROLLER_H_

#include <algorithm>

#include "power_manager/backlight_interface.h"
#include "power_manager/power_prefs_interface.h"
#include "power_manager/signal_callback.h"

namespace power_manager {

typedef int gboolean;

class AmbientLightSensor;

enum PowerState {
  BACKLIGHT_ACTIVE_ON,
  BACKLIGHT_DIM,
  BACKLIGHT_IDLE_OFF,
  BACKLIGHT_ACTIVE_OFF,
  BACKLIGHT_SUSPENDED,
  BACKLIGHT_UNINITIALIZED
};

enum PluggedState {
  kPowerDisconnected,
  kPowerConnected,
  kPowerUnknown,
};

// Control the backlight.
class BacklightController {
 public:
  BacklightController(BacklightInterface* backlight,
                      PowerPrefsInterface* prefs);
  virtual ~BacklightController() {}

  void set_light_sensor(AmbientLightSensor* als) { light_sensor_ = als; }

  int64 plugged_brightness_offset() const { return plugged_brightness_offset_; }
  void set_plugged_brightness_offset(int64 offset) {
    plugged_brightness_offset_ = offset;
  }

  int64 unplugged_brightness_offset() const {
    return unplugged_brightness_offset_;
  }
  void set_unplugged_brightness_offset(int64 offset) {
    unplugged_brightness_offset_ = offset;
  }

  int64 local_brightness() const { return local_brightness_; }

  // Initialize the object.
  bool Init();

  // Get the current brightness of the backlight, as a percentage.
  // TODO(sque): rename to something like GetCurrentBrightness to better
  // distinguish from GetTargetBrightness.
  bool GetBrightness(int64* level);
  // Get the intended brightness of the backlight, as a percentage.  Intended
  // brightness is the destination brightness during a transition.  Once the
  // transition completes, this equals the current brightness.
  bool GetTargetBrightness(int64* level);

  // Increase the brightness level of the backlight by one level.
  void IncreaseBrightness();

  // Decrease the brightness level of the backlight by one level.
  void DecreaseBrightness();

  // Turn the backlight on or off.  Returns true if the brightness was changed
  // and false otherwise.
  bool SetPowerState(PowerState state);

  // Mark the computer as plugged or unplugged, and adjust the brightness
  // appropriately.  Returns true if the brightness was changed and false
  // otherwise.
  bool OnPlugEvent(bool is_plugged);

  void SetAlsBrightnessLevel(int64 level);

  void SetMinimumBrightness(int64 level);

 private:
  // Clamp |value| to fit between 0 and 100.
  int64 Clamp(int64 value);

  // Clamp |value| to fit between min_ and 100.
  int64 ClampToMin(int64 value);

  // Converts between [0, 100] and [min, max] brightness scales.
  int64 RawBrightnessToLocalBrightness(int64 raw_level);
  int64 LocalBrightnessToRawBrightness(int64 local_level);

  void ReadPrefs();
  void WritePrefs();

  // Read brightness settings from the system and apply any changes made
  // by other programs to our local view. Return true if the brightness
  // has not been modified by other programs; return false otherwise.
  bool ReadBrightness();

  // Write brightness based on current settings.
  // Returns true if the brightness was changed and false otherwise.
  bool WriteBrightness();

  void SetBrightnessToZero();

  // Changes the brightness to |target_level| over time.  This is used for
  // smoothing effects.
  bool SetBrightnessGradual(int64 target_level);

  // Callback function to set backlight brightness through the backlight
  // interface.  This is used by SetBrightnessGradual to change the brightness
  // over a series of steps.
  //
  // Example:
  //   Current brightness = 40
  //   Want to set brightness to 60 over 5 steps, so the steps are:
  //      40 -> 44 -> 48 -> 52 -> 56 -> 60
  //   Thus, SetBrightnessHard(level, target_level) would be called five times
  //   with the args:
  //      SetBrightnessHard(44, 60);
  //      SetBrightnessHard(48, 60);
  //      SetBrightnessHard(52, 60);
  //      SetBrightnessHard(56, 60);
  //      SetBrightnessHard(60, 60);
  SIGNAL_CALLBACK_2(BacklightController, gboolean, SetBrightnessHard,
                    int64, int64);

  // Backlight used for dimming. Non-owned.
  BacklightInterface* backlight_;

  // Interface for saving preferences. Non-owned.
  PowerPrefsInterface* prefs_;

  // Light sensor we need to enable/disable on power events.  Non-owned.
  AmbientLightSensor* light_sensor_;

  // The brightness offset recommended by the light sensor.
  int64 als_brightness_level_;

  // Prevent small light sensor changes from updating the backlight.
  int64 als_hysteresis_level_;

  // User adjustable brightness offset when AC plugged.
  int64 plugged_brightness_offset_;

  // User adjustable brightness offset when AC unplugged.
  int64 unplugged_brightness_offset_;

  // Pointer to currently in-use user brightness offset.
  int64* brightness_offset_;

  // Backlight power state, used to distinguish between various cases.
  // Backlight nonzero, backlight zero, backlight idle-dimmed, etc.
  PowerState state_;

  // Whether the computer is plugged in.
  PluggedState plugged_state_;

  // Current system brightness, on local [0, 100] scale.
  int64 local_brightness_;

  // Min and max brightness for backlight object.
  int64 min_;
  int64 max_;

  // Minimum brightness as a percentage.
  int64 min_percent_;

  // Flag is set if a backlight device exists.
  bool is_initialized_;

  // The destination hardware brightness used for brightness transitions.
  int64 target_raw_brightness_;

  DISALLOW_COPY_AND_ASSIGN(BacklightController);
};

}  // namespace power_manager

#endif  // POWER_MANAGER_BACKLIGHT_CONTROLLER_H_
