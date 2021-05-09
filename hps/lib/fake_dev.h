// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/*
 * Fake device for HPS testing.
 */
#ifndef HPS_LIB_FAKE_DEV_H_
#define HPS_LIB_FAKE_DEV_H_

#include <memory>
#include <vector>

#include "hps/lib/dev.h"
#include "hps/lib/hps_reg.h"

namespace hps {

class DevImpl;

class FakeDev : public DevInterface {
 public:
  FakeDev();
  ~FakeDev();
  // Device interface
  bool Read(uint8_t cmd, uint8_t* data, size_t len) override;
  bool Write(uint8_t cmd, const uint8_t* data, size_t len) override;
  // Flags for controlling behaviour.
  enum Flags {
    kNone = 0,
    kBootFault = 1 << 0,
    kApplNotVerified = 1 << 1,
    kSpiNotVerified = 1 << 2,
    kWpOff = 1 << 3,
    kMemFail = 1 << 4,
    kSkipBoot = 1 << 5,
  };
  void Start(enum Flags flags);
  // TODO(amcrae): Add an interface to retrieve memory data written
  // to the device.
 private:
  std::unique_ptr<DevImpl> device_;
};

}  // namespace hps

#endif  // HPS_LIB_FAKE_DEV_H_
