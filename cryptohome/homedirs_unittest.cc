// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "homedirs.h"

#include <base/stl_util.h>
#include <base/stringprintf.h>
#include <chromeos/constants/cryptohome.h>
#include <chromeos/cryptohome.h>
#include <chromeos/secure_blob.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <policy/mock_device_policy.h>

#include "cryptolib.h"
#include "make_tests.h"
#include "mock_platform.h"
#include "mock_tpm.h"
#include "mock_user_oldest_activity_timestamp_cache.h"
#include "mock_vault_keyset.h"
#include "mock_vault_keyset_factory.h"
#include "signed_secret.pb.h"
#include "username_passkey.h"

namespace cryptohome {
using chromeos::SecureBlob;
using std::string;
using ::testing::DoAll;
using ::testing::EndsWith;
using ::testing::HasSubstr;
using ::testing::InSequence;
using ::testing::MatchesRegex;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::SaveArg;
using ::testing::SetArgPointee;
using ::testing::StartsWith;
using ::testing::StrEq;

using ::testing::_;

extern const char kKeyFile[];
extern const int kKeyFileMax;

ACTION_P2(SetOwner, owner_known, owner) {
  if (owner_known)
    *arg0 = owner;
  return owner_known;
}

ACTION_P(SetEphemeralUsersEnabled, ephemeral_users_enabled) {
  *arg0 = ephemeral_users_enabled;
  return true;
}

ACTION_P(SetCleanUpStrategy, clean_up_strategy) {
  if (!clean_up_strategy.empty()) {
    *arg0 = clean_up_strategy;
    return true;
  }
  return false;
}

namespace {
const char *kTestRoot = "alt_test_home_dir";

struct homedir {
  const char *name;
  base::Time::Exploded time;
};

const char *kOwner = "<<OWNER>>";
// Note, the order is important. These should be oldest to newest.
const struct homedir kHomedirs[] = {
  { "d5510a8dda6d743c46dadd979a61ae5603529742", { 2011, 1, 6, 1 } },
  { "8f995cdee8f0711fd32e1cf6246424002c483d47", { 2011, 2, 2, 1 } },
  { "973b9640e86f6073c6b6e2759ff3cf3084515e61", { 2011, 3, 2, 1 } },
  { kOwner, { 2011, 4, 5, 1 } }
};
const base::Time::Exploded jan1st2011_exploded = { 2011, 1, 6, 1 };
const base::Time time_jan1 = base::Time::FromUTCExploded(jan1st2011_exploded);
const base::Time::Exploded feb1st2011_exploded = { 2011, 2, 2, 1 };
const base::Time time_feb1 = base::Time::FromUTCExploded(feb1st2011_exploded);
const base::Time::Exploded mar1st2011_exploded = { 2011, 3, 2, 1 };
const base::Time time_mar1 = base::Time::FromUTCExploded(mar1st2011_exploded);
const base::Time::Exploded apr5th2011_exploded = { 2011, 4, 5, 1 };
const base::Time time_apr5 = base::Time::FromUTCExploded(apr5th2011_exploded);

NiceMock<MockFileEnumerator>* CreateMockFileEnumerator() {
  return new NiceMock<MockFileEnumerator>;
}
}  // namespace

class HomeDirsTest : public ::testing::Test {
 public:
  HomeDirsTest() { }
  virtual ~HomeDirsTest() { }

  void SetUp() {
    test_helper_.SetUpSystemSalt();
    // TODO(wad) Only generate the user data we need. This is time consuming.
    test_helper_.InitTestData(kTestRoot, kDefaultUsers, kDefaultUserCount);
    homedirs_.set_platform(&platform_);
    homedirs_.crypto()->set_platform(&platform_);
    homedirs_.set_shadow_root(kTestRoot);
    test_helper_.InjectSystemSalt(&platform_,
                                  StringPrintf("%s/salt", kTestRoot));
    set_policy(true, kOwner, false, "");

    // Mount() normally sets this.
    homedirs_.set_timestamp_cache(&timestamp_cache_);

    homedirs_.Init();
    FilePath fp = FilePath(kTestRoot);
    for (unsigned int i = 0; i < arraysize(kHomedirs); i++) {
      const struct homedir *hd = &kHomedirs[i];
      FilePath path;
      std::string owner;
      homedirs_.GetOwner(&owner);
      path = fp.Append(hd->name);
      if (!strcmp(hd->name, kOwner))
        path = fp.Append(owner);
      homedir_paths_.push_back(path.value());
      base::Time t = base::Time::FromUTCExploded(hd->time);
      homedir_times_.push_back(t);
    }
  }

  void TearDown() {
    test_helper_.TearDownSystemSalt();
  }

  void set_policy(bool owner_known,
                  const string& owner,
                  bool ephemeral_users_enabled,
                  const string& clean_up_strategy) {
    policy::MockDevicePolicy* device_policy = new policy::MockDevicePolicy();
    EXPECT_CALL(*device_policy, LoadPolicy())
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*device_policy, GetOwner(_))
        .WillRepeatedly(SetOwner(owner_known, owner));
    EXPECT_CALL(*device_policy, GetEphemeralUsersEnabled(_))
        .WillRepeatedly(SetEphemeralUsersEnabled(ephemeral_users_enabled));
    homedirs_.own_policy_provider(new policy::PolicyProvider(device_policy));
  }

 protected:
  HomeDirs homedirs_;
  NiceMock<MockPlatform> platform_;
  std::vector<std::string> homedir_paths_;
  MakeTests test_helper_;
  MockUserOldestActivityTimestampCache timestamp_cache_;
  std::vector<base::Time> homedir_times_;
  MockVaultKeysetFactory vault_keyset_factory_;

 private:
  DISALLOW_COPY_AND_ASSIGN(HomeDirsTest);
};

TEST_F(HomeDirsTest, RemoveNonOwnerCryptohomes) {
  // Ensure that RemoveNonOwnerCryptohomes does.
  EXPECT_CALL(platform_, EnumerateDirectoryEntries(kTestRoot, false, _))
    .WillOnce(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));
  FilePath user_prefix = chromeos::cryptohome::home::GetUserPathPrefix();
  FilePath root_prefix = chromeos::cryptohome::home::GetRootPathPrefix();
  EXPECT_CALL(platform_, EnumerateDirectoryEntries(user_prefix.value(), _, _))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, EnumerateDirectoryEntries(root_prefix.value(), _, _))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DirectoryExists(_))
    .WillRepeatedly(Return(true));
  EXPECT_CALL(platform_, IsDirectoryMountedWith(_, _))
    .WillRepeatedly(Return(false));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[0], true))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[1], true))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[2], true))
    .WillOnce(Return(true));

  homedirs_.RemoveNonOwnerCryptohomes();
}

class FreeDiskSpaceTest : public HomeDirsTest {
 public:
  FreeDiskSpaceTest() { }
  virtual ~FreeDiskSpaceTest() { }

  // The first half of HomeDirs::FreeDiskSpace does a purge of the Cache and
  // GCached dirs.  Unless these are being explicitly tested, we want these to
  // always succeed for every test. Set those expectations here.
  void ExpectCacheDirCleanupCalls() {
    EXPECT_CALL(platform_, EnumerateDirectoryEntries(kTestRoot, false, _))
      .WillRepeatedly(
          DoAll(SetArgPointee<2>(homedir_paths_),
                Return(true)));
    EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
      .Times(3).WillRepeatedly(Return(0))
      .RetiresOnSaturation();
    EXPECT_CALL(platform_, DirectoryExists(_))
      .WillRepeatedly(Return(true));
    // 4 users * (1 Cache dir + 1 GCache dir) = 8 dir iterations
    EXPECT_CALL(platform_, GetFileEnumerator(_, false, _))
      .Times(8).WillRepeatedly(InvokeWithoutArgs(CreateMockFileEnumerator));
  }
};

