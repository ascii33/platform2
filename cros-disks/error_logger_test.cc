// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cros-disks/error_logger.h"

#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace cros_disks {
namespace {

static_assert(!FORMAT_ERROR_NONE);
static_assert(!MOUNT_ERROR_NONE);
static_assert(!PARTITION_ERROR_NONE);
static_assert(!RENAME_ERROR_NONE);

template <typename T>
std::string ToString(T error) {
  std::ostringstream out;
  out << error << std::flush;
  return out.str();
}

TEST(ErrorLogger, FormatErrorType) {
  EXPECT_EQ(ToString(FORMAT_ERROR_NONE), "FORMAT_ERROR_NONE");
  EXPECT_EQ(ToString(FORMAT_ERROR_UNKNOWN), "FORMAT_ERROR_UNKNOWN");
  EXPECT_EQ(ToString(FORMAT_ERROR_INTERNAL), "FORMAT_ERROR_INTERNAL");
  EXPECT_EQ(ToString(FORMAT_ERROR_INVALID_DEVICE_PATH),
            "FORMAT_ERROR_INVALID_DEVICE_PATH");
  EXPECT_EQ(ToString(FORMAT_ERROR_DEVICE_BEING_FORMATTED),
            "FORMAT_ERROR_DEVICE_BEING_FORMATTED");
  EXPECT_EQ(ToString(FORMAT_ERROR_UNSUPPORTED_FILESYSTEM),
            "FORMAT_ERROR_UNSUPPORTED_FILESYSTEM");
  EXPECT_EQ(ToString(FORMAT_ERROR_FORMAT_PROGRAM_NOT_FOUND),
            "FORMAT_ERROR_FORMAT_PROGRAM_NOT_FOUND");
  EXPECT_EQ(ToString(FORMAT_ERROR_FORMAT_PROGRAM_FAILED),
            "FORMAT_ERROR_FORMAT_PROGRAM_FAILED");
  EXPECT_EQ(ToString(FORMAT_ERROR_DEVICE_NOT_ALLOWED),
            "FORMAT_ERROR_DEVICE_NOT_ALLOWED");
  EXPECT_EQ(ToString(FORMAT_ERROR_INVALID_OPTIONS),
            "FORMAT_ERROR_INVALID_OPTIONS");
  EXPECT_EQ(ToString(FORMAT_ERROR_LONG_NAME), "FORMAT_ERROR_LONG_NAME");
  EXPECT_EQ(ToString(FORMAT_ERROR_INVALID_CHARACTER),
            "FORMAT_ERROR_INVALID_CHARACTER");
  EXPECT_EQ(ToString(FormatErrorType(987654)), "FORMAT_ERROR_987654");
}

TEST(ErrorLogger, MountErrorType) {
  EXPECT_EQ(ToString(MOUNT_ERROR_NONE), "MOUNT_ERROR_NONE");
  EXPECT_EQ(ToString(MOUNT_ERROR_UNKNOWN), "MOUNT_ERROR_UNKNOWN");
  EXPECT_EQ(ToString(MOUNT_ERROR_INTERNAL), "MOUNT_ERROR_INTERNAL");
  EXPECT_EQ(ToString(MOUNT_ERROR_INVALID_ARGUMENT),
            "MOUNT_ERROR_INVALID_ARGUMENT");
  EXPECT_EQ(ToString(MOUNT_ERROR_INVALID_PATH), "MOUNT_ERROR_INVALID_PATH");
  EXPECT_EQ(ToString(MOUNT_ERROR_PATH_ALREADY_MOUNTED),
            "MOUNT_ERROR_PATH_ALREADY_MOUNTED");
  EXPECT_EQ(ToString(MOUNT_ERROR_PATH_NOT_MOUNTED),
            "MOUNT_ERROR_PATH_NOT_MOUNTED");
  EXPECT_EQ(ToString(MOUNT_ERROR_DIRECTORY_CREATION_FAILED),
            "MOUNT_ERROR_DIRECTORY_CREATION_FAILED");
  EXPECT_EQ(ToString(MOUNT_ERROR_INVALID_MOUNT_OPTIONS),
            "MOUNT_ERROR_INVALID_MOUNT_OPTIONS");
  EXPECT_EQ(ToString(MOUNT_ERROR_INVALID_UNMOUNT_OPTIONS),
            "MOUNT_ERROR_INVALID_UNMOUNT_OPTIONS");
  EXPECT_EQ(ToString(MOUNT_ERROR_INSUFFICIENT_PERMISSIONS),
            "MOUNT_ERROR_INSUFFICIENT_PERMISSIONS");
  EXPECT_EQ(ToString(MOUNT_ERROR_MOUNT_PROGRAM_NOT_FOUND),
            "MOUNT_ERROR_MOUNT_PROGRAM_NOT_FOUND");
  EXPECT_EQ(ToString(MOUNT_ERROR_MOUNT_PROGRAM_FAILED),
            "MOUNT_ERROR_MOUNT_PROGRAM_FAILED");
  EXPECT_EQ(ToString(MOUNT_ERROR_NEED_PASSWORD), "MOUNT_ERROR_NEED_PASSWORD");
  EXPECT_EQ(ToString(MOUNT_ERROR_IN_PROGRESS), "MOUNT_ERROR_IN_PROGRESS");
  EXPECT_EQ(ToString(MOUNT_ERROR_CANCELLED), "MOUNT_ERROR_CANCELLED");
  EXPECT_EQ(ToString(MOUNT_ERROR_INVALID_DEVICE_PATH),
            "MOUNT_ERROR_INVALID_DEVICE_PATH");
  EXPECT_EQ(ToString(MOUNT_ERROR_UNKNOWN_FILESYSTEM),
            "MOUNT_ERROR_UNKNOWN_FILESYSTEM");
  EXPECT_EQ(ToString(MOUNT_ERROR_UNSUPPORTED_FILESYSTEM),
            "MOUNT_ERROR_UNSUPPORTED_FILESYSTEM");
  EXPECT_EQ(ToString(MOUNT_ERROR_INVALID_ARCHIVE),
            "MOUNT_ERROR_INVALID_ARCHIVE");
  EXPECT_EQ(ToString(MOUNT_ERROR_UNSUPPORTED_ARCHIVE),
            "MOUNT_ERROR_UNSUPPORTED_ARCHIVE");
  EXPECT_EQ(ToString(MountErrorType(987654)), "MOUNT_ERROR_987654");
}

TEST(ErrorLogger, PartitionErrorType) {
  EXPECT_EQ(ToString(PARTITION_ERROR_NONE), "PARTITION_ERROR_NONE");
  EXPECT_EQ(ToString(PARTITION_ERROR_UNKNOWN), "PARTITION_ERROR_UNKNOWN");
  EXPECT_EQ(ToString(PARTITION_ERROR_INTERNAL), "PARTITION_ERROR_INTERNAL");
  EXPECT_EQ(ToString(PARTITION_ERROR_INVALID_DEVICE_PATH),
            "PARTITION_ERROR_INVALID_DEVICE_PATH");
  EXPECT_EQ(ToString(PARTITION_ERROR_DEVICE_BEING_PARTITIONED),
            "PARTITION_ERROR_DEVICE_BEING_PARTITIONED");
  EXPECT_EQ(ToString(PARTITION_ERROR_PROGRAM_NOT_FOUND),
            "PARTITION_ERROR_PROGRAM_NOT_FOUND");
  EXPECT_EQ(ToString(PARTITION_ERROR_PROGRAM_FAILED),
            "PARTITION_ERROR_PROGRAM_FAILED");
  EXPECT_EQ(ToString(PARTITION_ERROR_DEVICE_NOT_ALLOWED),
            "PARTITION_ERROR_DEVICE_NOT_ALLOWED");
  EXPECT_EQ(ToString(PartitionErrorType(987654)), "PARTITION_ERROR_987654");
}

TEST(ErrorLogger, RenameErrorType) {
  EXPECT_EQ(ToString(RENAME_ERROR_NONE), "RENAME_ERROR_NONE");
  EXPECT_EQ(ToString(RENAME_ERROR_UNKNOWN), "RENAME_ERROR_UNKNOWN");
  EXPECT_EQ(ToString(RENAME_ERROR_INTERNAL), "RENAME_ERROR_INTERNAL");
  EXPECT_EQ(ToString(RENAME_ERROR_INVALID_DEVICE_PATH),
            "RENAME_ERROR_INVALID_DEVICE_PATH");
  EXPECT_EQ(ToString(RENAME_ERROR_DEVICE_BEING_RENAMED),
            "RENAME_ERROR_DEVICE_BEING_RENAMED");
  EXPECT_EQ(ToString(RENAME_ERROR_UNSUPPORTED_FILESYSTEM),
            "RENAME_ERROR_UNSUPPORTED_FILESYSTEM");
  EXPECT_EQ(ToString(RENAME_ERROR_RENAME_PROGRAM_NOT_FOUND),
            "RENAME_ERROR_RENAME_PROGRAM_NOT_FOUND");
  EXPECT_EQ(ToString(RENAME_ERROR_RENAME_PROGRAM_FAILED),
            "RENAME_ERROR_RENAME_PROGRAM_FAILED");
  EXPECT_EQ(ToString(RENAME_ERROR_DEVICE_NOT_ALLOWED),
            "RENAME_ERROR_DEVICE_NOT_ALLOWED");
  EXPECT_EQ(ToString(RENAME_ERROR_LONG_NAME), "RENAME_ERROR_LONG_NAME");
  EXPECT_EQ(ToString(RENAME_ERROR_INVALID_CHARACTER),
            "RENAME_ERROR_INVALID_CHARACTER");
  EXPECT_EQ(ToString(RenameErrorType(987654)), "RENAME_ERROR_987654");
}

}  // namespace
}  // namespace cros_disks
