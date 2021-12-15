// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_session_manager.h"

#include <memory>
#include <string>
#include <utility>

#include <base/test/task_environment.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cryptohome/mock_keyset_management.h"

namespace cryptohome {

class AuthSessionManagerTest : public ::testing::Test {
 public:
  AuthSessionManagerTest() = default;
  ~AuthSessionManagerTest() override = default;
  AuthSessionManagerTest(const AuthSessionManagerTest&) = delete;
  AuthSessionManagerTest& operator=(AuthSessionManagerTest&) = delete;
};

namespace {

using ::testing::_;
using ::testing::ByMove;
using ::testing::Eq;
using ::testing::IsNull;
using ::testing::NiceMock;
using ::testing::NotNull;
using ::testing::Return;

using base::test::TaskEnvironment;

TEST_F(AuthSessionManagerTest, CreateFindRemove) {
  TaskEnvironment task_environment(
      TaskEnvironment::ThreadPoolExecutionMode::QUEUED);

  NiceMock<MockKeysetManagement> keyset_management;
  AuthSessionManager auth_session_manager(&keyset_management);

  AuthSession* auth_session =
      auth_session_manager.CreateAuthSession("foo@example.com", 0);
  ASSERT_THAT(auth_session, NotNull());
  base::UnguessableToken token = auth_session->token();
  ASSERT_THAT(auth_session_manager.FindAuthSession(token), Eq(auth_session));
  auth_session_manager.RemoveAuthSession(token);
  ASSERT_THAT(auth_session_manager.FindAuthSession(token), IsNull());

  // Repeat with serialized_token overload.
  auth_session = auth_session_manager.CreateAuthSession("foo@example.com", 0);
  ASSERT_THAT(auth_session, NotNull());
  std::string serialized_token = auth_session->serialized_token();
  ASSERT_THAT(auth_session_manager.FindAuthSession(serialized_token),
              Eq(auth_session));
  auth_session_manager.RemoveAuthSession(serialized_token);
  ASSERT_THAT(auth_session_manager.FindAuthSession(serialized_token), IsNull());
}

TEST_F(AuthSessionManagerTest, CreateExpire) {
  TaskEnvironment task_environment(
      TaskEnvironment::TimeSource::MOCK_TIME,
      TaskEnvironment::ThreadPoolExecutionMode::QUEUED);

  NiceMock<MockKeysetManagement> keyset_management;
  AuthSessionManager auth_session_manager(&keyset_management);

  AuthSession* auth_session =
      auth_session_manager.CreateAuthSession("foo@example.com", 0);
  ASSERT_THAT(auth_session, NotNull());
  base::UnguessableToken token = auth_session->token();
  ASSERT_THAT(auth_session_manager.FindAuthSession(token), Eq(auth_session));

  task_environment.FastForwardUntilNoTasksRemain();
  ASSERT_THAT(auth_session_manager.FindAuthSession(token), IsNull());
}

}  // namespace

}  // namespace cryptohome