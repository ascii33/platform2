// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_CELLULAR_CELLULAR_SERVICE_H_
#define SHILL_CELLULAR_CELLULAR_SERVICE_H_

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <base/macros.h>
#include <base/time/time.h>
#include <gtest/gtest_prod.h>  // for FRIEND_TEST

#include "shill/cellular/cellular.h"
#include "shill/cellular/subscription_state.h"
#include "shill/mockable.h"
#include "shill/refptr_types.h"
#include "shill/service.h"

namespace shill {

class Error;
class Manager;

class CellularService : public Service {
 public:
  enum ActivationType {
    kActivationTypeNonCellular,  // For future use
    kActivationTypeOMADM,        // For future use
    kActivationTypeOTA,
    kActivationTypeOTASP,
    kActivationTypeUnknown
  };

  CellularService(Manager* manager, const CellularRefPtr& device);
  ~CellularService() override;

  // Public Service overrides
  void AutoConnect() override;
  void CompleteCellularActivation(Error* error) override;
  std::string GetStorageIdentifier() const override;
  std::string GetLoadableStorageIdentifier(
      const StoreInterface& storage) const override;
  bool IsLoadableFrom(const StoreInterface& storage) const override;
  bool Load(StoreInterface* storage) override;
  bool Save(StoreInterface* storage) override;

  const CellularRefPtr& cellular() const { return cellular_; }

  void SetActivationType(ActivationType type);
  std::string GetActivationTypeString() const;

  mockable void SetActivationState(const std::string& state);
  mockable const std::string& activation_state() const {
    return activation_state_;
  }

  void SetOLP(const std::string& url,
              const std::string& method,
              const std::string& post_data);
  const Stringmap& olp() const { return olp_; }

  void SetUsageURL(const std::string& url);
  const std::string& usage_url() const { return usage_url_; }

  void SetServingOperator(const Stringmap& serving_operator);
  const Stringmap& serving_operator() const { return serving_operator_; }

  // Sets network technology to |technology| and broadcasts the property change.
  void SetNetworkTechnology(const std::string& technology);
  const std::string& network_technology() const { return network_technology_; }

  // Sets roaming state to |state| and broadcasts the property change.
  void SetRoamingState(const std::string& state);
  const std::string& roaming_state() const { return roaming_state_; }

  bool is_auto_connecting() const { return is_auto_connecting_; }

  const std::string& ppp_username() const { return ppp_username_; }
  const std::string& ppp_password() const { return ppp_password_; }

  Stringmap* GetUserSpecifiedApn();
  Stringmap* GetLastGoodApn();
  virtual void SetLastGoodApn(const Stringmap& apn_info);
  virtual void ClearLastGoodApn();

  void NotifySubscriptionStateChanged(SubscriptionState subscription_state);

 protected:
  // Protected Service overrides
  void OnConnect(Error* error) override;
  void OnDisconnect(Error* error, const char* reason) override;
  bool IsAutoConnectable(const char** reason) const override;
  uint64_t GetMaxAutoConnectCooldownTimeMilliseconds() const override;
  bool IsMeteredByServiceProperties() const override;
  RpcIdentifier GetDeviceRpcId(Error* error) const override;

 private:
  friend class CellularCapability3gppTest;
  friend class CellularServiceTest;

  template <typename key_type, typename value_type>
  friend class ContainsCellularPropertiesMatcherP2;

  FRIEND_TEST(CellularCapability3gppMainTest, UpdatePendingActivationState);
  FRIEND_TEST(CellularTest, Connect);
  FRIEND_TEST(CellularTest, FriendlyServiceName);
  FRIEND_TEST(CellularTest, GetLogin);  // ppp_username_, ppp_password_
  FRIEND_TEST(CellularServiceTest, SetApn);
  FRIEND_TEST(CellularServiceTest, ClearApn);
  FRIEND_TEST(CellularServiceTest, LastGoodApn);
  FRIEND_TEST(CellularServiceTest, LoadFromFirstOfMultipleMatchingProfiles);
  FRIEND_TEST(CellularServiceTest, LoadFromProfileMatchingImsi);
  FRIEND_TEST(CellularServiceTest, LoadFromProfileMatchingMeid);
  FRIEND_TEST(CellularServiceTest, LoadResetsPPPAuthFailure);
  FRIEND_TEST(CellularServiceTest, Save);
  FRIEND_TEST(CellularServiceTest, IsAutoConnectable);
  FRIEND_TEST(CellularServiceTest, CustomSetterNoopChange);

  static const char kAutoConnActivating[];
  static const char kAutoConnBadPPPCredentials[];
  static const char kAutoConnDeviceDisabled[];
  static const char kAutoConnOutOfCredits[];
  static const char kStorageIccid[];
  static const char kStorageImei[];
  static const char kStorageImsi[];
  static const char kStorageMeid[];
  static const char kStoragePPPUsername[];
  static const char kStoragePPPPassword[];

  void HelpRegisterDerivedString(
      const std::string& name,
      std::string (CellularService::*get)(Error* error),
      bool (CellularService::*set)(const std::string& value, Error* error));
  void HelpRegisterDerivedStringmap(
      const std::string& name,
      Stringmap (CellularService::*get)(Error* error),
      bool (CellularService::*set)(const Stringmap& value, Error* error));
  void HelpRegisterDerivedBool(const std::string& name,
                               bool (CellularService::*get)(Error* error),
                               bool (CellularService::*set)(const bool&,
                                                            Error*));

  std::set<std::string> GetStorageGroupsWithProperty(
      const StoreInterface& storage,
      const std::string& key,
      const std::string& value) const;

  std::string CalculateActivationType(Error* error);

  Stringmap GetApn(Error* error);
  bool SetApn(const Stringmap& value, Error* error);
  static void LoadApn(StoreInterface* storage,
                      const std::string& storage_group,
                      const std::string& keytag,
                      Stringmap* apn_info);
  static bool LoadApnField(StoreInterface* storage,
                           const std::string& storage_group,
                           const std::string& keytag,
                           const std::string& apntag,
                           Stringmap* apn_info);
  static void SaveApn(StoreInterface* storage,
                      const std::string& storage_group,
                      const Stringmap* apn_info,
                      const std::string& keytag);
  static void SaveApnField(StoreInterface* storage,
                           const std::string& storage_group,
                           const Stringmap* apn_info,
                           const std::string& keytag,
                           const std::string& apntag);
  bool IsOutOfCredits(Error* /*error*/);

  // Properties
  ActivationType activation_type_;
  std::string activation_state_;
  Stringmap serving_operator_;
  std::string network_technology_;
  std::string roaming_state_;
  Stringmap olp_;
  std::string usage_url_;
  Stringmap apn_info_;
  Stringmap last_good_apn_info_;
  std::string ppp_username_;
  std::string ppp_password_;

  std::string storage_identifier_;

  CellularRefPtr cellular_;

  // Flag indicating that a connect request is an auto-connect request.
  // Note: Since Connect() is asynchronous, this flag is only set during the
  // call to Connect().  It does not remain set while the async request is
  // in flight.
  bool is_auto_connecting_;
  // Flag indicating if the user has run out of data credits.
  bool out_of_credits_;

  DISALLOW_COPY_AND_ASSIGN(CellularService);
};

}  // namespace shill

#endif  // SHILL_CELLULAR_CELLULAR_SERVICE_H_
