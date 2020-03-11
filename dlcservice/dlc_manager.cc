// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dlcservice/dlc_manager.h"

#include <utility>
#include <vector>

#include <base/strings/stringprintf.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <brillo/message_loops/message_loop.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus/dlcservice/dbus-constants.h>

#include "dlcservice/system_state.h"
#include "dlcservice/utils.h"

using base::Callback;
using base::File;
using base::FilePath;
using std::string;
using std::unique_ptr;
using std::vector;

namespace dlcservice {

namespace {
// Timeout in ms for DBus method calls into imageloader.
constexpr int kImageLoaderTimeoutMs = 5000;
}  // namespace

const char kDlcMetadataActiveValue[] = "1";
// Keep kDlcMetadataFilePingActive in sync with update_engine's.
const char kDlcMetadataFilePingActive[] = "active";

class DlcManager::DlcManagerImpl {
 public:
  DlcManagerImpl() {
    const auto system_state = SystemState::Get();
    image_loader_proxy_ = system_state->image_loader();
    manifest_dir_ = system_state->manifest_dir();
    preloaded_content_dir_ = system_state->preloaded_content_dir();
    content_dir_ = system_state->content_dir();
    metadata_dir_ = system_state->metadata_dir();

    string boot_disk_name;
    if (!system_state->boot_slot().GetCurrentSlot(&boot_disk_name,
                                                  &current_boot_slot_))
      LOG(FATAL) << "Can not get current boot slot.";

    // Initialize supported DLC modules.
    supported_ = ScanDirectory(manifest_dir_);
  }
  ~DlcManagerImpl() = default;

  bool IsSupported(const DlcId& id) {
    return supported_.find(id) != supported_.end();
  }

  bool IsInstalling() { return !installing_.empty(); }

  DlcMap GetInstalled() {
    RefreshInstalled();
    return installed_;
  }

  bool GetState(const DlcId& id, DlcState* state) {
    // TODO(crbug.com/1059124): Implement state logic storing and error code
    // propagation.
    RefreshInstalled();
    if (installed_.find(id) != installed_.end())
      state->set_state(DlcState::INSTALLED);
    else if (installing_.find(id) != installing_.end())
      state->set_state(DlcState::INSTALLING);
    else
      state->set_state(DlcState::NOT_INSTALLED);
    return true;
  }

  void PreloadDlcModuleImages() { RefreshPreloaded(); }

  void LoadDlcModuleImages() { RefreshInstalled(); }

  bool InitInstall(const DlcMap& requested_install,
                   string* err_code,
                   string* err_msg) {
    CHECK(installing_.empty());
    RefreshInstalled();
    installing_ = requested_install;

    for (const auto& dlc : installing_) {
      const string& id = dlc.first;
      string throwaway_err_code, throwaway_err_msg;
      // If already installed, pick up the root.
      if (installed_.find(id) != installed_.end()) {
        installing_[id] = installed_[id];
      } else {
        if (!Create(id, err_code, err_msg)) {
          CancelInstall(&throwaway_err_code, &throwaway_err_msg);
          return false;
        }
      }
      // Failure to set the metadata flags should not fail the install.
      if (!SetActive(id, &throwaway_err_code, &throwaway_err_msg)) {
        LOG(WARNING) << throwaway_err_msg;
      }
    }
    return true;
  }

  DlcMap GetInstalling() {
    DlcMap required_installing;
    for (const auto& dlc : installing_)
      if (dlc.second.root.empty())
        required_installing[dlc.first];
    return required_installing;
  }

