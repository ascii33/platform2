// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device_manager.h"

#include <libudev.h>
#include <sys/stat.h>

#include <memory>
#include <set>

#include <base/bind.h>
#include <base/files/file_path.h>
#include <base/logging.h>
#include <base/memory/free_deleter.h>
#include <base/memory/ptr_util.h>
#include <base/stl_util.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/stringprintf.h>
#include <chromeos/dbus/service_constants.h>

#include "device_event_delegate.h"

namespace mtpd {

namespace {

// For GetObjectHandles PTP operations, this tells GetObjectHandles to only
// list the objects of the root of a store.
// Use this when referring to the root node in the context of ReadDirectory().
// This is an implementation detail that is not exposed to the outside.
const uint32_t kPtpGohRootParent = 0xFFFFFFFF;

// Used to identify a PTP USB device interface.
const char kPtpUsbInterfaceClass[] = "6";
const char kPtpUsbInterfaceSubClass[] = "1";
const char kPtpUsbInterfaceProtocol[] = "1";

// Used to identify a vendor-specific USB device interface.
// Manufacturers sometimes do not report MTP/PTP capable devices using the
// well known PTP interface class. See libgphoto2 and libmtp device databases
// for examples.
const char kVendorSpecificUsbInterfaceClass[] = "255";

const char kUsbPrefix[] = "usb";
const char kUDevEventType[] = "udev";
const char kUDevUsbSubsystem[] = "usb";

gboolean GlibRunClosure(gpointer data) {
  base::Closure* cb = reinterpret_cast<base::Closure*>(data);
  cb->Run();
  delete cb;
  return FALSE;
}

std::string RawDeviceToString(const LIBMTP_raw_device_t& device) {
  return base::StringPrintf("%s:%u,%d", kUsbPrefix, device.bus_location,
                            device.devnum);
}

std::string StorageToString(const std::string& usb_bus_str,
                            uint32_t storage_id) {
  return base::StringPrintf("%s:%u", usb_bus_str.c_str(), storage_id);
}

struct LibmtpFileDeleter {
  void operator()(LIBMTP_file_t* file) { LIBMTP_destroy_file_t(file); }
};

using ScopedMtpFile = std::unique_ptr<LIBMTP_file_t, LibmtpFileDeleter>;
ScopedMtpFile CreateScopedMtpFile(LIBMTP_mtpdevice_t* mtp_device,
                                  uint32_t file_id) {
  return ScopedMtpFile((file_id == kRootFileId)
                           ? LIBMTP_new_file_t()
                           : LIBMTP_Get_Filemetadata(mtp_device, file_id));
}

}  // namespace

DeviceManager::DeviceManager(DeviceEventDelegate* delegate)
    : udev_(udev_new()),
      udev_monitor_(nullptr),
      udev_monitor_fd_(-1),
      delegate_(delegate),
      weak_ptr_factory_(this) {
  // Set up udev monitoring.
  CHECK(delegate_);
  CHECK(udev_);
  udev_monitor_ = udev_monitor_new_from_netlink(udev_, kUDevEventType);
  CHECK(udev_monitor_);
  int ret = udev_monitor_filter_add_match_subsystem_devtype(
      udev_monitor_, kUDevUsbSubsystem, nullptr);
  CHECK_EQ(0, ret);
  ret = udev_monitor_enable_receiving(udev_monitor_);
  CHECK_EQ(0, ret);
  udev_monitor_fd_ = udev_monitor_get_fd(udev_monitor_);
  CHECK_GE(udev_monitor_fd_, 0);

  // Initialize libmtp.
  LIBMTP_Init();

  // Trigger a device scan.
  AddDevices(nullptr /* no callback source */);
}

DeviceManager::~DeviceManager() {
  udev_monitor_unref(udev_monitor_);
  udev_unref(udev_);
  RemoveDevices(true /* remove all */);
}

// static
bool DeviceManager::ParseStorageName(const std::string& storage_name,
                                     std::string* usb_bus_str,
                                     uint32_t* storage_id) {
  std::vector<std::string> split_str = base::SplitString(
      storage_name, ":", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  if (split_str.size() != 3)
    return false;

  if (split_str[0] != kUsbPrefix)
    return false;

  uint32_t id = 0;
  if (!base::StringToUint(split_str[2], &id))
    return false;

  *usb_bus_str = base::StringPrintf("%s:%s", kUsbPrefix, split_str[1].c_str());
  *storage_id = id;
  return true;
}

int DeviceManager::GetDeviceEventDescriptor() const {
  return udev_monitor_fd_;
}

void DeviceManager::ProcessDeviceEvents() {
  udev_device* dev = udev_monitor_receive_device(udev_monitor_);
  if (!dev)
    return;
  HandleDeviceNotification(dev);
  udev_device_unref(dev);
}

std::vector<std::string> DeviceManager::EnumerateStorages() {
  std::vector<std::string> ret;
  base::AutoLock al(device_map_lock_);
  for (const auto& device : device_map_) {
    const std::string& usb_bus_str = device.first;
    for (const auto& storage : device.second.storage_map) {
      std::string storage_str = StorageToString(usb_bus_str, storage.first);
      ret.push_back(storage_str);
      LOG(INFO) << "Found storage: " << storage_str;
    }
  }
  return ret;
}

bool DeviceManager::HasStorage(const std::string& storage_name) {
  return GetStorageInfo(storage_name) != nullptr;
}

const StorageInfo* DeviceManager::GetStorageInfo(
    const std::string& storage_name) {
  std::string usb_bus_str;
  uint32_t storage_id = 0;
  if (!ParseStorageName(storage_name, &usb_bus_str, &storage_id))
    return nullptr;

  base::AutoLock al(device_map_lock_);
  MtpDeviceMap::const_iterator device_it = device_map_.find(usb_bus_str);
  if (device_it == device_map_.end())
    return nullptr;

  const MtpStorageMap& storage_map = device_it->second.storage_map;
  MtpStorageMap::const_iterator storage_it = storage_map.find(storage_id);
  return storage_it != storage_map.end() ? &storage_it->second : nullptr;
}

const StorageInfo* DeviceManager::GetStorageInfoFromDevice(
    const std::string& storage_name) {
  std::string usb_bus_str;
  uint32_t storage_id = 0;
  if (!ParseStorageName(storage_name, &usb_bus_str, &storage_id))
    return nullptr;

  base::AutoLock al(device_map_lock_);
  MtpDeviceMap::iterator device_it = device_map_.find(usb_bus_str);
  if (device_it == device_map_.end())
    return nullptr;

  // Update |storage_map| with the latest storage info.
  MtpStorageMap& storage_map = device_it->second.storage_map;
  LIBMTP_mtpdevice_t* mtp_device = device_it->second.device;
  LIBMTP_Get_Storage(mtp_device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
  for (LIBMTP_devicestorage_t* storage = mtp_device->storage; storage;
       storage = storage->next) {
    MtpStorageMap::iterator storage_it = storage_map.find(storage->id);
    // If |storage->id| does not exist in the map, just ignore here. It should
    // be added at AddOrUpdateDevices.
    if (storage_it == storage_map.end())
      continue;

    storage_it->second.Update(*storage);
  }

  // Returns StorageInfo of |storage_id|.
  MtpStorageMap::const_iterator new_storage_it = storage_map.find(storage_id);
  return new_storage_it != storage_map.end() ? &new_storage_it->second
                                             : nullptr;
}

bool DeviceManager::ReadDirectoryEntryIds(const std::string& storage_name,
                                          uint32_t file_id,
                                          std::vector<uint32_t>* out) {
  LIBMTP_mtpdevice_t* mtp_device = nullptr;
  uint32_t storage_id = 0;
  if (!GetDeviceAndStorageId(storage_name, &mtp_device, &storage_id))
    return false;

  if (file_id == kRootFileId)
    file_id = kPtpGohRootParent;

  uint32_t* children;
  int ret = LIBMTP_Get_Children(mtp_device, storage_id, file_id, &children);
  if (ret < 0)
    return false;

  if (ret > 0) {
    for (int i = 0; i < ret; ++i)
      out->push_back(children[i]);
    free(children);
  }
  return true;
}

bool DeviceManager::GetFileInfo(const std::string& storage_name,
                                const std::vector<uint32_t> file_ids,
                                std::vector<FileEntry>* out) {
  LIBMTP_mtpdevice_t* mtp_device = nullptr;
  uint32_t storage_id = 0;
  if (!GetDeviceAndStorageId(storage_name, &mtp_device, &storage_id))
    return false;

  for (size_t i = 0; i < file_ids.size(); ++i) {
    uint32_t file_id = file_ids[i];
    auto file = CreateScopedMtpFile(mtp_device, file_id);
    if (!file)
      continue;

    // LIBMTP_Get_Filemetadata() does not know how to handle the root node, so
    // fill in relevant fields in the struct manually. The rest of the struct
    // has already been initialized by LIBMTP_new_file_t().
    if (file_id == kRootFileId) {
      file->storage_id = storage_id;
      file->filename = strdup("/");
      file->filetype = LIBMTP_FILETYPE_FOLDER;
    }

    out->push_back(FileEntry(*file));
  }
  return true;
}

bool DeviceManager::ReadFileChunk(const std::string& storage_name,
                                  uint32_t file_id,
                                  uint32_t offset,
                                  uint32_t count,
                                  std::vector<uint8_t>* out) {
  LIBMTP_mtpdevice_t* mtp_device = nullptr;
  uint32_t storage_id = 0;
  if (!GetDeviceAndStorageId(storage_name, &mtp_device, &storage_id))
    return false;
  return ReadFileChunk(mtp_device, file_id, offset, count, out);
}

bool DeviceManager::CopyFileFromLocal(const std::string& storage_name,
                                      const uint32_t file_descriptor,
                                      const uint32_t parent_id,
                                      const std::string& file_name) {
  // Get device.
  LIBMTP_mtpdevice_t* mtp_device = nullptr;
  uint32_t storage_id = 0;
  if (!GetDeviceAndStorageId(storage_name, &mtp_device, &storage_id))
    return false;

  // Get file size.
  struct stat file_stat;
  if (fstat(file_descriptor, &file_stat) != 0)
    return false;

  // Create a new file
  ScopedMtpFile new_file(LIBMTP_new_file_t());
  new_file->filename = strdup(file_name.c_str());
  new_file->filesize = file_stat.st_size;
  new_file->parent_id = parent_id;

  // Transfer a file.
  int transfer_status = LIBMTP_Send_File_From_File_Descriptor(
      mtp_device, file_descriptor, new_file.get(), nullptr, nullptr);
  return transfer_status == 0;
}

bool DeviceManager::DeleteObject(const std::string& storage_name,
                                 const uint32_t object_id) {
  // Get the device.
  LIBMTP_mtpdevice_t* mtp_device = nullptr;
  uint32_t storage_id = 0;
  if (!GetDeviceAndStorageId(storage_name, &mtp_device, &storage_id))
    return false;
  return DeleteObjectInternal(mtp_device, storage_id, object_id);
}

bool DeviceManager::RenameObject(const std::string& storage_name,
                                 const uint32_t object_id,
                                 const std::string& new_name) {
  // Get the device.
  LIBMTP_mtpdevice_t* mtp_device = nullptr;
  uint32_t storage_id = 0;
  if (!GetDeviceAndStorageId(storage_name, &mtp_device, &storage_id))
    return false;

  // The root node cannot be renamed.
  if (object_id == kRootFileId)
    return false;

  // Check the object exists.
  auto file = CreateScopedMtpFile(mtp_device, object_id);
  if (!file)
    return false;

  // Rename the object. While libmtp provides LIBMTP_Set_Folder_Name and other
  // methods for other types, they result in the same call of
  // set_object_filename.
  return LIBMTP_Set_File_Name(mtp_device, file.get(), new_name.c_str()) == 0;
}

bool DeviceManager::CreateDirectory(const std::string& storage_name,
                                    const uint32_t parent_id,
                                    const std::string& directory_name) {
  // Do not allow to create a directory with empty string.
  if (directory_name.empty())
    return false;

  // Get the device.
  LIBMTP_mtpdevice_t* mtp_device = nullptr;
  uint32_t storage_id = 0;
  if (!GetDeviceAndStorageId(storage_name, &mtp_device, &storage_id))
    return false;

  // Creates a directory.
  std::unique_ptr<char, base::FreeDeleter> new_directory_name(
      strdup(directory_name.c_str()));
  int new_directory_object_id = LIBMTP_Create_Folder(
      mtp_device, new_directory_name.get(), parent_id, storage_id);
  if (!strcmp(new_directory_name.get(), directory_name.c_str()))
    return new_directory_object_id > 0;

  // When directory name is changed, handle it as an error.
  if (new_directory_object_id > 0)
    DeleteObjectInternal(mtp_device, storage_id, new_directory_object_id);
  return false;
}

bool DeviceManager::AddStorageForTest(const std::string& storage_name,
                                      const StorageInfo& storage_info) {
  std::string device_location;
  uint32_t storage_id;
  if (!ParseStorageName(storage_name, &device_location, &storage_id))
    return false;

  base::AutoLock al(device_map_lock_);
  MtpDeviceMap::iterator it = device_map_.find(device_location);
  if (it == device_map_.end()) {
    // New device case.
    MtpStorageMap new_storage_map;
    new_storage_map.insert(std::make_pair(storage_id, storage_info));
    MtpDevice new_mtp_device(nullptr, new_storage_map, nullptr);
    device_map_.insert(std::make_pair(device_location, new_mtp_device));
    return true;
  }

  // Existing device case.
  // There should be no real LIBMTP_mtpdevice_t device for this dummy storage.
  MtpDevice& existing_mtp_device = it->second;
  if (existing_mtp_device.device)
    return false;

  // And the storage should not already exist.
  MtpStorageMap& existing_mtp_storage_map = existing_mtp_device.storage_map;
  if (ContainsKey(existing_mtp_storage_map, storage_id))
    return false;

  existing_mtp_storage_map.insert(std::make_pair(storage_id, storage_info));
  return true;
}

bool DeviceManager::ReadDirectory(LIBMTP_mtpdevice_t* device,
                                  uint32_t storage_id,
                                  uint32_t file_id,
                                  std::vector<FileEntry>* out) {
  LIBMTP_file_t* file =
      LIBMTP_Get_Files_And_Folders(device, storage_id, file_id);
  while (file) {
    ScopedMtpFile current_file(file);
    file = file->next;
    out->push_back(FileEntry(*current_file));
  }
  return true;
}

bool DeviceManager::ReadFileChunk(LIBMTP_mtpdevice_t* device,
                                  uint32_t file_id,
                                  uint32_t offset,
                                  uint32_t count,
                                  std::vector<uint8_t>* out) {
  // The root node is a virtual node and cannot be read from.
  if (file_id == kRootFileId)
    return false;

  uint8_t* data = nullptr;
  uint32_t bytes_read = 0;
  int transfer_status =
      LIBMTP_Get_File_Chunk(device, file_id, offset, count, &data, &bytes_read);

  // Own |data| in a scoper so it gets freed when this function returns.
  std::unique_ptr<uint8_t, base::FreeDeleter> scoped_data(data);

  if (transfer_status != 0 || bytes_read != count)
    return false;

  for (size_t i = 0; i < count; ++i)
    out->push_back(data[i]);
  return true;
}

bool DeviceManager::DeleteObjectInternal(LIBMTP_mtpdevice_t* mtp_device,
                                         const uint32_t storage_id,
                                         const uint32_t object_id) {
  // The root node cannot be deleted.
  if (object_id == kRootFileId)
    return false;

  // Check the object exists.
  auto file = CreateScopedMtpFile(mtp_device, object_id);
  if (!file)
    return false;

  // If the object is a directory, check it is empty.
  if (file->filetype == LIBMTP_FILETYPE_FOLDER) {
    uint32_t* children;
    int num_of_children =
        LIBMTP_Get_Children(mtp_device, storage_id, object_id, &children);
    if (num_of_children > 0)
      free(children);

    if (num_of_children != 0)
      return false;
  }

  // Delete an object.
  return LIBMTP_Delete_Object(mtp_device, object_id) == 0;
}

bool DeviceManager::GetFileInfoInternal(LIBMTP_mtpdevice_t* device,
                                        uint32_t storage_id,
                                        uint32_t file_id,
                                        FileEntry* out) {
  auto file = CreateScopedMtpFile(device, file_id);
  if (!file)
    return false;

  // LIBMTP_Get_Filemetadata() does not know how to handle the root node, so
  // fill in relevant fields in the struct manually. The rest of the struct has
  // already been initialized by LIBMTP_new_file_t().
  if (file_id == kRootFileId) {
    file->storage_id = storage_id;
    file->filename = strdup("/");
    file->filetype = LIBMTP_FILETYPE_FOLDER;
  }

  *out = FileEntry(*file);
  return true;
}

bool DeviceManager::GetDeviceAndStorageId(const std::string& storage_name,
                                          LIBMTP_mtpdevice_t** mtp_device,
                                          uint32_t* storage_id) {
  std::string usb_bus_str;
  uint32_t id = 0;
  if (!ParseStorageName(storage_name, &usb_bus_str, &id))
    return false;

  base::AutoLock al(device_map_lock_);
  MtpDeviceMap::const_iterator device_it = device_map_.find(usb_bus_str);
  if (device_it == device_map_.end())
    return false;

  if (!ContainsKey(device_it->second.storage_map, id))
    return false;

  *storage_id = id;
  *mtp_device = device_it->second.device;
  return true;
}

void DeviceManager::HandleDeviceNotification(udev_device* device) {
  const char* action = udev_device_get_property_value(device, "ACTION");
  const char* interface = udev_device_get_property_value(device, "INTERFACE");
  if (!action || !interface)
    return;

  // Check the USB interface. Since this gets called many times by udev for a
  // given physical action, use the udev "INTERFACE" event property as a quick
  // way of getting one unique and interesting udev event for a given physical
  // action. At the same time, do some light filtering and ignore events for
  // uninteresting devices.
  const std::string kEventInterface(interface);
  std::vector<std::string> split_usb_interface = base::SplitString(
      kEventInterface, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  if (split_usb_interface.size() != 3)
    return;

  // Check to see if the device has a vendor-specific interface class.
  // In this case, continue and let libmtp figure it out.
  const std::string& usb_interface_class = split_usb_interface[0];
  const std::string& usb_interface_subclass = split_usb_interface[1];
  const std::string& usb_interface_protocol = split_usb_interface[2];
  bool is_interesting_device =
      (usb_interface_class == kVendorSpecificUsbInterfaceClass);
  if (!is_interesting_device) {
    // Many MTP/PTP devices have this PTP interface.
    is_interesting_device =
        (usb_interface_class == kPtpUsbInterfaceClass &&
         usb_interface_subclass == kPtpUsbInterfaceSubClass &&
         usb_interface_protocol == kPtpUsbInterfaceProtocol);
  }
  if (!is_interesting_device)
    return;

  // Handle the action.
  const std::string kEventAction(action);
  if (kEventAction == "add") {
    // Some devices do not respond well when immediately probed. Thus there is
    // a 1 second wait here to give the device to settle down.
    GSource* source = g_timeout_source_new_seconds(1);
    auto cb = base::MakeUnique<base::Closure>(base::Bind(
        &DeviceManager::AddDevices, weak_ptr_factory_.GetWeakPtr(), source));
    g_source_set_callback(source, &GlibRunClosure, cb.release(), nullptr);
    g_source_attach(source, nullptr);
    return;
  }
  if (kEventAction == "remove") {
    RemoveDevices(false /* !remove_all */);
    return;
  }
  // udev notes the existence of other actions like "change" and "move", but
  // they have never been observed with real MTP/PTP devices in testing.
}

class MtpPollThread : public base::SimpleThread {
 public:
  explicit MtpPollThread(const base::Closure& cb)
      : SimpleThread("MTP polling"), callback_(cb) {}
  ~MtpPollThread() override {}

 private:
  void Run() override { callback_.Run(); }

  base::Closure callback_;

  DISALLOW_COPY_AND_ASSIGN(MtpPollThread);
};

void DeviceManager::PollDevice(LIBMTP_mtpdevice_t* mtp_device,
                               const std::string& usb_bus_name) {
  LIBMTP_event_t event;
  uint32_t extra;
  while (LIBMTP_Read_Event(mtp_device, &event, &extra) == 0) {
    if (event == LIBMTP_EVENT_STORE_ADDED ||
        event == LIBMTP_EVENT_STORE_REMOVED) {
      UpdateDevice(usb_bus_name);
    }
  }
}

void DeviceManager::AddDevices(GSource* source) {
  if (source) {
    // Matches g_source_attach().
    g_source_destroy(source);
    // Matches the implicit add-ref in g_timeout_source_new_seconds().
    g_source_unref(source);
  }
  AddOrUpdateDevices(true /* add */, "");
}

void DeviceManager::UpdateDevice(const std::string& usb_bus_name) {
  AddOrUpdateDevices(false /* update */, usb_bus_name);
}

void DeviceManager::AddOrUpdateDevices(
    bool add_update,
    const std::string& changed_usb_device_name) {
  base::AutoLock al(device_map_lock_);

  // Get raw devices.
  LIBMTP_raw_device_t* raw_devices = nullptr;
  int raw_devices_count = 0;
  LIBMTP_error_number_t err =
      LIBMTP_Detect_Raw_Devices(&raw_devices, &raw_devices_count);
  if (err != LIBMTP_ERROR_NONE) {
    LOG(ERROR) << "LIBMTP_Detect_Raw_Devices failed with " << err;
    return;
  }
  std::unique_ptr<LIBMTP_raw_device_t, base::FreeDeleter> scoped_raw_devices(
      raw_devices);
  // Iterate through raw devices. Look for target device, if updating.
  for (int i = 0; i < raw_devices_count; ++i) {
    const std::string usb_bus_str = RawDeviceToString(raw_devices[i]);

    if (add_update) {
      // Skip devices that have already been opened.
      if (ContainsKey(device_map_, usb_bus_str))
        continue;
    } else {
      // Skip non-target device.
      if (usb_bus_str != changed_usb_device_name)
        continue;
    }

    LIBMTP_mtpdevice_t* mtp_device = nullptr;
    if (add_update) {
      // Open the mtp device.
      mtp_device = LIBMTP_Open_Raw_Device_Uncached(&raw_devices[i]);
      if (!mtp_device) {
        LOG(ERROR) << "LIBMTP_Open_Raw_Device_Uncached failed for "
                   << usb_bus_str;
        continue;
      }
    } else {
      mtp_device = device_map_[usb_bus_str].device;

      // For existing devices, update the storage lists.
      if (LIBMTP_Get_Storage(mtp_device, LIBMTP_STORAGE_SORTBY_NOTSORTED) < 0) {
        LOG(ERROR) << "LIBMTP_Get_Storage failed for " << usb_bus_str;
        return;
      }
    }

    // Fetch fallback vendor / product info.
    std::unique_ptr<char, base::FreeDeleter> duplicated_string;
    duplicated_string.reset(LIBMTP_Get_Manufacturername(mtp_device));
    std::string fallback_vendor;
    if (duplicated_string.get())
      fallback_vendor = duplicated_string.get();

    duplicated_string.reset(LIBMTP_Get_Modelname(mtp_device));
    std::string fallback_product;
    if (duplicated_string.get())
      fallback_product = duplicated_string.get();

    MtpStorageMap new_storage_map;
    MtpStorageMap* storage_map_ptr;
    if (add_update)
      storage_map_ptr = &new_storage_map;
    else
      storage_map_ptr = &device_map_[usb_bus_str].storage_map;

    // Compute the set of storage ids that are contained in the mtpd's
    // storage_map but not in the latest device info. They are removed storages.
    std::set<uint32_t> removed_storage_ids;
    for (const auto& it : *storage_map_ptr)
      removed_storage_ids.insert(it.first);

    for (LIBMTP_devicestorage_t* storage = mtp_device->storage; storage;
         storage = storage->next) {
      removed_storage_ids.erase(storage->id);
    }
    // Iterate through storages on the device and remove storages that are no
    // longer on the device.
    for (const auto& storage_id : removed_storage_ids) {
      storage_map_ptr->erase(storage_id);
      delegate_->StorageDetached(StorageToString(usb_bus_str, storage_id));
    }

    // Iterate through storages on the device and add any that are missing.
    for (LIBMTP_devicestorage_t* storage = mtp_device->storage; storage;
         storage = storage->next) {
      if (ContainsKey(*storage_map_ptr, storage->id))
        continue;
      const std::string storage_name =
          StorageToString(usb_bus_str, storage->id);
      StorageInfo info(storage_name, raw_devices[i].device_entry, *storage,
                       fallback_vendor, fallback_product);
      bool storage_added =
          storage_map_ptr->insert(std::make_pair(storage->id, info)).second;
      CHECK(storage_added);
      delegate_->StorageAttached(storage_name);
      LOG(INFO) << "Added storage " << storage_name;
    }
    if (!add_update) {
      LOG(INFO) << "Updated device " << usb_bus_str << " with "
                << storage_map_ptr->size() << " storages";
      return;
    }

    base::Closure callback(base::Bind(&DeviceManager::PollDevice,
                                      base::Unretained(this), mtp_device,
                                      usb_bus_str));
    auto p_thread = base::MakeUnique<MtpPollThread>(callback);
    p_thread->Start();
    bool device_added =
        device_map_
            .insert(std::make_pair(
                usb_bus_str,
                MtpDevice(mtp_device, *storage_map_ptr, p_thread.release())))
            .second;
    CHECK(device_added);
    LOG(INFO) << "Added device " << usb_bus_str << " with "
              << storage_map_ptr->size() << " storages";
  }
}

void DeviceManager::RemoveDevices(bool remove_all) {
  LIBMTP_raw_device_t* raw_devices = nullptr;
  int raw_devices_count = 0;

  if (!remove_all) {
    LIBMTP_error_number_t err =
        LIBMTP_Detect_Raw_Devices(&raw_devices, &raw_devices_count);
    if (!(err == LIBMTP_ERROR_NONE || err == LIBMTP_ERROR_NO_DEVICE_ATTACHED)) {
      LOG(ERROR) << "LIBMTP_Detect_Raw_Devices failed with " << err;
      return;
    }
  }

  base::AutoLock al(device_map_lock_);
  // Populate |devices_set| with all known attached devices.
  std::set<std::string> devices_set;
  for (const auto& device : device_map_)
    devices_set.insert(device.first);

  // And remove the ones that are still attached.
  for (int i = 0; i < raw_devices_count; ++i)
    devices_set.erase(RawDeviceToString(raw_devices[i]));

  // The ones left in the set are the detached devices.
  for (const auto& device : devices_set) {
    LOG(INFO) << "Removed " << device;
    MtpDeviceMap::iterator device_it = device_map_.find(device);
    if (device_it == device_map_.end()) {
      NOTREACHED();
      continue;
    }

    // Remove all the storages on that device.
    const std::string& usb_bus_str = device_it->first;
    const MtpStorageMap& storage_map = device_it->second.storage_map;
    for (const auto& storage : storage_map) {
      delegate_->StorageDetached(StorageToString(usb_bus_str, storage.first));
    }

    // Delete the device's map entry and cleanup.
    LIBMTP_mtpdevice_t* mtp_device = device_it->second.device;
    linked_ptr<base::SimpleThread> p_thread(device_it->second.watcher_thread);
    device_map_.erase(device_it);

    // |mtp_device| can be NULL in testing.
    if (!mtp_device)
      continue;

    // When |remove_all| is false, the device has already been detached
    // and this runs after the fact. As such, this call will very
    // likely fail and spew a bunch of error messages. Call it anyway to
    // let libmtp do any cleanup it can.
    LIBMTP_Release_Device(mtp_device);

    // This shouldn't block now.
    p_thread.get()->Join();
  }
}

}  // namespace mtpd