TEST_F(FreeDiskSpaceTest, InitializeTimeCacheWithNoTime) {
  // To get to the init logic, we need to fail three kEnoughFreeSpace checks.
  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(0))
    .WillOnce(Return(0))
    .WillOnce(Return(kEnoughFreeSpace - 1));

  // Expect cache clean ups.
  EXPECT_CALL(platform_, EnumerateDirectoryEntries(kTestRoot, false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));
  EXPECT_CALL(platform_, DirectoryExists(_))
    .WillRepeatedly(Return(true));
  EXPECT_CALL(platform_, IsDirectoryMountedWith(_, _))
    .WillRepeatedly(Return(false));

  // The master.* enumerators (wildcard matcher first)
  EXPECT_CALL(platform_, GetFileEnumerator(_, false, _))
    .Times(4).WillRepeatedly(InvokeWithoutArgs(CreateMockFileEnumerator));
  // Empty enumerators per-user per-cache dirs plus
  // enumerators for empty vaults.
  EXPECT_CALL(platform_, GetFileEnumerator(HasSubstr("user/Cache"), false, _))
    .Times(4).WillRepeatedly(InvokeWithoutArgs(CreateMockFileEnumerator));
  EXPECT_CALL(platform_, GetFileEnumerator(HasSubstr("user/GCache"), false, _))
    .Times(4).WillRepeatedly(InvokeWithoutArgs(CreateMockFileEnumerator));
  // Now cover the actual initialization piece
  EXPECT_CALL(timestamp_cache_, initialized())
    .WillOnce(Return(false));
  EXPECT_CALL(timestamp_cache_, Initialize())
    .Times(1);

  // It then walks the user vault to populate.
  MockVaultKeyset* vk[arraysize(kHomedirs)];
  EXPECT_CALL(vault_keyset_factory_, New(_, _))
    .WillOnce(Return(vk[0] = new MockVaultKeyset()))
    .WillOnce(Return(vk[1] = new MockVaultKeyset()))
    .WillOnce(Return(vk[2] = new MockVaultKeyset()))
    .WillOnce(Return(vk[3] = new MockVaultKeyset()));
  for (size_t i = 0; i < arraysize(vk); ++i) {
    EXPECT_CALL(*vk[i], Load(_))
      .WillRepeatedly(Return(false));
  }
  homedirs_.set_vault_keyset_factory(&vault_keyset_factory_);

  // Expect an addition for all users with no time.
  EXPECT_CALL(timestamp_cache_, AddExistingUserNotime(_))
    .Times(4);

  // Now skip the deletion steps by not having a legit owner.
  set_policy(false, "", false, "");

  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, InitializeTimeCacheWithOneTime) {
  // To get to the init logic, we need to fail three kEnoughFreeSpace checks.
  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(0))
    .WillOnce(Return(0))
    .WillOnce(Return(kEnoughFreeSpace - 1));

  // Expect cache clean ups.
  EXPECT_CALL(platform_, EnumerateDirectoryEntries(kTestRoot, false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));
  EXPECT_CALL(platform_, DirectoryExists(_))
    .WillRepeatedly(Return(true));
  // The master.* enumerators (wildcard matcher first)
  EXPECT_CALL(platform_, GetFileEnumerator(_, false, _))
    .Times(3).WillRepeatedly(InvokeWithoutArgs(CreateMockFileEnumerator));
  // Empty enumerators per-user per-cache dirs plus
  // enumerators for empty vaults.
  EXPECT_CALL(platform_, GetFileEnumerator(HasSubstr("user/Cache"), false, _))
    .Times(4).WillRepeatedly(InvokeWithoutArgs(CreateMockFileEnumerator));
  EXPECT_CALL(platform_, GetFileEnumerator(HasSubstr("user/GCache"), false, _))
    .Times(4).WillRepeatedly(InvokeWithoutArgs(CreateMockFileEnumerator));

  // Now cover the actual initialization piece
  EXPECT_CALL(timestamp_cache_, initialized())
    .WillOnce(Return(false));
  EXPECT_CALL(timestamp_cache_, Initialize())
    .Times(1);
  // Skip vault keyset loading to cause "Notime".
  EXPECT_CALL(platform_, FileExists(StartsWith(homedir_paths_[0])))
    .WillRepeatedly(Return(true));

  MockVaultKeyset* vk[arraysize(kHomedirs)];
  EXPECT_CALL(vault_keyset_factory_, New(_, _))
    .WillOnce(Return(vk[0] = new MockVaultKeyset()))
    .WillOnce(Return(vk[1] = new MockVaultKeyset()))
    .WillOnce(Return(vk[2] = new MockVaultKeyset()))
    .WillOnce(Return(vk[3] = new MockVaultKeyset()));
  // The first three will have no time.
  size_t i;
  for (i = 0; i < arraysize(vk) - 1; ++i) {
    EXPECT_CALL(*vk[i], Load(_))
      .WillRepeatedly(Return(false));
  }
  // Owner will have a master.0
  NiceMock<MockFileEnumerator> *master0;
  EXPECT_CALL(platform_, GetFileEnumerator(homedir_paths_[3], false, _))
    .WillOnce(Return(master0 = new NiceMock<MockFileEnumerator>));
  EXPECT_CALL(*master0, Next())
    .WillOnce(Return(StringPrintf("%s/%s0",
                       homedir_paths_[3].c_str(), kKeyFile)))
    .WillRepeatedly(Return(""));

  // The owner will have a time.
  EXPECT_CALL(*vk[i], Load(_))
    .WillOnce(Return(true));
  SerializedVaultKeyset serialized;
  serialized.set_last_activity_timestamp(homedir_times_[3].ToInternalValue());
  EXPECT_CALL(*vk[i], serialized())
    .Times(2)
    .WillRepeatedly(ReturnRef(serialized));
  homedirs_.set_vault_keyset_factory(&vault_keyset_factory_);

  // Expect an addition for all users with no time.
  EXPECT_CALL(timestamp_cache_, AddExistingUserNotime(_))
    .Times(3);
  // Adding the owner
  EXPECT_CALL(timestamp_cache_,
              AddExistingUser(FilePath(homedir_paths_[3]), homedir_times_[3]))
    .Times(1);

  // Now skip the deletion steps by not having a legit owner.
  set_policy(false, "", false, "");

  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, NoCacheCleanup) {
  // Pretend we have lots of free space
  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(kMinFreeSpaceInBytes + 1));
  EXPECT_FALSE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, OnlyCacheCleanup) {
  // Only clean up the Cache data. Not GCache, etc.
  EXPECT_CALL(platform_, EnumerateDirectoryEntries(kTestRoot, false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));

  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(0))
    .WillOnce(Return(kEnoughFreeSpace + 1));
  EXPECT_CALL(platform_, DirectoryExists(_))
    .WillRepeatedly(Return(true));
  // Empty enumerators per-user per-cache dirs
  NiceMock<MockFileEnumerator>* fe[arraysize(kHomedirs)];
  EXPECT_CALL(platform_, GetFileEnumerator(_, false, _))
    .WillOnce(Return(fe[0] = new NiceMock<MockFileEnumerator>))
    .WillOnce(Return(fe[1] = new NiceMock<MockFileEnumerator>))
    .WillOnce(Return(fe[2] = new NiceMock<MockFileEnumerator>))
    .WillOnce(Return(fe[3] = new NiceMock<MockFileEnumerator>));
  // Exercise the delete file path.
  for (size_t f = 0; f < arraysize(fe); ++f) {
    EXPECT_CALL(*fe[f], Next())
      .WillOnce(Return(StringPrintf("%s/%s", homedir_paths_[f].c_str(),
                                             "Cache/foo")))
      .WillRepeatedly(Return(""));
  }
  EXPECT_CALL(platform_, DeleteFile(EndsWith("/Cache/foo"), true))
    .Times(4)
    .WillRepeatedly(Return(true));

  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, CacheAndGCacheCleanup) {
  EXPECT_CALL(platform_, EnumerateDirectoryEntries(kTestRoot, false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));
  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(0))
    .WillOnce(Return(0))
    .WillOnce(Return(kEnoughFreeSpace + 1));
  EXPECT_CALL(platform_, DirectoryExists(_))
    .WillRepeatedly(Return(true));
  // Empty enumerators per-user per-cache dirs
  NiceMock<MockFileEnumerator>* fe[arraysize(kHomedirs)];
  EXPECT_CALL(platform_, GetFileEnumerator(EndsWith("/Cache"), false, _))
    .WillOnce(Return(fe[0] = new NiceMock<MockFileEnumerator>))
    .WillOnce(Return(fe[1] = new NiceMock<MockFileEnumerator>))
    .WillOnce(Return(fe[2] = new NiceMock<MockFileEnumerator>))
    .WillOnce(Return(fe[3] = new NiceMock<MockFileEnumerator>));
  // Exercise the delete file path.
  for (size_t f = 0; f < arraysize(fe); ++f) {
    EXPECT_CALL(*fe[f], Next())
      .WillOnce(Return(StringPrintf("%s/%s", homedir_paths_[f].c_str(),
                                             "Cache/foo")))
      .WillRepeatedly(Return(""));
  }
  EXPECT_CALL(platform_, DeleteFile(EndsWith("/Cache/foo"), true))
    .Times(4)
    .WillRepeatedly(Return(true));
  // Repeat for GCache
  EXPECT_CALL(platform_, GetFileEnumerator(EndsWith("GCache/v1/tmp"), false, _))
    .WillOnce(Return(fe[0] = new NiceMock<MockFileEnumerator>))
    .WillOnce(Return(fe[1] = new NiceMock<MockFileEnumerator>))
    .WillOnce(Return(fe[2] = new NiceMock<MockFileEnumerator>))
    .WillOnce(Return(fe[3] = new NiceMock<MockFileEnumerator>));
  // Exercise the delete file path.
  for (size_t f = 0; f < arraysize(fe); ++f) {
    EXPECT_CALL(*fe[f], Next())
      .WillOnce(Return(StringPrintf("%s/%s", homedir_paths_[f].c_str(),
                                             "GCache/v1/tmp/foo")))
      .WillRepeatedly(Return(""));
  }
  EXPECT_CALL(platform_, DeleteFile(EndsWith("/GCache/v1/tmp/foo"), true))
    .Times(4)
    .WillRepeatedly(Return(true));

  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, CleanUpOneUser) {
  // Ensure that the oldest user directory deleted, but not any
  // others, if the first deletion frees enough space.
  EXPECT_CALL(timestamp_cache_, initialized())
    .WillRepeatedly(Return(true));

  EXPECT_CALL(timestamp_cache_, empty())
    .WillOnce(Return(false));

  EXPECT_CALL(timestamp_cache_, RemoveOldestUser())
    .WillOnce(Return(FilePath(homedir_paths_[0])));

  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(kEnoughFreeSpace + 1));

  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[0], true))
    .WillOnce(Return(true));

  ExpectCacheDirCleanupCalls();
  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, CleanUpMultipleUsers) {
  // Ensure that the two oldest user directories are deleted, but not any
  // others, if the second deletion frees enough space.
  EXPECT_CALL(timestamp_cache_, initialized())
    .WillRepeatedly(Return(true));

  EXPECT_CALL(timestamp_cache_, empty())
    .WillOnce(Return(false))
    .WillOnce(Return(false));

  EXPECT_CALL(timestamp_cache_, RemoveOldestUser())
    .WillOnce(Return(FilePath(homedir_paths_[0])))
    .WillOnce(Return(FilePath(homedir_paths_[1])));

  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(kEnoughFreeSpace - 1))
    .WillOnce(Return(kEnoughFreeSpace + 1));

  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[0], true))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[1], true))
    .WillOnce(Return(true));

  ExpectCacheDirCleanupCalls();
  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, EnterpriseCleanUpAllUsersButLast) {
   set_policy(true, "", false, "");
   homedirs_.set_enterprise_owned(true);

  EXPECT_CALL(timestamp_cache_, initialized())
    .WillRepeatedly(Return(true));

  EXPECT_CALL(timestamp_cache_, empty())
    .WillOnce(Return(false))  // loop
    .WillOnce(Return(false))  // enterprise && empty check
    .WillOnce(Return(false))  // loop
    .WillOnce(Return(true));  // enterprise && empty check

  EXPECT_CALL(timestamp_cache_, RemoveOldestUser())
    .WillOnce(Return(FilePath(homedir_paths_[0])))
    .WillOnce(Return(FilePath(homedir_paths_[1])));

  // Confirm deletion isn't time bound; timestamps never checked.
  EXPECT_CALL(timestamp_cache_, oldest_known_timestamp()).Times(0);

  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillRepeatedly(Return(0));

  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[0], true))
    .WillOnce(Return(true));

  // Most-recent user isn't deleted.
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[1], true)).Times(0);

  ExpectCacheDirCleanupCalls();
  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, CleanUpMultipleNonadjacentUsers) {

  // Ensure that the two oldest user directories are deleted, but not any
  // others.  The owner is inserted in the middle.
  EXPECT_CALL(timestamp_cache_, initialized())
    .WillRepeatedly(Return(true));

  EXPECT_CALL(timestamp_cache_, empty())
    .WillOnce(Return(false))
    .WillOnce(Return(false))
    .WillOnce(Return(false));

  EXPECT_CALL(timestamp_cache_, RemoveOldestUser())
    .WillOnce(Return(FilePath(homedir_paths_[0])))
    .WillOnce(Return(FilePath(homedir_paths_[3])))
    .WillOnce(Return(FilePath(homedir_paths_[1])));

  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(0))
    // Loop continued before we check disk space for owner.
    .WillOnce(Return(kEnoughFreeSpace + 1));

  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[0], true))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[1], true))
    .WillOnce(Return(true));
  // Ensure the owner isn't deleted!
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[3], true))
    .Times(0);

  ExpectCacheDirCleanupCalls();
  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, NoOwnerNoEnterpriseNoCleanup) {
  // Ensure that no users are deleted with no owner/enterprise-owner.
  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillRepeatedly(Return(0));

  // Skip re-init
  EXPECT_CALL(timestamp_cache_, initialized())
    .WillOnce(Return(true));

  // No user deletions!
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[0], true))
    .Times(0);
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[1], true))
    .Times(0);
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[2], true))
    .Times(0);
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[3], true))
    .Times(0);

  // Now skip the deletion steps by not having a legit owner.
  set_policy(false, "", false, "");

  ExpectCacheDirCleanupCalls();
  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, ConsumerEphemeralUsers) {
  // When ephemeral users are enabled, no cryptohomes are kept except the owner.
  set_policy(true, kOwner, true, "");

  EXPECT_CALL(platform_, EnumerateDirectoryEntries(kTestRoot, false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));
  EXPECT_CALL(platform_,
      EnumerateDirectoryEntries(
          chromeos::cryptohome::home::GetUserPathPrefix().value(),
          false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));
  EXPECT_CALL(platform_,
      EnumerateDirectoryEntries(
          chromeos::cryptohome::home::GetRootPathPrefix().value(),
          false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));

  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(kMinFreeSpaceInBytes - 1));

  EXPECT_CALL(platform_, DirectoryExists(_))
    .WillRepeatedly(Return(true));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[0], true))
    .WillOnce(Return(true))  // vault
    .WillOnce(Return(true))  // user
    .WillOnce(Return(true));  // root
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[1], true))
    .WillOnce(Return(true))
    .WillOnce(Return(true))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[2], true))
    .WillOnce(Return(true))
    .WillOnce(Return(true))
    .WillOnce(Return(true));
  // Ensure the owner isn't deleted!
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[3], true))
    .Times(0);

  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, EnterpriseEphemeralUsers) {
  // When ephemeral users are enabled, no cryptohomes are kept except the owner.
  set_policy(true, "", true, "");
  homedirs_.set_enterprise_owned(true);

  EXPECT_CALL(platform_, EnumerateDirectoryEntries(kTestRoot, false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));
  EXPECT_CALL(platform_,
      EnumerateDirectoryEntries(
          chromeos::cryptohome::home::GetUserPathPrefix().value(),
          false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));
  EXPECT_CALL(platform_,
      EnumerateDirectoryEntries(
          chromeos::cryptohome::home::GetRootPathPrefix().value(),
          false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));

  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillOnce(Return(kMinFreeSpaceInBytes - 1));

  EXPECT_CALL(platform_, DirectoryExists(_))
    .WillRepeatedly(Return(true));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[0], true))
    .WillOnce(Return(true))  // vault
    .WillOnce(Return(true))  // user
    .WillOnce(Return(true));  // root
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[1], true))
    .WillOnce(Return(true))
    .WillOnce(Return(true))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[2], true))
    .WillOnce(Return(true))
    .WillOnce(Return(true))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(homedir_paths_[3], true))
    .WillOnce(Return(true))
    .WillOnce(Return(true))
    .WillOnce(Return(true));

  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(FreeDiskSpaceTest, DontCleanUpMountedUser) {
  // Ensure that a user isn't deleted if it appears to be mounted.
  EXPECT_CALL(timestamp_cache_, initialized())
    .WillRepeatedly(Return(true));
  EXPECT_CALL(timestamp_cache_, empty())
    .WillOnce(Return(false))
    .WillOnce(Return(true));

  // This will only be called once (see time below).
  EXPECT_CALL(timestamp_cache_, RemoveOldestUser())
    .WillOnce(Return(FilePath(homedir_paths_[0])));

  EXPECT_CALL(platform_, EnumerateDirectoryEntries(kTestRoot, false, _))
    .WillRepeatedly(
        DoAll(SetArgPointee<2>(homedir_paths_),
              Return(true)));
  EXPECT_CALL(platform_, AmountOfFreeDiskSpace(kTestRoot))
    .WillRepeatedly(Return(0));
  EXPECT_CALL(platform_, DirectoryExists(_))
    .WillRepeatedly(Return(true));

  // Ensure the mounted user never has (G)Cache traversed!
  EXPECT_CALL(platform_, GetFileEnumerator(
      StartsWith(homedir_paths_[0]), false, _))
    .Times(0);

  // 3 users * (1 Cache dir + 1 GCache dir) = 6 dir iterations
  EXPECT_CALL(platform_, GetFileEnumerator(_, false, _))
    .Times(6).WillRepeatedly(InvokeWithoutArgs(CreateMockFileEnumerator));

  EXPECT_CALL(platform_,
      IsDirectoryMountedWith(StartsWith(homedir_paths_[0]), _))
    .Times(3)  // Cache, GCache, user removal
    .WillRepeatedly(Return(true));
  for (size_t i = 1; i < arraysize(kHomedirs); ++i) {
    EXPECT_CALL(platform_,
        IsDirectoryMountedWith(StartsWith(homedir_paths_[i]), _))
      .Times(2) // Cache, GCache
      .WillRepeatedly(Return(false));
  }

  EXPECT_TRUE(homedirs_.FreeDiskSpace());
}