  bool FinishInstall(DlcMap* installed, string* err_code, string* err_msg) {
    *installed = installing_;

    ScopedCleanups<base::Callback<void()>> scoped_cleanups;

    for (const auto& dlc : installing_) {
      const auto& id = dlc.first;
      auto cleanup = base::Bind(
          [](Callback<bool()> unmounter, Callback<bool()> deleter,
             string* err_code, string* err_msg) {
            if (!unmounter.Run())
              LOG(ERROR) << *err_code << ":" << *err_msg;
            if (!deleter.Run())
              LOG(ERROR) << *err_code << ":" << *err_msg;
          },
          base::Bind(&DlcManagerImpl::Unmount, base::Unretained(this), id,
                     err_code, err_msg),
          base::Bind(&DlcManagerImpl::Delete, base::Unretained(this), id,
                     err_code, err_msg),
          err_code, err_msg);
      scoped_cleanups.Insert(cleanup);
    }
    scoped_cleanups.Insert(
        base::Bind(&DlcManagerImpl::ClearInstalling, base::Unretained(this)));

    for (auto& dlc : installing_) {
      const auto& id = dlc.first;
      const auto& info = dlc.second;
      if (!info.root.empty())
        continue;
      string mount_point;
      if (!Mount(id, &mount_point, err_code, err_msg))
        return false;
      dlc.second = DlcInfo(GetDlcRoot(FilePath(mount_point)).value());
    }

    scoped_cleanups.Cancel();

    for (const auto& dlc : installing_) {
      const auto& id = dlc.first;
      const auto& info = dlc.second;
      installed_[id] = (*installed)[id] = info;
    }

    ClearInstalling();
    return true;
  }

  bool CancelInstall(string* err_code, string* err_msg) {
    bool ret = true;
    if (installing_.empty()) {
      LOG(WARNING) << "No install started to being with, nothing to cancel.";
      return ret;
    }
    for (const auto& dlc : installing_) {
      const auto& id = dlc.first;
      const auto& info = dlc.second;
      if (!info.root.empty())
        continue;
      if (!Delete(id, err_code, err_msg)) {
        LOG(ERROR) << *err_msg;
        ret = false;
      }
    }
    ClearInstalling();
    return ret;
  }

  // Deletes all directories related to the given DLC |id|. If |err_code| or
  // |err_msg| are passed in, they will be set. Otherwise error will be logged.
  bool Delete(const string& id,
              string* err_code = nullptr,
              string* err_msg = nullptr) {
    vector<string> undeleted_paths;
    for (const auto& path :
         {JoinPaths(content_dir_, id), JoinPaths(metadata_dir_, id)}) {
      if (!base::DeleteFile(path, true))
        undeleted_paths.push_back(path.value());
    }
    installed_.erase(id);
    bool ret = undeleted_paths.empty();
    if (!ret) {
      string local_err_code = kErrorInternal;
      string local_err_msg =
          base::StringPrintf("DLC directories (%s) could not be deleted.",
                             base::JoinString(undeleted_paths, ",").c_str());
      if (err_code)
        *err_code = std::move(local_err_code);
      if (err_msg)
        *err_msg = std::move(local_err_msg);
      if (!err_code && !err_msg)
        LOG(ERROR) << local_err_code << "|" << local_err_msg;
    }
    return ret;
  }

  bool Mount(const string& id,
             string* mount_point,
             string* err_code,
             string* err_msg) {
    if (!image_loader_proxy_->LoadDlcImage(
            id, GetDlcPackage(id),
            current_boot_slot_ == BootSlot::Slot::A ? imageloader::kSlotNameA
                                                    : imageloader::kSlotNameB,
            mount_point, nullptr, kImageLoaderTimeoutMs)) {
      *err_code = kErrorInternal;
      *err_msg = "Imageloader is unavailable.";
      return false;
    }
    if (mount_point->empty()) {
      *err_code = kErrorInternal;
      *err_msg = "Imageloader LoadDlcImage() call failed.";
      return false;
    }
    return true;
  }

  bool Unmount(const string& id, string* err_code, string* err_msg) {
    bool success = false;
    if (!image_loader_proxy_->UnloadDlcImage(id, GetDlcPackage(id), &success,
                                             nullptr, kImageLoaderTimeoutMs)) {
      *err_code = kErrorInternal;
      *err_msg = "Imageloader is unavailable.";
      return false;
    }
    if (!success) {
      *err_code = kErrorInternal;
      *err_msg = "Imageloader UnloadDlcImage() call failed for DLC: " + id;
      return false;
    }
    return true;
  }

 private:
  string GetDlcPackage(const string& id) {
    return *(ScanDirectory(JoinPaths(manifest_dir_, id)).begin());
  }

  void ClearInstalling() { installing_.clear(); }

