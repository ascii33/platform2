// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRASH_REPORTER_CRASH_COLLECTOR_TEST_H_
#define CRASH_REPORTER_CRASH_COLLECTOR_TEST_H_

#include "crash-reporter/crash_collector.h"

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

class CrashCollectorMock : public CrashCollector {
 public:
  CrashCollectorMock();
  MOCK_METHOD0(SetUpDBus, void());
};

#endif  // CRASH_REPORTER_CRASH_COLLECTOR_TEST_H_
