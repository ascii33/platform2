// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DISK_H__
#define DISK_H__

#include <base/basictypes.h>
#include <dbus-c++/dbus.h>  // NOLINT
#include <map>
#include <string>
#include <vector>

namespace cros_disks {

typedef std::map<std::string, DBus::Variant> DBusDisk;
typedef std::vector<DBusDisk> DBusDisks;

// A simple type that describes a storage device attached to our system.
//
// This class was designed to run in a single threaded context and should not
// be considered thread safe.
class Disk {
 public:

  Disk();
  virtual ~Disk();

  bool is_drive() const { return is_drive_; }
  void set_is_drive(bool is_drive) { is_drive_ = is_drive; }

  bool is_hidden() const { return is_hidden_; }
  void set_is_hidden(bool is_hidden) { is_hidden_ = is_hidden; }
  
  bool is_mounted() const { return is_mounted_; }
  void set_is_mounted(bool is_mounted) { is_mounted_ = is_mounted; }

  bool is_media_available() const { return is_media_available_; }
  void set_is_media_available(bool is_media_available) {
    is_media_available_ = is_media_available;
  }

  bool is_rotational() const { return is_rotational_; }
  void set_is_rotational(bool is_rotational) { is_rotational_ = is_rotational; }

  bool is_optical_disk() const { return is_optical_disk_; }
  void set_is_optical_disk(bool is_optical_disk) { 
    is_optical_disk_ = is_optical_disk; 
  }

  bool is_read_only() const { return is_read_only_; }
  void set_is_read_only(bool is_read_only) { is_read_only_ = is_read_only; }

  std::string mount_path() const { return mount_path_; }
  void set_mount_path(const std::string& mount_path) { mount_path_ = mount_path; }

  std::string native_path() const { return native_path_; }
  void set_native_path(const std::string& native_path) {
    native_path_ = native_path;
  }

  std::string device_file() const { return device_file_; }
  void set_device_file(const std::string& device_file) {
    device_file_ = device_file;
  }
  
  std::string label() const { return label_; }
  void set_label(const std::string& label) { label_ = label; }

  std::string drive_model() const { return drive_model_; }
  void set_drive_model(const std::string& drive_model) {
    drive_model_ = drive_model; 
  }

  uint64 device_capacity() const { return device_capacity_; }
  void set_device_capacity(uint64 device_capacity) {
    device_capacity_ = device_capacity; 
  }

  uint64 bytes_remaining() { return bytes_remaining_; }
  void set_bytes_remaining(uint64 bytes_remaining) { 
    bytes_remaining_ = bytes_remaining; 
  }

 private:

  bool is_drive_;
  bool is_hidden_;
  bool is_mounted_;
  bool is_media_available_;
  bool is_rotational_;
  bool is_optical_disk_;
  bool is_read_only_;
  std::string mount_path_;
  std::string native_path_;
  std::string device_file_;
  std::string label_;
  std::string drive_model_;
  uint64 device_capacity_;
  uint64 bytes_remaining_;
};

} // namespace cros_disks


#endif // DISK_H__
