// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CROS_DISKS_SESSION_MANAGER_OBSERVER_H_
#define CROS_DISKS_SESSION_MANAGER_OBSERVER_H_

#include <string>

namespace cros_disks {

// An interface class for observing events from the session manager.
// A derived class of this class should override the event methods
// that it would like to observe.
class SessionManagerObserver {
 public:
  virtual ~SessionManagerObserver() {}

  // This method is called when a session has started.
  virtual void OnSessionStarted(const std::string& user) = 0;

  // This method is called when a session has stopped.
  virtual void OnSessionStopped(const std::string& user) = 0;
};

}  // namespace cros_disks

#endif  // CROS_DISKS_SESSION_MANAGER_OBSERVER_H_