  // Returns true if the DLC module has a boolean true for 'preload-allowed'
  // attribute in the manifest for the given |id| and |package|.
  bool IsDlcPreloadAllowed(const base::FilePath& dlc_manifest_path,
                           const std::string& id) {
    imageloader::Manifest manifest;
    if (!GetDlcManifest(dlc_manifest_path, id, GetDlcPackage(id), &manifest)) {
      // Failing to read the manifest will be considered a preloading blocker.
      return false;
    }
    return manifest.preload_allowed();
  }

  bool CreateMetadata(const std::string& id,
                      string* err_code,
                      string* err_msg) {
    // Create the DLC ID metadata directory with correct permissions if it
    // doesn't exist.
    FilePath metadata_path_local = JoinPaths(metadata_dir_, id);
    if (!base::PathExists(metadata_path_local)) {
      if (!CreateDir(metadata_path_local)) {
        *err_code = kErrorInternal;
        *err_msg = "Failed to create the DLC metadata directory for DLC:" + id;
        return false;
      }
    }
    return true;
  }

  bool SetActive(const string& id, string* err_code, string* err_msg) {
    // Create the metadata directory if it doesn't exist.
    if (!CreateMetadata(id, err_code, err_msg)) {
      LOG(ERROR) << err_msg;
      return false;
    }
    auto active_metadata_path =
        JoinPaths(metadata_dir_, id, kDlcMetadataFilePingActive);
    if (!WriteToFile(active_metadata_path, kDlcMetadataActiveValue)) {
      *err_code = kErrorInternal;
      *err_msg = "Failed to write into active metadata file: " +
                 active_metadata_path.value();
      return false;
    }
    return true;
  }

  // Create the DLC |id| and |package| directories if they don't exist.
  bool CreateDlcPackagePath(const string& id,
                            const string& package,
                            string* err_code,
                            string* err_msg) {
    FilePath content_path_local = JoinPaths(content_dir_, id);
    FilePath content_package_path = JoinPaths(content_dir_, id, package);

    // Create the DLC ID directory with correct permissions.
    if (!CreateDir(content_path_local)) {
      *err_code = kErrorInternal;
      *err_msg = "Failed to create DLC (" + id + ") directory";
      return false;
    }
    // Create the DLC package directory with correct permissions.
    if (!CreateDir(content_package_path)) {
      *err_code = kErrorInternal;
      *err_msg = "Failed to create DLC (" + id + ") package directory";
      return false;
    }
    return true;
  }

  bool Create(const string& id, string* err_code, string* err_msg) {
    CHECK(err_code);
    CHECK(err_msg);

    if (!IsSupported(id)) {
      *err_code = kErrorInvalidDlc;
      *err_msg = "The DLC (" + id + ") provided is not supported.";
      return false;
    }

    const string& package = GetDlcPackage(id);
    FilePath content_path_local = JoinPaths(content_dir_, id);

    if (base::PathExists(content_path_local)) {
      *err_code = kErrorInternal;
      *err_msg = "The DLC (" + id + ") is installed or duplicate.";
      return false;
    }

    if (!CreateDlcPackagePath(id, package, err_code, err_msg))
      return false;

    // Creates DLC module storage.
    // TODO(xiaochu): Manifest currently returns a signed integer, which means
    // it will likely fail for modules >= 2 GiB in size.
    // https://crbug.com/904539
    imageloader::Manifest manifest;
    if (!dlcservice::GetDlcManifest(manifest_dir_, id, package, &manifest)) {
      *err_code = kErrorInternal;
      *err_msg = "Failed to create DLC (" + id + ") manifest.";
      return false;
    }
    int64_t image_size = manifest.preallocated_size();
    if (image_size <= 0) {
      *err_code = kErrorInternal;
      *err_msg = "Preallocated size in manifest is illegal: " +
                 base::Int64ToString(image_size);
      return false;
    }

    // Creates image A.
    FilePath image_a_path =
        GetDlcImagePath(content_dir_, id, package, BootSlot::Slot::A);
    if (!CreateFile(image_a_path, image_size)) {
      *err_code = kErrorAllocation;
      *err_msg = "Failed to create slot A DLC (" + id + ") image file.";
      return false;
    }

    // Creates image B.
    FilePath image_b_path =
        GetDlcImagePath(content_dir_, id, package, BootSlot::Slot::B);
    if (!CreateFile(image_b_path, image_size)) {
      *err_code = kErrorAllocation;
      *err_msg = "Failed to create slot B DLC (" + id + ") image file.";
      return false;
    }

    return true;
  }