TEST_F(HomeDirsTest, GoodDecryptTest) {
  // create a HomeDirs instance that points to a good shadow root, test that it
  // properly authenticates against the first key.
  SecureBlob system_salt;
  NiceMock<MockTpm> tpm;
  homedirs_.crypto()->set_tpm(&tpm);
  homedirs_.crypto()->set_use_tpm(false);
  ASSERT_TRUE(homedirs_.GetSystemSalt(&system_salt));
  set_policy(false, "", false, "");

  test_helper_.users[1].InjectKeyset(&platform_);
  cryptohome::SecureBlob passkey;
  cryptohome::Crypto::PasswordToPasskey(test_helper_.users[1].password,
                                        system_salt, &passkey);
  UsernamePasskey up(test_helper_.users[1].username, passkey);

  ASSERT_TRUE(homedirs_.AreCredentialsValid(up));
}

TEST_F(HomeDirsTest, BadDecryptTest) {
  // create a HomeDirs instance that points to a good shadow root, test that it
  // properly denies access with a bad passkey
  SecureBlob system_salt;
  NiceMock<MockTpm> tpm;
  homedirs_.crypto()->set_tpm(&tpm);
  homedirs_.crypto()->set_use_tpm(false);
  set_policy(false, "", false, "");

  test_helper_.users[4].InjectKeyset(&platform_);

  cryptohome::SecureBlob passkey;
  cryptohome::Crypto::PasswordToPasskey("bogus", system_salt, &passkey);
  UsernamePasskey up(test_helper_.users[4].username, passkey);

  ASSERT_FALSE(homedirs_.AreCredentialsValid(up));
}

