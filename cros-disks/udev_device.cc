// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cros-disks/udev_device.h"

#include <fcntl.h>
#include <libudev.h>
#include <linux/limits.h>
#include <stdlib.h>
#include <sys/statvfs.h>

#include <base/logging.h>
#include <base/sha1.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>
#include <rootdev/rootdev.h>

#include "cros-disks/mount_info.h"
#include "cros-disks/usb_device_info.h"

namespace {

const char kNullDeviceFile[] = "/dev/null";
const char kAttributeIdProduct[] = "idProduct";
const char kAttributeIdVendor[] = "idVendor";
const char kAttributePartition[] = "partition";
const char kAttributeRange[] = "range";
const char kAttributeReadOnly[] = "ro";
const char kAttributeRemovable[] = "removable";
const char kAttributeSize[] = "size";
const char kPropertyBlkIdFilesystemType[] = "TYPE";
const char kPropertyBlkIdFilesystemLabel[] = "LABEL";
const char kPropertyBlkIdFilesystemUUID[] = "UUID";
const char kPropertyCDROM[] = "ID_CDROM";
const char kPropertyCDROMDVD[] = "ID_CDROM_DVD";
const char kPropertyCDROMMedia[] = "ID_CDROM_MEDIA";
const char kPropertyCDROMMediaTrackCountData[] =
    "ID_CDROM_MEDIA_TRACK_COUNT_DATA";
const char kPropertyDeviceType[] = "DEVTYPE";
const char kPropertyDeviceTypeUSBDevice[] = "usb_device";
const char kPropertyFilesystemUsage[] = "ID_FS_USAGE";
const char kPropertyMistSupportedDevice[] = "MIST_SUPPORTED_DEVICE";
const char kPropertyMmcType[] = "MMC_TYPE";
const char kPropertyMmcTypeSd[] = "SD";
const char kPropertyModel[] = "ID_MODEL";
const char kPropertyPartitionEntryType[] = "ID_PART_ENTRY_TYPE";
const char kPropertyPartitionSize[] = "UDISKS_PARTITION_SIZE";
const char kPropertyPresentationHide[] = "UDISKS_PRESENTATION_HIDE";
const char kPropertyRotationRate[] = "ID_ATA_ROTATION_RATE_RPM";
const char kPropertySerial[] = "ID_SERIAL";
const char kSubsystemUsb[] = "usb";
const char kVirtualDevicePathPrefix[] = "/sys/devices/virtual/";
const char kLoopDevicePathPrefix[] = "/sys/devices/virtual/block/loop";
const char kUSBDeviceInfoFile[] = "/usr/share/cros-disks/usb-device-info";
const char kUSBIdentifierDatabase[] = "/usr/share/misc/usb.ids";
const char* const kPartitionTypesToHide[] = {
    "c12a7328-f81f-11d2-ba4b-00a0c93ec93b",  // EFI system partition
    "fe3a2a5d-4f32-41a7-b725-accc3285a309",  // Chrome OS kernel
    "3cb8e202-3b7e-47dd-8a3c-7ff2a13cfcec",  // Chrome OS root filesystem
    "cab6e88e-abf3-4102-a07a-d4bb9be3c1d3",  // Chrome OS firmware
    "2e0a753d-9e48-43b0-8337-b15192cb1b5e",  // Chrome OS reserved
};

}  // namespace

