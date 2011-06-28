// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CROS_DISKS_POWER_MANAGER_OBSERVER_H_
#define CROS_DISKS_POWER_MANAGER_OBSERVER_H_

namespace cros_disks {

// An interface class for observing events from the power manager.
// A derived class of this class should override the event methods
// that it would like to observe.
class PowerManagerObserver {
 public:
  virtual ~PowerManagerObserver() {}

  // This method is called when the screen is locked.
  virtual void OnScreenIsLocked() = 0;

  // This method is called when the screen is unlocked.
  virtual void OnScreenIsUnlocked() = 0;
};

}  // namespace cros_disks

#endif  // CROS_DISKS_POWER_MANAGER_OBSERVER_H_