#define MAX_VKS 5
class KeysetManagementTest : public HomeDirsTest {
 public:
  KeysetManagementTest() { }
  virtual ~KeysetManagementTest() { }

  void SetUp() {
    HomeDirsTest::SetUp();
    last_vk_ = -1;
    active_vk_ = NULL;
    memset(active_vks_, 0, sizeof(active_vks_));
  }

  void TearDown() {
    HomeDirsTest::TearDown();
    last_vk_++;
    for ( ; last_vk_ < MAX_VKS; ++last_vk_) {
      if (active_vks_[last_vk_])
        delete active_vks_[last_vk_];
      active_vks_[last_vk_] = NULL;
    }
    last_vk_ = -1;
    active_vk_ = NULL;
  }

  virtual bool VkDecrypt0(const chromeos::SecureBlob& key) {
    return memcmp(key.const_data(), keys_[0].const_data(),
                  key.size()) == 0;
  }

  virtual const SerializedVaultKeyset& FakeSerialized() const {
    return serialized_;
  }

  virtual SerializedVaultKeyset* FakeMutableSerialized() {
    return &serialized_;
  }

  virtual MockFileEnumerator* NewKeysetFileEnumerator() {
    MockFileEnumerator* files = new MockFileEnumerator();
    {
      InSequence s;
      // Single key.
      EXPECT_CALL(*files, Next())
        .WillOnce(Return(keyset_paths_[0]));
      EXPECT_CALL(*files, Next())
        .WillOnce(Return(""));
    }
    return files;
  }