  // Validate that:
  //  - [1] Inactive image for a |dlc_id| exists and create it if missing.
  //    -> Failure to do so returns false.
  //  - [2] Active and inactive images both are the same size and try fixing for
  //        certain scenarios after update only.
  //    -> Failure to do so only logs error.
  bool ValidateImageFiles(const string& id, string* err_code, string* err_msg) {
    string mount_point;
    const auto& package = GetDlcPackage(id);
    FilePath inactive_img_path = GetDlcImagePath(
        content_dir_, id, package,
        current_boot_slot_ == BootSlot::Slot::A ? BootSlot::Slot::B
                                                : BootSlot::Slot::A);

    imageloader::Manifest manifest;
    if (!dlcservice::GetDlcManifest(manifest_dir_, id, package, &manifest)) {
      return false;
    }
    int64_t max_allowed_img_size = manifest.preallocated_size();

    // [1]
    if (!base::PathExists(inactive_img_path)) {
      LOG(WARNING) << "The DLC image " << inactive_img_path.value()
                   << " does not exist.";
      if (!CreateDlcPackagePath(id, package, err_code, err_msg))
        return false;
      if (!CreateFile(inactive_img_path, max_allowed_img_size)) {
        // Don't make this error |kErrorAllocation|, this is during a refresh
        // and should be considered and internal problem of keeping DLC(s) in a
        // completely valid state.
        *err_code = kErrorInternal;
        *err_msg = "Failed to create DLC image during validation: " +
                   inactive_img_path.value();
        return false;
      }
    }

    // Different scenarios possible to hit this flow:
    //  - Inactive and manifest size are the same -> Do nothing.
    //
    // TODO(crbug.com/943780): This requires further design updates to both
    //  dlcservice and upate_engine in order to fully handle. Solution pending.
    //  - Update applied and not rebooted -> Do nothing. A lot more corner cases
    //    than just always keeping active and inactive image sizes the same.
    //
    //  - Update applied and rebooted -> Try fixing up inactive image.
    // [2]
    int64_t inactive_img_size;
    if (!base::GetFileSize(inactive_img_path, &inactive_img_size)) {
      LOG(ERROR) << "Failed to get DLC (" << id << ") size.";
    } else {
      // When |inactive_img_size| is less than the size permitted in the
      // manifest, this means that we rebooted into an update.
      if (inactive_img_size < max_allowed_img_size) {
        // Only increasing size, the inactive DLC is still usable in case of
        // reverts.
        if (!ResizeFile(inactive_img_path, max_allowed_img_size)) {
          LOG(ERROR)
              << "Failed to increase inactive image, update_engine may "
                 "face problems in updating when stateful is full later.";
        }
      }
    }

    return true;
  }

  // Helper used by |RefreshPreload()| to load in (copy + cleanup) preloadable
  // files for the given DLC ID.
  bool RefreshPreloadedCopier(const string& id) {
    const auto& package = GetDlcPackage(id);
    FilePath image_preloaded_path =
        JoinPaths(preloaded_content_dir_, id, package, kDlcImageFileName);
    FilePath image_a_path =
        GetDlcImagePath(content_dir_, id, package, BootSlot::Slot::A);
    FilePath image_b_path =
        GetDlcImagePath(content_dir_, id, package, BootSlot::Slot::B);

    // Check the size of file to copy is valid.
    imageloader::Manifest manifest;
    if (!dlcservice::GetDlcManifest(manifest_dir_, id, package, &manifest)) {
      LOG(ERROR) << "Failed to get DLC (" << id << " module manifest.";
      return false;
    }
    int64_t max_allowed_image_size = manifest.preallocated_size();
    // Scope the |image_preloaded| file so it always closes before deleting.
    {
      int64_t image_preloaded_size;
      if (!base::GetFileSize(image_preloaded_path, &image_preloaded_size)) {
        LOG(ERROR) << "Failed to get preloaded DLC (" << id << ") size.";
        return false;
      }
      if (image_preloaded_size > max_allowed_image_size) {
        LOG(ERROR) << "Preloaded DLC (" << id << ") is ("
                   << image_preloaded_size
                   << ") larger than the preallocated size ("
                   << max_allowed_image_size << ") in manifest.";
        return false;
      }
    }

    // Based on |current_boot_slot_|, copy the preloadable image.
    FilePath image_boot_path, image_non_boot_path;
    switch (current_boot_slot_) {
      case BootSlot::Slot::A:
        image_boot_path = image_a_path;
        image_non_boot_path = image_b_path;
        break;
      case BootSlot::Slot::B:
        image_boot_path = image_b_path;
        image_non_boot_path = image_a_path;
        break;
      default:
        NOTREACHED();
    }
    // TODO(kimjae): when preloaded images are place into unencrypted, this
    // operation can be a move.
    if (!CopyAndResizeFile(image_preloaded_path, image_boot_path,
                           max_allowed_image_size)) {
      LOG(ERROR) << "Failed to preload DLC (" << id << ") into boot slot.";
      return false;
    }

    return true;
  }

