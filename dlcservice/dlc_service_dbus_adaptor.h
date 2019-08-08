// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DLCSERVICE_DLC_SERVICE_DBUS_ADAPTOR_H_
#define DLCSERVICE_DLC_SERVICE_DBUS_ADAPTOR_H_

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/files/file_path.h>
#include <base/memory/weak_ptr.h>
#include <dlcservice/proto_bindings/dlcservice.pb.h>
#include <imageloader/dbus-proxies.h>
#include <update_engine/proto_bindings/update_engine.pb.h>
#include <update_engine/dbus-proxies.h>

#include "dlcservice/dbus_adaptors/org.chromium.DlcServiceInterface.h"
#include "dlcservice/types.h"

namespace dlcservice {

class BootSlot;

// DlcServiceDBusAdaptor is a D-Bus adaptor that manages life-cycles of DLCs
// (Downloadable Content) and provides an API for the rest of the system to
// install/uninstall DLCs.
class DlcServiceDBusAdaptor
    : public org::chromium::DlcServiceInterfaceInterface,
      public org::chromium::DlcServiceInterfaceAdaptor {
 public:
  DlcServiceDBusAdaptor(
      std::unique_ptr<org::chromium::ImageLoaderInterfaceProxyInterface>
          image_loader_proxy,
      std::unique_ptr<org::chromium::UpdateEngineInterfaceProxyInterface>
          update_engine_proxy,
      std::unique_ptr<BootSlot> boot_slot,
      const base::FilePath& manifest_dir,
      const base::FilePath& content_dir);
  ~DlcServiceDBusAdaptor();

  // Loads installed DLC module images.
  void LoadDlcModuleImages();

  // org::chromium::DlServiceInterfaceInterface overrides:
  bool Install(brillo::ErrorPtr* err,
               const DlcModuleList& dlc_module_list_in) override;
  bool Uninstall(brillo::ErrorPtr* err, const std::string& id_in) override;
  bool GetInstalled(brillo::ErrorPtr* err,
                    DlcModuleList* dlc_module_list_out) override;

  // Called on receiving update_engine's |StatusUpdate| signal.
  void OnStatusUpdateAdvancedSignal(
      const update_engine::StatusResult& status_result);

 private:
  // Returns indication of update_engine's install completion.
  bool InstallingComplete(const update_engine::StatusResult& status_result);

  // Creates the necessary directories and images for DLC installation. Will set
  // |path| to the top DLC directory for cleanup scoping.
  bool CreateDlc(brillo::ErrorPtr* err,
                 const std::string& id,
                 base::FilePath* path);

  // Deletes the DLC installation.
  bool DeleteDlc(brillo::ErrorPtr* err, const std::string& id);

  // Tries to mount DLC images.
  bool MountDlc(brillo::ErrorPtr* err,
                const std::string& id,
                std::string* mount_point);

  // Tries to unmount DLC images.
  bool UnmountDlc(brillo::ErrorPtr* err, const std::string& id);

  // Scans a specific DLC |id| to discover all its packages. Currently, we only
  // support one package per DLC. If at some point in the future we decided to
  // support multiple packages, then appropriate changes to this function is
  // granted.
  std::string ScanDlcModulePackage(const std::string& id);

  // Checks if Update Engine is in a state among |status_list|.
  bool CheckForUpdateEngineStatus(
      const std::vector<update_engine::Operation>& status_list);

  // Send |OnInstalled| D-Bus signal.
  void SendOnInstalledSignal(const InstallResult& install_result);

  // Called on being connected to update_engine's |StatusUpdate| signal.
  void OnStatusUpdateAdvancedSignalConnected(const std::string& interface_name,
                                             const std::string& signal_name,
                                             bool success);

  std::unique_ptr<org::chromium::ImageLoaderInterfaceProxyInterface>
      image_loader_proxy_;
  std::unique_ptr<org::chromium::UpdateEngineInterfaceProxyInterface>
      update_engine_proxy_;
  std::unique_ptr<BootSlot> boot_slot_;

  base::FilePath manifest_dir_;
  base::FilePath content_dir_;

  // DLC modules being installed. An empty module infos signifies no install.
  DlcModuleList dlc_modules_being_installed_;

  std::string current_boot_slot_name_;

  // Map of currently existing DLC modules with it's corresponding DLC root.
  DlcRootMap installed_dlc_modules_;

  // List of allowed DLC modules based on manifest. Use a set in order to handle
  // repeats implicitly.
  std::set<DlcId> supported_dlc_modules_;

  base::WeakPtrFactory<DlcServiceDBusAdaptor> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(DlcServiceDBusAdaptor);
};

}  // namespace dlcservice

#endif  // DLCSERVICE_DLC_SERVICE_DBUS_ADAPTOR_H_