  virtual MockVaultKeyset* NewActiveVaultKeyset() {
    last_vk_++;
    CHECK(last_vk_ < MAX_VKS);
    active_vk_ = active_vks_[last_vk_];
    EXPECT_CALL(*active_vk_, Load(keyset_paths_[0]))
      .WillOnce(Return(true));
    EXPECT_CALL(*active_vk_, Decrypt(_))
      .WillRepeatedly(Invoke(this, &KeysetManagementTest::VkDecrypt0));

    EXPECT_CALL(*active_vk_, serialized())
      .WillRepeatedly(
          Invoke(this, &KeysetManagementTest::FakeSerialized));
    EXPECT_CALL(*active_vk_, mutable_serialized())
      .WillRepeatedly(
          Invoke(this, &KeysetManagementTest::FakeMutableSerialized));
    return active_vk_;
  }

  virtual void KeysetSetUp() {
    serialized_.Clear();
    NiceMock<MockTpm> tpm;
    homedirs_.crypto()->set_tpm(&tpm);
    homedirs_.crypto()->set_use_tpm(false);
    ASSERT_TRUE(homedirs_.GetSystemSalt(&system_salt_));
    set_policy(false, "", false, "");

    // Setup the base keyset files for users[1]
    keyset_paths_.push_back(test_helper_.users[1].keyset_path);
    keys_.push_back(test_helper_.users[1].passkey);

    EXPECT_CALL(platform_, GetFileEnumerator(
        test_helper_.users[1].base_path, false, _))
      .WillRepeatedly(
        InvokeWithoutArgs(this,
                          &KeysetManagementTest::NewKeysetFileEnumerator));

    homedirs_.set_vault_keyset_factory(&vault_keyset_factory_);
    // Pre-allocate VKs so that each call can advance
    // but expectations can be set.
    for (int i = 0; i < MAX_VKS; ++i) {
      active_vks_[i] = new MockVaultKeyset();
    }
    active_vk_ = active_vks_[0];

    EXPECT_CALL(vault_keyset_factory_, New(_, _))
      .WillRepeatedly(
          InvokeWithoutArgs(this, &KeysetManagementTest::NewActiveVaultKeyset));
    cryptohome::SecureBlob passkey;
    cryptohome::Crypto::PasswordToPasskey(test_helper_.users[1].password,
                                          system_salt_, &passkey);
    up_.reset(new UsernamePasskey(test_helper_.users[1].username, passkey));
  }

  int last_vk_;
  MockVaultKeyset* active_vk_;
  MockVaultKeyset* active_vks_[MAX_VKS];
  std::vector<std::string> keyset_paths_;
  std::vector<chromeos::SecureBlob> keys_;
  scoped_ptr<UsernamePasskey> up_;
  SecureBlob system_salt_;
  SerializedVaultKeyset serialized_;
};

TEST_F(KeysetManagementTest, AddKeysetSuccess) {
  KeysetSetUp();

  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);
  int index = -1;
  // The injected keyset in the fixture handles the up_ validation.
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.0"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(NULL)));
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.1"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));
  EXPECT_CALL(*active_vk_, Encrypt(newkey))
    .WillOnce(Return(true));
  EXPECT_CALL(*active_vk_, Save(EndsWith("master.1")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(_, _))
    .Times(0);

  EXPECT_EQ(CRYPTOHOME_ERROR_NOT_SET,
            homedirs_.AddKeyset(*up_, newkey, NULL, false, &index));
  EXPECT_EQ(index, 1);
}

TEST_F(KeysetManagementTest, AddKeysetClobber) {
  KeysetSetUp();

  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);
  serialized_.mutable_key_data()->set_label("current label");
  KeyData key_data;
  key_data.set_label("current label");
  std::string vk_path = "/some/path/master.0";
  // Show that 0 is taken.
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.0"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(NULL)));
  // Let it claim 1 until it searches the labels.
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.1"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));
  EXPECT_CALL(*active_vk_, Encrypt(newkey))
    .WillOnce(Return(true));
  EXPECT_CALL(*active_vks_[1], set_legacy_index(_));
  EXPECT_CALL(*active_vks_[1], legacy_index())
    .WillOnce(Return(0));
  EXPECT_CALL(*active_vks_[1], source_file())
    .WillOnce(ReturnRef(vk_path));
  EXPECT_CALL(*active_vk_, Save(vk_path))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(EndsWith("master.1"), _))
    .Times(1);

  int index = -1;
  EXPECT_EQ(CRYPTOHOME_ERROR_NOT_SET,
            homedirs_.AddKeyset(*up_, newkey, &key_data, true, &index));
  EXPECT_EQ(index, 0);
}


TEST_F(KeysetManagementTest, AddKeysetNoClobber) {
  KeysetSetUp();

  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);
  int index = -1;
  serialized_.mutable_key_data()->set_label("current label");
  KeyData key_data;
  key_data.set_label("current label");
  // The injected keyset in the fixture handles the up_ validation.
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.0"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(NULL)));
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.1"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));

  EXPECT_EQ(CRYPTOHOME_ERROR_KEY_LABEL_EXISTS,
            homedirs_.AddKeyset(*up_, newkey, &key_data, false, &index));
  EXPECT_EQ(index, -1);
}


TEST_F(KeysetManagementTest, UpdateKeysetSuccess) {
  KeysetSetUp();

  // No need to do PasswordToPasskey as that is the
  // external callers job.
  cryptohome::SecureBlob new_secret("why not", strlen("why not"));
  Key new_key;
  new_key.set_secret("why not");
  new_key.mutable_data()->set_label("new label");
  // The injected keyset in the fixture handles the up_ validation.
  serialized_.mutable_key_data()->set_label("current label");
  std::string vk_path = "some/path/master.0";
  EXPECT_CALL(*active_vk_, source_file())
    .WillOnce(ReturnRef(vk_path));
  EXPECT_CALL(*active_vk_, Encrypt(new_secret))
    .WillOnce(Return(true));
  EXPECT_CALL(*active_vk_, Save(vk_path))
    .WillOnce(Return(true));

  EXPECT_EQ(CRYPTOHOME_ERROR_NOT_SET,
            homedirs_.UpdateKeyset(*up_,
                                   const_cast<const Key *>(&new_key),
                                   ""));
  EXPECT_EQ(serialized_.key_data().label(), new_key.data().label());
}

TEST_F(KeysetManagementTest, UpdateKeysetAuthorizedNoSignature) {
  KeysetSetUp();

  // No need to do PasswordToPasskey as that is the
  // external callers job.
  Key new_key;
  new_key.set_secret("why not");
  new_key.mutable_data()->set_label("new label");
  new_key.mutable_data()->set_revision(1);
  // The injected keyset in the fixture handles the up_ validation.
  KeyData* key_data = serialized_.mutable_key_data();
  key_data->set_label("current label");
  // Allow the default override on the revision.
  key_data->mutable_privileges()->set_update(false);
  key_data->mutable_privileges()->set_authorized_update(true);
  KeyAuthorizationData* auth_data = key_data->add_authorization_data();
  auth_data->set_type(KeyAuthorizationData::KEY_AUTHORIZATION_TYPE_HMACSHA256);
  KeyAuthorizationSecret* auth_secret = auth_data->add_secrets();
  auth_secret->mutable_usage()->set_sign(true);
  const std::string kSomeHMACKey("abc123");
  auth_secret->set_symmetric_key(kSomeHMACKey);

  EXPECT_EQ(CRYPTOHOME_ERROR_UPDATE_SIGNATURE_INVALID,
            homedirs_.UpdateKeyset(*up_,
                                   const_cast<const Key *>(&new_key),
                                   ""));
  EXPECT_NE(serialized_.key_data().label(), new_key.data().label());
}

