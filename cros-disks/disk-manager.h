// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CROS_DISKS_DISK_MANAGER_H_
#define CROS_DISKS_DISK_MANAGER_H_

#include <blkid/blkid.h>
#include <libudev.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include <base/basictypes.h>
#include <gtest/gtest_prod.h>

#include "cros-disks/device-event.h"
#include "cros-disks/mount-manager.h"

namespace cros_disks {

class Disk;
class Filesystem;
class Mounter;
class Platform;
class UdevDevice;

// The DiskManager is responsible for reading device state from udev.
// Said changes could be the result of a udev notification or a synchronous
// call to enumerate the relevant storage devices attached to the system.
//
// Sample Usage:
//
// Platform platform;
// DiskManager manager("/media/removable", &platform);
// manager.Initialize();
// manager.EnumerateDisks();
// select(manager.udev_monitor_fd())...
//
// This class is designed to run within a single-threaded GMainLoop application
// and should not be considered thread safe.
class DiskManager : public MountManager {
 public:
  DiskManager(const std::string& mount_root, Platform* platform);
  virtual ~DiskManager();

  // Initializes the disk manager and registers default filesystems.
  // Returns true on success.
  virtual bool Initialize();

  // Returns true if mounting |source_path| is supported.
  virtual bool CanMount(const std::string& source_path) const;

  // Returns the type of mount sources supported by the manager.
  virtual MountSourceType GetMountSourceType() const {
    return kMountSourceRemovableDevice;
  }

  // Lists the current block devices attached to the system.
  virtual std::vector<Disk> EnumerateDisks() const;

  // Reads the changes from udev and converts the changes into a device event.
  // Returns true on success. Must be called to clear the fd.
  bool GetDeviceEvent(DeviceEvent* event);

  // Gets a Disk object that corresponds to a given device file.
  bool GetDiskByDevicePath(const std::string& device_path, Disk *disk) const;

  // Gets the filesystem type of a device.
  std::string GetFilesystemTypeOfDevice(const std::string& device_path);

  // Registers a set of default filesystems to the disk manager.
  void RegisterDefaultFilesystems();

  // Registers a filesystem to the disk manager.
  // Subsequent registrations of the same filesystem type are ignored.
  void RegisterFilesystem(const Filesystem& filesystem);

  // A file descriptor that can be select()ed or poll()ed for system changes.
  int udev_monitor_fd() const { return udev_monitor_fd_; }

 protected:
  // Mounts |source_path| to |mount_path| as |filesystem_type| with |options|.
  virtual MountErrorType DoMount(const std::string& source_path,
                                 const std::string& filesystem_type,
                                 const std::vector<std::string>& options,
                                 const std::string& mount_path);

  // Unmounts |path| with |options|.
  virtual MountErrorType DoUnmount(const std::string& path,
                                   const std::vector<std::string>& options);

  // Returns a suggested mount path for a source path.
  virtual std::string SuggestMountPath(const std::string& source_path) const;

 private:
  // Creates an appropriate mounter object for a given filesystem.
  // The caller is responsible for deleting the mounter object.
  Mounter* CreateMounter(const Disk& disk, const Filesystem& filesystem,
                         const std::string& target_path,
                         const std::vector<std::string>& options) const;

  // Returns a Filesystem object if a given filesystem type is supported.
  // Otherwise, it returns NULL.
  const Filesystem* GetFilesystem(const std::string& filesystem_type) const;

  // Determines a device/disk event from a udev block device change.
  DeviceEvent::EventType ProcessBlockDeviceEvent(const UdevDevice& device,
                                                 const char *action);

  // Determines a device/disk event from a udev SCSI device change.
  DeviceEvent::EventType ProcessScsiDeviceEvent(const UdevDevice& device,
                                                const char *action);

  // The root udev object.
  mutable struct udev* udev_;

  // Provides access to udev changes as they occur.
  struct udev_monitor* udev_monitor_;

  // A file descriptor that indicates changes to the system.
  int udev_monitor_fd_;

  // blkid_cache object.
  blkid_cache blkid_cache_;

  // A set of device sysfs paths detected by the udev monitor.
  std::set<std::string> devices_detected_;

  // A set of disk sysfs paths detected by the udev monitor.
  std::set<std::string> disks_detected_;

  // A set of supported filesystems indexed by filesystem type.
  std::map<std::string, Filesystem> filesystems_;

  FRIEND_TEST(DiskManagerTest, CreateExternalMounter);
  FRIEND_TEST(DiskManagerTest, CreateNTFSMounter);
  FRIEND_TEST(DiskManagerTest, CreateSystemMounter);
  FRIEND_TEST(DiskManagerTest, GetFilesystem);
  FRIEND_TEST(DiskManagerTest, RegisterFilesystem);
  FRIEND_TEST(DiskManagerTest, DoMountDiskWithNonexistentSourcePath);
  FRIEND_TEST(DiskManagerTest, DoUnmountDiskWithInvalidUnmountOptions);

  DISALLOW_COPY_AND_ASSIGN(DiskManager);
};

}  // namespace cros_disks

#endif  // CROS_DISKS_DISK_MANAGER_H_