  // Loads the preloadable DLC(s) from |preloaded_content_dir_| by scanning the
  // preloaded DLC(s) and verifying the validity to be preloaded before doing
  // so.
  void RefreshPreloaded() {
    string err_code, err_msg;
    // Load all preloaded DLC modules into |content_dir_| one by one.
    for (auto id : ScanDirectory(preloaded_content_dir_)) {
      if (!IsDlcPreloadAllowed(manifest_dir_, id)) {
        LOG(ERROR) << "Preloading for DLC (" << id << ") is not allowed.";
        continue;
      }

      DlcMap dlc_map = {{id, DlcInfo()}};
      if (!InitInstall(dlc_map, &err_code, &err_msg)) {
        LOG(ERROR) << "Failed to create DLC (" << id << ") for preloading.";
        continue;
      }

      if (!RefreshPreloadedCopier(id)) {
        LOG(ERROR) << "Something went wrong during preloading DLC (" << id
                   << "), please check for previous errors.";
        CancelInstall(&err_code, &err_msg);
        continue;
      }

      // When the copying is successful, go ahead and finish installation.
      if (!FinishInstall(&dlc_map, &err_code, &err_msg)) {
        LOG(ERROR) << "Failed to |FinishInstall()| preloaded DLC (" << id
                   << ") "
                   << "because: " << err_code << "|" << err_msg;
        continue;
      }

      // Delete the preloaded DLC only after both copies into A and B succeed as
      // well as mounting.
      auto image_preloaded_path = JoinPaths(
          preloaded_content_dir_, id, GetDlcPackage(id), kDlcImageFileName);
      if (!base::DeleteFile(image_preloaded_path.DirName().DirName(), true)) {
        LOG(ERROR) << "Failed to delete preloaded DLC (" << id << ").";
        continue;
      }
    }
  }

  // A refresh mechanism that keeps installed DLC(s), |installed_|, in check.
  // Provides correction to DLC(s) that may have been altered by non-internal
  // actions.
  void RefreshInstalled() {
    decltype(installed_) verified_installed;

    // Recheck installed DLC modules.
    for (auto id : ScanDirectory(content_dir_)) {
      if (!IsSupported(id)) {
        LOG(ERROR) << "Found unsupported DLC (" << id
                   << ") installed, will delete.";
        Delete(id);
        continue;
      }
      string err_code, err_msg;
      auto info = installed_[id];
      // Create the metadata directory if it doesn't exist.
      if (!CreateMetadata(id, &err_code, &err_msg))
        LOG(WARNING) << err_code << "|" << err_msg;
      // Validate images are in a good state.
      if (!ValidateImageFiles(id, &err_code, &err_msg)) {
        LOG(ERROR) << "Failed to validate DLC (" << id
                   << ") during refresh: " << err_code << "|" << err_msg;
        Delete(id);
      }
      // - If the root is empty and is currently installing then skip.
      // - If the root exists set it and continue.
      // - Try mounting, if mounted set it and continue.
      // - Remove the DLC if none of the previous checks are met.
      string mount;
      if (info.root.empty() && installing_.find(id) != installing_.end()) {
        continue;
      } else if (base::PathExists(base::FilePath(info.root))) {
        verified_installed[id] = info;
      } else if (Mount(id, &mount, &err_code, &err_msg)) {
        verified_installed[id] = DlcInfo(GetDlcRoot(FilePath(mount)).value());
      } else {
        LOG(ERROR) << "Failed to mount DLC (" << id
                   << ") during refresh: " << err_code << "|" << err_msg;
        Delete(id);
      }
    }
    installed_ = std::move(verified_installed);
  }