TEST_F(KeysetManagementTest, UpdateKeysetAuthorizedSuccess) {
  KeysetSetUp();

  // No need to do PasswordToPasskey as that is the
  // external callers job.
  cryptohome::SecureBlob new_pass("why not", strlen("why not"));
  Key new_key;
  new_key.set_secret("why not");
  new_key.mutable_data()->set_label("new label");
  // Allow updating over an undefined revision.
  new_key.mutable_data()->set_revision(0);
  // The injected keyset in the fixture handles the up_ validation.
  KeyData* key_data = serialized_.mutable_key_data();
  key_data->set_label("current label");
  key_data->mutable_privileges()->set_update(false);
  key_data->mutable_privileges()->set_authorized_update(true);
  KeyAuthorizationData* auth_data = key_data->add_authorization_data();
  auth_data->set_type(KeyAuthorizationData::KEY_AUTHORIZATION_TYPE_HMACSHA256);
  KeyAuthorizationSecret* auth_secret = auth_data->add_secrets();
  auth_secret->mutable_usage()->set_sign(true);
  const std::string kSomeHMACKey("abc123");
  auth_secret->set_symmetric_key(kSomeHMACKey);

  std::string vk_path = "some/path/master.0";
  EXPECT_CALL(*active_vk_, source_file())
    .WillOnce(ReturnRef(vk_path));
  EXPECT_CALL(*active_vk_, Encrypt(new_pass))
    .WillOnce(Return(true));
  EXPECT_CALL(*active_vk_, Save(vk_path))
    .WillOnce(Return(true));

  std::string changes_str;
  ac::chrome::managedaccounts::account::Secret new_secret;
  new_secret.set_revision(new_key.data().revision());
  new_secret.set_secret(new_key.secret());
  ASSERT_TRUE(new_secret.SerializeToString(&changes_str));

  chromeos::SecureBlob hmac_key(auth_secret->symmetric_key());
  chromeos::SecureBlob hmac_data(changes_str.c_str(), changes_str.size());
  SecureBlob hmac = CryptoLib::HmacSha256(hmac_key, hmac_data);
  std::string hmac_str(reinterpret_cast<const char*>(vector_as_array(&hmac)),
                                                     hmac.size());
  EXPECT_EQ(CRYPTOHOME_ERROR_NOT_SET,
            homedirs_.UpdateKeyset(*up_,
                                   const_cast<const Key *>(&new_key),
                                   hmac_str));
  EXPECT_EQ(serialized_.key_data().revision(), new_key.data().revision());
}

TEST_F(KeysetManagementTest, UpdateKeysetAuthorizedNoEqualReplay) {
  KeysetSetUp();

  // No need to do PasswordToPasskey as that is the
  // external callers job.
  Key new_key;
  new_key.set_secret("why not");
  new_key.mutable_data()->set_label("new label");
  new_key.mutable_data()->set_revision(100);
  // The injected keyset in the fixture handles the up_ validation.
  KeyData* key_data = serialized_.mutable_key_data();
  key_data->set_revision(100);
  key_data->set_label("current label");
  key_data->mutable_privileges()->set_update(false);
  key_data->mutable_privileges()->set_authorized_update(true);
  KeyAuthorizationData* auth_data = key_data->add_authorization_data();
  auth_data->set_type(KeyAuthorizationData::KEY_AUTHORIZATION_TYPE_HMACSHA256);
  KeyAuthorizationSecret* auth_secret = auth_data->add_secrets();
  auth_secret->mutable_usage()->set_sign(true);
  const std::string kSomeHMACKey("abc123");
  auth_secret->set_symmetric_key(kSomeHMACKey);

  std::string changes_str;
  ac::chrome::managedaccounts::account::Secret new_secret;
  new_secret.set_revision(new_key.data().revision());
  new_secret.set_secret(new_key.secret());
  ASSERT_TRUE(new_secret.SerializeToString(&changes_str));
  chromeos::SecureBlob hmac_key(auth_secret->symmetric_key());
  chromeos::SecureBlob hmac_data(changes_str.c_str(), changes_str.size());
  SecureBlob hmac = CryptoLib::HmacSha256(hmac_key, hmac_data);
  std::string hmac_str(reinterpret_cast<const char*>(vector_as_array(&hmac)),
                                                     hmac.size());
  EXPECT_EQ(CRYPTOHOME_ERROR_UPDATE_SIGNATURE_INVALID,
            homedirs_.UpdateKeyset(*up_,
                                   const_cast<const Key *>(&new_key),
                                   hmac_str));
  EXPECT_NE(serialized_.key_data().label(), new_key.data().label());
}


TEST_F(KeysetManagementTest, UpdateKeysetAuthorizedNoLessReplay) {
  KeysetSetUp();

  // No need to do PasswordToPasskey as that is the
  // external callers job.
  Key new_key;
  new_key.set_secret("why not");
  new_key.mutable_data()->set_label("new label");
  new_key.mutable_data()->set_revision(0);
  // The injected keyset in the fixture handles the up_ validation.
  KeyData* key_data = serialized_.mutable_key_data();
  key_data->set_revision(1);
  key_data->set_label("current label");
  key_data->mutable_privileges()->set_update(false);
  key_data->mutable_privileges()->set_authorized_update(true);
  KeyAuthorizationData* auth_data = key_data->add_authorization_data();
  auth_data->set_type(KeyAuthorizationData::KEY_AUTHORIZATION_TYPE_HMACSHA256);
  KeyAuthorizationSecret* auth_secret = auth_data->add_secrets();
  auth_secret->mutable_usage()->set_sign(true);
  const std::string kSomeHMACKey("abc123");
  auth_secret->set_symmetric_key(kSomeHMACKey);

  std::string changes_str;
  ac::chrome::managedaccounts::account::Secret new_secret;
  new_secret.set_revision(new_key.data().revision());
  new_secret.set_secret(new_key.secret());
  ASSERT_TRUE(new_secret.SerializeToString(&changes_str));

  chromeos::SecureBlob hmac_key(auth_secret->symmetric_key());
  chromeos::SecureBlob hmac_data(changes_str.c_str(), changes_str.size());
  SecureBlob hmac = CryptoLib::HmacSha256(hmac_key, hmac_data);
  std::string hmac_str(reinterpret_cast<const char*>(vector_as_array(&hmac)),
                                                     hmac.size());
  EXPECT_EQ(CRYPTOHOME_ERROR_UPDATE_SIGNATURE_INVALID,
            homedirs_.UpdateKeyset(*up_,
                                   const_cast<const Key *>(&new_key),
                                   hmac_str));
  EXPECT_NE(serialized_.key_data().label(), new_key.data().label());
}

TEST_F(KeysetManagementTest, UpdateKeysetAuthorizedBadSignature) {
  KeysetSetUp();

  // No need to do PasswordToPasskey as that is the
  // external callers job.
  Key new_key;
  new_key.set_secret("why not");
  new_key.mutable_data()->set_label("new label");
  new_key.mutable_data()->set_revision(0);
  // The injected keyset in the fixture handles the up_ validation.
  KeyData* key_data = serialized_.mutable_key_data();
  key_data->set_label("current label");
  key_data->mutable_privileges()->set_update(false);
  key_data->mutable_privileges()->set_authorized_update(true);
  KeyAuthorizationData* auth_data = key_data->add_authorization_data();
  auth_data->set_type(KeyAuthorizationData::KEY_AUTHORIZATION_TYPE_HMACSHA256);
  KeyAuthorizationSecret* auth_secret = auth_data->add_secrets();
  auth_secret->mutable_usage()->set_sign(true);
  const std::string kSomeHMACKey("abc123");
  auth_secret->set_symmetric_key(kSomeHMACKey);

  std::string changes_str;
  ac::chrome::managedaccounts::account::Secret bad_secret;
  bad_secret.set_revision(new_key.data().revision());
  bad_secret.set_secret("something else");
  ASSERT_TRUE(bad_secret.SerializeToString(&changes_str));

  chromeos::SecureBlob hmac_key(auth_secret->symmetric_key());
  chromeos::SecureBlob hmac_data(changes_str.c_str(), changes_str.size());
  SecureBlob hmac = CryptoLib::HmacSha256(hmac_key, hmac_data);
  std::string hmac_str(reinterpret_cast<const char*>(vector_as_array(&hmac)),
                                                     hmac.size());
  EXPECT_EQ(CRYPTOHOME_ERROR_UPDATE_SIGNATURE_INVALID,
            homedirs_.UpdateKeyset(*up_,
                                   const_cast<const Key *>(&new_key),
                                   hmac_str));
  EXPECT_NE(serialized_.key_data().label(), new_key.data().label());
}