namespace cros_disks {

UdevDevice::UdevDevice(udev_device* dev) : dev_(dev), blkid_cache_(nullptr) {
  CHECK(dev_) << "Invalid udev device";
  udev_device_ref(dev_);
}

UdevDevice::~UdevDevice() {
  if (blkid_cache_) {
    // It needs to call blkid_put_cache to deallocate the blkid cache.
    blkid_put_cache(blkid_cache_);
  }
  udev_device_unref(dev_);
}

// static
std::string UdevDevice::EnsureUTF8String(const std::string& str) {
  return base::IsStringUTF8(str) ? str : "";
}

// static
bool UdevDevice::IsValueBooleanTrue(const char* value) {
  return value && strcmp(value, "1") == 0;
}

std::string UdevDevice::GetAttribute(const char* key) const {
  const char* value = udev_device_get_sysattr_value(dev_, key);
  return (value) ? value : "";
}

bool UdevDevice::IsAttributeTrue(const char* key) const {
  const char* value = udev_device_get_sysattr_value(dev_, key);
  return IsValueBooleanTrue(value);
}

bool UdevDevice::HasAttribute(const char* key) const {
  const char* value = udev_device_get_sysattr_value(dev_, key);
  return value != nullptr;
}

std::string UdevDevice::GetProperty(const char* key) const {
  const char* value = udev_device_get_property_value(dev_, key);
  return (value) ? value : "";
}

bool UdevDevice::IsPropertyTrue(const char* key) const {
  const char* value = udev_device_get_property_value(dev_, key);
  return IsValueBooleanTrue(value);
}

bool UdevDevice::HasProperty(const char* key) const {
  const char* value = udev_device_get_property_value(dev_, key);
  return value != nullptr;
}

std::string UdevDevice::GetPropertyFromBlkId(const char* key) {
  std::string value;
  const char* dev_file = udev_device_get_devnode(dev_);
  if (dev_file) {
    // No cache file is used as it should always query information from
    // the device, i.e. setting cache file to /dev/null.
    if (blkid_cache_ || blkid_get_cache(&blkid_cache_, kNullDeviceFile) == 0) {
      blkid_dev dev = blkid_get_dev(blkid_cache_, dev_file, BLKID_DEV_NORMAL);
      if (dev) {
        char* tag_value = blkid_get_tag_value(blkid_cache_, key, dev_file);
        if (tag_value) {
          value = tag_value;
          free(tag_value);
        }
      }
    }
  }
  return value;
}

void UdevDevice::GetSizeInfo(uint64_t* total_size,
                             uint64_t* remaining_size) const {
  static const int kSectorSize = 512;
  uint64_t total = 0, remaining = 0;

  // If the device is mounted, obtain the total and remaining size in bytes
  // using statvfs.
  std::vector<std::string> mount_paths = GetMountPaths();
  if (!mount_paths.empty()) {
    struct statvfs stat;
    if (statvfs(mount_paths[0].c_str(), &stat) == 0) {
      total = stat.f_blocks * stat.f_frsize;
      remaining = stat.f_bfree * stat.f_frsize;
    }
  }

  // If the UDISKS_PARTITION_SIZE property is set, use it as the total size
  // instead. If the UDISKS_PARTITION_SIZE property is not set but sysfs
  // provides a size value, which is the actual size in bytes divided by 512,
  // use that as the total size instead.
  const char* partition_size =
      udev_device_get_property_value(dev_, kPropertyPartitionSize);
  int64_t size = 0;
  if (partition_size) {
    base::StringToInt64(partition_size, &size);
    total = size;
  } else {
    const char* size_attr = udev_device_get_sysattr_value(dev_, kAttributeSize);
    if (size_attr) {
      base::StringToInt64(size_attr, &size);
      total = size * kSectorSize;
    }
  }

  if (total_size)
    *total_size = total;
  if (remaining_size)
    *remaining_size = remaining;
}

size_t UdevDevice::GetPartitionCount() const {
  size_t partition_count = 0;
  const char* dev_file = udev_device_get_devnode(dev_);
  if (dev_file) {
    blkid_probe probe = blkid_new_probe_from_filename(dev_file);
    if (probe) {
      blkid_partlist partitions = blkid_probe_get_partitions(probe);
      if (partitions) {
        partition_count = blkid_partlist_numof_partitions(partitions);
      }
      blkid_free_probe(probe);
    }
  }
  return partition_count;
}

DeviceMediaType UdevDevice::GetDeviceMediaType() const {
  if (IsPropertyTrue(kPropertyCDROMDVD))
    return DEVICE_MEDIA_DVD;

  if (IsPropertyTrue(kPropertyCDROM))
    return DEVICE_MEDIA_OPTICAL_DISC;

  if (IsOnSdDevice())
    return DEVICE_MEDIA_SD;

  std::string vendor_id, product_id;
  if (GetVendorAndProductId(&vendor_id, &product_id)) {
    USBDeviceInfo info;
    info.RetrieveFromFile(kUSBDeviceInfoFile);
    return info.GetDeviceMediaType(vendor_id, product_id);
  }
  return DEVICE_MEDIA_UNKNOWN;
}

bool UdevDevice::GetVendorAndProductId(std::string* vendor_id,
                                       std::string* product_id) const {
  // Search up the parent device tree to obtain the vendor and product ID
  // of the first device with a device type "usb_device". Then look up the
  // media type based on the vendor and product ID from a USB device info file.
  for (udev_device* dev = dev_; dev; dev = udev_device_get_parent(dev)) {
    const char* device_type =
        udev_device_get_property_value(dev, kPropertyDeviceType);
    if (device_type && strcmp(device_type, kPropertyDeviceTypeUSBDevice) == 0) {
      const char* vendor_id_attr =
          udev_device_get_sysattr_value(dev, kAttributeIdVendor);
      const char* product_id_attr =
          udev_device_get_sysattr_value(dev, kAttributeIdProduct);
      if (vendor_id_attr && product_id_attr) {
        *vendor_id = vendor_id_attr;
        *product_id = product_id_attr;
        return true;
      }
    }
  }
  return false;
}

bool UdevDevice::IsMediaAvailable() const {
  bool is_media_available = true;
  if (IsAttributeTrue(kAttributeRemovable)) {
    if (IsPropertyTrue(kPropertyCDROM)) {
      is_media_available = IsPropertyTrue(kPropertyCDROMMedia);
    } else {
      const char* dev_file = udev_device_get_devnode(dev_);
      if (dev_file) {
        int fd = open(dev_file, O_RDONLY);
        if (fd < 0) {
          is_media_available = false;
        } else {
          close(fd);
        }
      }
    }
  }
  return is_media_available;
}

bool UdevDevice::IsMobileBroadbandDevice() const {
  // Check if a parent device, which belongs to the "usb" subsystem and has a
  // device type "usb_device", has a property "MIST_SUPPORTED_DEVICE=1". If so,
  // it is a mobile broadband device supported by mist.
  udev_device* parent = udev_device_get_parent_with_subsystem_devtype(
      dev_, kSubsystemUsb, kPropertyDeviceTypeUSBDevice);
  if (!parent)
    return false;

  const char* value =
      udev_device_get_property_value(parent, kPropertyMistSupportedDevice);
  return IsValueBooleanTrue(value);
}

bool UdevDevice::IsAutoMountable() const {
  // TODO(benchan): Find a reliable way to detect if a device is a removable
  // storage as the removable attribute in sysfs does not always tell the truth.
  return !IsOnBootDevice() && !IsVirtual();
}

bool UdevDevice::IsHidden() {
  if (IsPropertyTrue(kPropertyPresentationHide))
    return true;

  // Hide an optical disc without any data track.
  // udev/cdrom_id only sets ID_CDROM_MEDIA_TRACK_COUNT_DATA when there is at
  // least one data track.
  if (IsPropertyTrue(kPropertyCDROM) &&
      !HasProperty(kPropertyCDROMMediaTrackCountData)) {
    return true;
  }

  // Hide a mobile broadband device, which may initially expose itself as a USB
  // mass storage device and later be switched to a modem by mist.
  if (IsMobileBroadbandDevice())
    return true;

  // Hide a device that is neither marked as a partition nor a filesystem,
  // unless it has no valid partitions (e.g. the device is unformatted or
  // corrupted). An unformatted or corrupted device is visible in the file
  // the file browser so that we can provide a way to format it.
  if (!HasAttribute(kAttributePartition) &&
      !HasProperty(kPropertyFilesystemUsage) && (GetPartitionCount() > 0))
    return true;

  // Hide special partitions based on partition type.
  std::string partition_type = GetProperty(kPropertyPartitionEntryType);
  if (!partition_type.empty()) {
    for (const char* partition_type_to_hide : kPartitionTypesToHide) {
      if (partition_type == partition_type_to_hide)
        return true;
    }
  }
  return false;
}

bool UdevDevice::IsIgnored() const {
  return IsVirtual() && !IsLoopDevice();
}

bool UdevDevice::IsOnBootDevice() const {
  // Obtain the boot device path, e.g. /dev/sda
  char boot_device_path[PATH_MAX];
  if (rootdev(boot_device_path, sizeof(boot_device_path), true, true)) {
    LOG(ERROR) << "Could not determine root device";
    // Assume it is on the boot device when there is any uncertainty.
    // This is to prevent a device, which is potentially on the boot device,
    // from being auto mounted and exposed to users.
    // TODO(benchan): Find a way to eliminate the uncertainty.
    return true;
  }

  // Compare the device file path of the current device and all its parents
  // with the boot device path. Any match indicates that the current device
  // is on the boot device.
  for (udev_device* dev = dev_; dev; dev = udev_device_get_parent(dev)) {
    const char* dev_file = udev_device_get_devnode(dev);
    if (dev_file) {
      if (strncmp(boot_device_path, dev_file, PATH_MAX) == 0) {
        return true;
      }
    }
  }
  return false;
}

bool UdevDevice::IsOnSdDevice() const {
  for (udev_device* dev = dev_; dev; dev = udev_device_get_parent(dev)) {
    const char* mmc_type =
        udev_device_get_property_value(dev, kPropertyMmcType);
    if (mmc_type && strcmp(mmc_type, kPropertyMmcTypeSd) == 0) {
      return true;
    }
  }
  return false;
}

bool UdevDevice::IsOnRemovableDevice() const {
  for (udev_device* dev = dev_; dev; dev = udev_device_get_parent(dev)) {
    const char* value = udev_device_get_sysattr_value(dev, kAttributeRemovable);
    if (IsValueBooleanTrue(value))
      return true;
  }
  return false;
}

bool UdevDevice::IsVirtual() const {
  const char* sys_path = udev_device_get_syspath(dev_);
  if (sys_path) {
    return base::StartsWith(sys_path, kVirtualDevicePathPrefix,
                            base::CompareCase::SENSITIVE);
  }
  // To be safe, mark it as virtual device if sys path cannot be determined.
  return true;
}

bool UdevDevice::IsLoopDevice() const {
  const char* sys_path = udev_device_get_syspath(dev_);
  if (sys_path) {
    return base::StartsWith(sys_path, kLoopDevicePathPrefix,
                            base::CompareCase::SENSITIVE);
  }
  return false;
}

std::string UdevDevice::NativePath() const {
  const char* sys_path = udev_device_get_syspath(dev_);
  return sys_path ? sys_path : "";
}

std::vector<std::string> UdevDevice::GetMountPaths() const {
  const char* device_path = udev_device_get_devnode(dev_);
  if (device_path) {
    return GetMountPaths(device_path);
  }
  return std::vector<std::string>();
}

std::vector<std::string> UdevDevice::GetMountPaths(
    const std::string& device_path) {
  MountInfo mount_info;
  if (mount_info.RetrieveFromCurrentProcess()) {
    return mount_info.GetMountPaths(device_path);
  }
  return std::vector<std::string>();
}

Disk UdevDevice::ToDisk() {
  Disk disk;

  disk.is_auto_mountable = IsAutoMountable();
  disk.is_read_only = IsAttributeTrue(kAttributeReadOnly);
  disk.is_drive = HasAttribute(kAttributeRange);
  disk.is_rotational = HasProperty(kPropertyRotationRate);
  disk.is_hidden = IsHidden();
  disk.is_media_available = IsMediaAvailable();
  disk.is_on_boot_device = IsOnBootDevice();
  disk.is_on_removable_device = IsOnRemovableDevice();
  disk.is_virtual = IsVirtual();
  disk.media_type = GetDeviceMediaType();
  disk.filesystem_type = GetPropertyFromBlkId(kPropertyBlkIdFilesystemType);
  disk.native_path = NativePath();

  // Drive model and filesystem label may not be UTF-8 encoded, so we
  // need to ensure that they are either set to a valid UTF-8 string or
  // an empty string before later passed to a DBus message iterator.
  disk.drive_model = EnsureUTF8String(GetProperty(kPropertyModel));
  disk.label =
      EnsureUTF8String(GetPropertyFromBlkId(kPropertyBlkIdFilesystemLabel));

  if (GetVendorAndProductId(&disk.vendor_id, &disk.product_id)) {
    USBDeviceInfo info;
    info.GetVendorAndProductName(kUSBIdentifierDatabase, disk.vendor_id,
                                 disk.product_id, &disk.vendor_name,
                                 &disk.product_name);
  }

  // TODO(benchan): Add a proper unit test when fixing crbug.com/221380.
  std::string uuid_hash = base::SHA1HashString(
      disk.vendor_id + disk.product_id + GetProperty(kPropertySerial) +
      GetPropertyFromBlkId(kPropertyBlkIdFilesystemUUID));
  disk.uuid = base::HexEncode(uuid_hash.data(), uuid_hash.size());

  const char* dev_file = udev_device_get_devnode(dev_);
  if (dev_file)
    disk.device_file = dev_file;

  disk.mount_paths = GetMountPaths();

  GetSizeInfo(&disk.device_capacity, &disk.bytes_remaining);

  return disk;
}

}  // namespace cros_disks