  org::chromium::ImageLoaderInterfaceProxyInterface* image_loader_proxy_;

  FilePath manifest_dir_;
  FilePath preloaded_content_dir_;
  FilePath content_dir_;
  FilePath metadata_dir_;

  BootSlot::Slot current_boot_slot_;

  string installing_omaha_url_;
  DlcMap installing_;
  DlcMap installed_;
  DlcSet supported_;
};

DlcManager::DlcManager() {
  impl_ = std::make_unique<DlcManagerImpl>();
}

DlcManager::~DlcManager() = default;

bool DlcManager::IsInstalling() {
  return impl_->IsInstalling();
}

DlcModuleList DlcManager::GetInstalled() {
  return ToDlcModuleList(impl_->GetInstalled(),
                         [](DlcId, DlcInfo) { return true; });
}

bool DlcManager::GetState(const DlcId& id,
                          DlcState* state,
                          std::string* err_code,
                          std::string* err_msg) {
  CHECK(err_code);
  CHECK(err_msg);

  if (!impl_->IsSupported(id)) {
    *err_code = kErrorInvalidDlc;
    *err_msg = "Can not get state of unsupported DLC: " + id;
    return false;
  }

  return impl_->GetState(id, state);
}

void DlcManager::LoadDlcModuleImages() {
  impl_->PreloadDlcModuleImages();
  impl_->LoadDlcModuleImages();
}

bool DlcManager::InitInstall(const DlcModuleList& dlc_module_list,
                             string* err_code,
                             string* err_msg) {
  CHECK(err_code);
  CHECK(err_msg);
  DlcMap dlc_map =
      ToDlcMap(dlc_module_list, [](DlcModuleInfo) { return true; });

  if (dlc_map.empty()) {
    *err_code = kErrorInvalidDlc;
    *err_msg = "Must provide at lease one DLC to install.";
    return false;
  }

  return impl_->InitInstall(dlc_map, err_code, err_msg);
}

DlcModuleList DlcManager::GetMissingInstalls() {
  // Only return the DLC(s) that aren't already installed.
  return ToDlcModuleList(impl_->GetInstalling(),
                         [](DlcId, DlcInfo info) { return info.root.empty(); });
}

bool DlcManager::FinishInstall(DlcModuleList* dlc_module_list,
                               string* err_code,
                               string* err_msg) {
  CHECK(dlc_module_list);
  CHECK(err_code);
  CHECK(err_msg);

  DlcMap dlc_map;
  if (!impl_->FinishInstall(&dlc_map, err_code, err_msg))
    return false;

  *dlc_module_list = ToDlcModuleList(dlc_map, [](DlcId id, DlcInfo info) {
    CHECK(!id.empty());
    CHECK(!info.root.empty());
    return true;
  });
  return true;
}

bool DlcManager::CancelInstall(std::string* err_code, std::string* err_msg) {
  return impl_->CancelInstall(err_code, err_msg);
}

bool DlcManager::Delete(const string& id,
                        std::string* err_code,
                        std::string* err_msg) {
  CHECK(err_code);
  CHECK(err_msg);

  if (!impl_->IsSupported(id)) {
    *err_code = kErrorInvalidDlc;
    *err_msg = "Trying to delete DLC (" + id + ") which isn't supported.";
    return false;
  }
  auto installed_dlcs = impl_->GetInstalled();
  if (installed_dlcs.find(id) == installed_dlcs.end()) {
    LOG(WARNING) << "Uninstalling DLC (" << id << ") that's not installed.";
    return true;
  }
  if (!impl_->Unmount(id, err_code, err_msg))
    return false;
  if (!impl_->Delete(id, err_code, err_msg))
    return false;
  return true;
}

}  // namespace dlcservice