TEST_F(KeysetManagementTest, UpdateKeysetBadSecret) {
  KeysetSetUp();

  // No need to do PasswordToPasskey as that is the
  // external callers job.
  cryptohome::SecureBlob new_secret("why not", strlen("why not"));
  Key new_key;
  new_key.set_secret("why not");
  new_key.mutable_data()->set_label("new label");
  // The injected keyset in the fixture handles the up_ validation.
  serialized_.mutable_key_data()->set_label("current label");

  cryptohome::SecureBlob bad_pass("not it", strlen("not it"));
  up_.reset(new UsernamePasskey(test_helper_.users[1].username, bad_pass));
  EXPECT_EQ(CRYPTOHOME_ERROR_AUTHORIZATION_KEY_FAILED,
            homedirs_.UpdateKeyset(*up_,
                                   const_cast<const Key *>(&new_key),
                                   ""));
  EXPECT_NE(serialized_.key_data().label(), new_key.data().label());
}

TEST_F(KeysetManagementTest, UpdateKeysetNotFoundWithLabel) {
  KeysetSetUp();

  KeyData some_label;
  some_label.set_label("key that doesn't exist");
  up_->set_key_data(some_label);
  const Key new_key;
  EXPECT_EQ(CRYPTOHOME_ERROR_AUTHORIZATION_KEY_NOT_FOUND,
            homedirs_.UpdateKeyset(*up_,
                                   &new_key,
                                   ""));
}

TEST_F(KeysetManagementTest, RemoveKeysetSuccess) {
  KeysetSetUp();

  Key remove_key;
  remove_key.mutable_data()->set_label("remove me");
  // Expect the 0 slot since it'll match all the fake keys.
  EXPECT_CALL(*active_vks_[1], set_legacy_index(0));
  // Return a different slot to make sure the code is using the right object.
  EXPECT_CALL(*active_vks_[1], legacy_index())
    .WillOnce(Return(1));
  EXPECT_CALL(platform_, FileExists(EndsWith("master.1")))
    .WillOnce(Return(true));

  serialized_.mutable_key_data()->mutable_privileges()->set_remove(true);
  serialized_.mutable_key_data()->set_label("remove me");
  EXPECT_EQ(CRYPTOHOME_ERROR_NOT_SET,
            homedirs_.RemoveKeyset(*up_, remove_key.data()));
}

TEST_F(KeysetManagementTest, RemoveKeysetNotFound) {
  KeysetSetUp();

  Key remove_key;
  remove_key.mutable_data()->set_label("remove me please");

  serialized_.mutable_key_data()->mutable_privileges()->set_remove(true);
  serialized_.mutable_key_data()->set_label("the only key in town");
  EXPECT_EQ(CRYPTOHOME_ERROR_KEY_NOT_FOUND,
            homedirs_.RemoveKeyset(*up_, remove_key.data()));
}

TEST_F(KeysetManagementTest, AddKeysetInvalidCreds) {
  KeysetSetUp();

  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);
  int index = -1;

  EXPECT_CALL(platform_, DeleteFile(_, _))
    .Times(0);
  // Try to authenticate with an unknown key.
  UsernamePasskey bad_p(test_helper_.users[1].username, newkey);
  ASSERT_EQ(CRYPTOHOME_ERROR_AUTHORIZATION_KEY_FAILED,
            homedirs_.AddKeyset(bad_p, newkey, NULL, false, &index));
  EXPECT_EQ(index, -1);
}

TEST_F(KeysetManagementTest, AddKeysetInvalidPrivileges) {
  // Check for key use that lacks valid add privileges
  KeysetSetUp();

  // The injected keyset in the fixture handles the up_ validation.
  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);

  serialized_.mutable_key_data()->mutable_privileges()->set_add(false);
  int index = -1;
  // Tery to authenticate with a key that cannot add keys.
  ASSERT_EQ(CRYPTOHOME_ERROR_AUTHORIZATION_KEY_DENIED,
            homedirs_.AddKeyset(*up_, newkey, NULL, false, &index));
  EXPECT_EQ(index, -1);
}

TEST_F(KeysetManagementTest, AddKeyset0Available) {
  // While this doesn't affect the hole-finding logic, it's good to cover the
  // full logical behavior by changing which key auths too.
  // master.0 -> master.1
  test_helper_.users[1].keyset_path.erase(
    test_helper_.users[1].keyset_path.end() - 1);
  test_helper_.users[1].keyset_path.append("1");
  KeysetSetUp();

  // The injected keyset in the fixture handles the up_ validation.
  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);

  EXPECT_CALL(platform_, OpenFile(EndsWith("master.0"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));
  EXPECT_CALL(*active_vk_, Encrypt(newkey))
    .WillOnce(Return(true));
  EXPECT_CALL(*active_vk_, Save(EndsWith("master.0")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(_, _))
    .Times(0);

  int index = -1;
  // Try to authenticate with an unknown key.
  ASSERT_EQ(CRYPTOHOME_ERROR_NOT_SET,
            homedirs_.AddKeyset(*up_, newkey, NULL, false, &index));
  EXPECT_EQ(index, 0);
}

TEST_F(KeysetManagementTest, AddKeyset10Available) {
  KeysetSetUp();

  // The injected keyset in the fixture handles the up_ validation.
  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);

  EXPECT_CALL(platform_, OpenFile(MatchesRegex(".*/master\\..$"), StrEq("wx")))
    .Times(10)
    .WillRepeatedly(Return(reinterpret_cast<FILE*>(NULL)));
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.10"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));
  EXPECT_CALL(platform_, DeleteFile(_, _))
    .Times(0);
  EXPECT_CALL(*active_vk_, Encrypt(newkey))
    .WillOnce(Return(true));
  EXPECT_CALL(*active_vk_, Save(EndsWith("master.10")))
    .WillOnce(Return(true));

  int index = -1;
  ASSERT_EQ(CRYPTOHOME_ERROR_NOT_SET,
            homedirs_.AddKeyset(*up_, newkey, NULL, false, &index));
  EXPECT_EQ(index, 10);
}

TEST_F(KeysetManagementTest, AddKeysetNoFreeIndices) {
  KeysetSetUp();

  // The injected keyset in the fixture handles the up_ validation.
  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);

  EXPECT_CALL(platform_, OpenFile(MatchesRegex(".*/master\\..*$"), StrEq("wx")))
    .Times(kKeyFileMax)
    .WillRepeatedly(Return(reinterpret_cast<FILE*>(NULL)));
  EXPECT_CALL(platform_, DeleteFile(_, _))
    .Times(0);

  int index = -1;
  ASSERT_EQ(CRYPTOHOME_ERROR_KEY_QUOTA_EXCEEDED,
            homedirs_.AddKeyset(*up_, newkey, NULL, false, &index));
  EXPECT_EQ(index, -1);
}

