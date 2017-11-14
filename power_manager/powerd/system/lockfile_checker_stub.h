// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_SYSTEM_LOCKFILE_CHECKER_STUB_H_
#define POWER_MANAGER_POWERD_SYSTEM_LOCKFILE_CHECKER_STUB_H_

#include "power_manager/powerd/system/lockfile_checker.h"

#include <vector>

#include <base/files/file_path.h>

namespace power_manager {
namespace system {

// LockfileCheckerStub is a stub implementation of LockfileCheckerInterface for
// use by tests.
class LockfileCheckerStub : public LockfileCheckerInterface {
 public:
  LockfileCheckerStub();
  ~LockfileCheckerStub() override;

  void set_files_to_return(const std::vector<base::FilePath>& files) {
    files_to_return_ = files;
  }

  // LockfileChecker:
  std::vector<base::FilePath> GetValidLockfiles() const override;

 private:
  // Paths to be returned by GetValidLockfiles().
  std::vector<base::FilePath> files_to_return_;

  DISALLOW_COPY_AND_ASSIGN(LockfileCheckerStub);
};

}  // namespace system
}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_SYSTEM_LOCKFILE_CHECKER_STUB_H_
