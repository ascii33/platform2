// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_UTILS_CBI_UTILS_H_
#define RMAD_UTILS_CBI_UTILS_H_

#include <string>

namespace rmad {

class CbiUtils {
 public:
  CbiUtils() = default;
  virtual ~CbiUtils() = default;

  // Set a (tag, value) pair to CBI. Return true if successfully set the value,
  // false if fail to set the value.
  virtual bool SetCbi(int tag, const std::string& value, int set_flag) = 0;

  // Get the value associated with tag |tag| in CBI, and store it to |value|
  // with a string. Return true if successfully get the value, false if fail to
  // get the value.
  virtual bool GetCbi(int tag, std::string* value, int get_flag) const = 0;

  // Set a (tag, value, size) tuple to CBI. Return true if successfully set the
  // value, false if fail to set the value.
  virtual bool SetCbi(int tag, uint64_t value, int size, int set_flag) = 0;

  // Get the value associated with tag |tag| in CBI, and store it to |value|
  // with a uint64_t. Return true if successfully get the value, false if fail
  // to get the value.
  virtual bool GetCbi(int tag, uint64_t* value, int get_flag) const = 0;
};

}  // namespace rmad

#endif  // RMAD_UTILS_CBI_UTILS_H_