TEST_F(KeysetManagementTest, AddKeysetEncryptFail) {
  KeysetSetUp();

  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);
  int index = -1;
  // The injected keyset in the fixture handles the up_ validation.
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.0"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));
  EXPECT_CALL(*active_vk_, Encrypt(newkey))
    .WillOnce(Return(false));
  EXPECT_CALL(platform_, CloseFile(reinterpret_cast<FILE*>(0xbeefbeef)))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(EndsWith("master.0"), false))
    .WillOnce(Return(true));
  ASSERT_EQ(CRYPTOHOME_ERROR_BACKING_STORE_FAILURE,
            homedirs_.AddKeyset(*up_, newkey, NULL, false, &index));
  EXPECT_EQ(index, -1);
}

TEST_F(KeysetManagementTest, AddKeysetSaveFail) {
  KeysetSetUp();

  cryptohome::SecureBlob newkey;
  cryptohome::Crypto::PasswordToPasskey("why not", system_salt_, &newkey);
  int index = -1;
  // The injected keyset in the fixture handles the up_ validation.
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.0"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));
  EXPECT_CALL(*active_vk_, Encrypt(newkey))
    .WillOnce(Return(true));
  EXPECT_CALL(*active_vk_, Save(EndsWith("master.0")))
    .WillOnce(Return(false));
  EXPECT_CALL(platform_, CloseFile(reinterpret_cast<FILE*>(0xbeefbeef)))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(EndsWith("master.0"), false))
    .WillOnce(Return(true));
  ASSERT_EQ(CRYPTOHOME_ERROR_BACKING_STORE_FAILURE,
            homedirs_.AddKeyset(*up_, newkey, NULL, false, &index));
  EXPECT_EQ(index, -1);
}

TEST_F(KeysetManagementTest, ForceRemoveKeysetSuccess) {
  EXPECT_CALL(platform_, FileExists(EndsWith("master.0")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(EndsWith("master.0"), false))
    .WillOnce(Return(true));
  ASSERT_TRUE(homedirs_.ForceRemoveKeyset("a0b0c0", 0));
}

TEST_F(KeysetManagementTest, ForceRemoveKeysetMissingKeyset) {
  EXPECT_CALL(platform_, FileExists(EndsWith("master.0")))
    .WillOnce(Return(false));
  ASSERT_TRUE(homedirs_.ForceRemoveKeyset("a0b0c0", 0));
}

TEST_F(KeysetManagementTest, ForceRemoveKeysetNegativeIndex) {
  ASSERT_FALSE(homedirs_.ForceRemoveKeyset("a0b0c0", -1));
}

TEST_F(KeysetManagementTest, ForceRemoveKeysetOverMaxIndex) {
  ASSERT_FALSE(homedirs_.ForceRemoveKeyset("a0b0c0", kKeyFileMax));
}

TEST_F(KeysetManagementTest, ForceRemoveKeysetFailedDelete) {
  EXPECT_CALL(platform_, FileExists(EndsWith("master.0")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, DeleteFile(EndsWith("master.0"), false))
    .WillOnce(Return(false));
  ASSERT_FALSE(homedirs_.ForceRemoveKeyset("a0b0c0", 0));
}

TEST_F(KeysetManagementTest, MoveKeysetSuccess_0_to_1) {
  const std::string obfuscated = "a0b0c0";
  EXPECT_CALL(platform_, FileExists(EndsWith("master.0")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, FileExists(EndsWith("master.1")))
    .WillOnce(Return(false));
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.1"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));
  EXPECT_CALL(platform_, Rename(EndsWith("master.0"), EndsWith("master.1")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, CloseFile(reinterpret_cast<FILE*>(0xbeefbeef)))
    .WillOnce(Return(true));
  ASSERT_TRUE(homedirs_.MoveKeyset(obfuscated, 0, 1));
}

TEST_F(KeysetManagementTest, MoveKeysetSuccess_1_to_99) {
  const std::string obfuscated = "a0b0c0";
  EXPECT_CALL(platform_, FileExists(EndsWith("master.1")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, FileExists(EndsWith("master.99")))
    .WillOnce(Return(false));
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.99"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));
  EXPECT_CALL(platform_, Rename(EndsWith("master.1"), EndsWith("master.99")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, CloseFile(reinterpret_cast<FILE*>(0xbeefbeef)))
    .WillOnce(Return(true));
  ASSERT_TRUE(homedirs_.MoveKeyset(obfuscated, 1, 99));
}

TEST_F(KeysetManagementTest, MoveKeysetNegativeSource) {
  const std::string obfuscated = "a0b0c0";
  ASSERT_FALSE(homedirs_.MoveKeyset(obfuscated, -1, 1));
}

TEST_F(KeysetManagementTest, MoveKeysetNegativeDestination) {
  const std::string obfuscated = "a0b0c0";
  ASSERT_FALSE(homedirs_.MoveKeyset(obfuscated, 1, -1));
}

TEST_F(KeysetManagementTest, MoveKeysetTooLargeDestination) {
  const std::string obfuscated = "a0b0c0";
  ASSERT_FALSE(homedirs_.MoveKeyset(obfuscated, 1, kKeyFileMax));
}

TEST_F(KeysetManagementTest, MoveKeysetTooLargeSource) {
  const std::string obfuscated = "a0b0c0";
  ASSERT_FALSE(homedirs_.MoveKeyset(obfuscated, kKeyFileMax, 0));
}

TEST_F(KeysetManagementTest, MoveKeysetMissingSource) {
  const std::string obfuscated = "a0b0c0";
  EXPECT_CALL(platform_, FileExists(EndsWith("master.0")))
    .WillOnce(Return(false));
  ASSERT_FALSE(homedirs_.MoveKeyset(obfuscated, 0, 1));
}

TEST_F(KeysetManagementTest, MoveKeysetDestinationExists) {
  const std::string obfuscated = "a0b0c0";
  EXPECT_CALL(platform_, FileExists(EndsWith("master.0")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, FileExists(EndsWith("master.1")))
    .WillOnce(Return(true));
  ASSERT_FALSE(homedirs_.MoveKeyset(obfuscated, 0, 1));
}

TEST_F(KeysetManagementTest, MoveKeysetExclusiveOpenFailed) {
  const std::string obfuscated = "a0b0c0";
  EXPECT_CALL(platform_, FileExists(EndsWith("master.0")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, FileExists(EndsWith("master.1")))
    .WillOnce(Return(false));
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.1"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(NULL)));
  ASSERT_FALSE(homedirs_.MoveKeyset(obfuscated, 0, 1));
}

TEST_F(KeysetManagementTest, MoveKeysetRenameFailed) {
  const std::string obfuscated = "a0b0c0";
  EXPECT_CALL(platform_, FileExists(EndsWith("master.0")))
    .WillOnce(Return(true));
  EXPECT_CALL(platform_, FileExists(EndsWith("master.1")))
    .WillOnce(Return(false));
  EXPECT_CALL(platform_, OpenFile(EndsWith("master.1"), StrEq("wx")))
    .WillOnce(Return(reinterpret_cast<FILE*>(0xbeefbeef)));
  EXPECT_CALL(platform_, Rename(EndsWith("master.0"), EndsWith("master.1")))
    .WillOnce(Return(false));
  EXPECT_CALL(platform_, CloseFile(reinterpret_cast<FILE*>(0xbeefbeef)))
    .WillOnce(Return(true));
  ASSERT_FALSE(homedirs_.MoveKeyset(obfuscated, 0, 1));
}

}  // namespace cryptohome
