// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Tests for the ChallengeCredentialsDecryptOperation class.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <brillo/secure_blob.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cryptohome/challenge_credentials/challenge_credentials_decrypt_operation.h"
#include "cryptohome/challenge_credentials/challenge_credentials_test_utils.h"
#include "cryptohome/cryptolib.h"
#include "cryptohome/mock_key_challenge_service.h"
#include "cryptohome/mock_signature_sealing_backend.h"
#include "cryptohome/mock_tpm.h"
#include "cryptohome/signature_sealing_backend.h"
#include "cryptohome/signature_sealing_backend_test_utils.h"
#include "cryptohome/username_passkey.h"

#include "key.pb.h"                    // NOLINT(build/include)
#include "rpc.pb.h"                    // NOLINT(build/include)
#include "signature_sealed_data.pb.h"  // NOLINT(build/include)

using brillo::Blob;
using brillo::BlobToString;
using brillo::CombineBlobs;
using testing::Return;
using testing::StrictMock;
using testing::Values;

namespace cryptohome {

using KeysetSignatureChallengeInfo =
    ChallengeCredentialsDecryptOperation::KeysetSignatureChallengeInfo;

namespace {

KeyData MakeKeyData(
    const Blob& set_public_key_spki_der,
    const std::vector<ChallengeSignatureAlgorithm>& key_algorithms) {
  KeyData key_data;
  key_data.set_type(KeyData::KEY_TYPE_CHALLENGE_RESPONSE);
  ChallengePublicKeyInfo* const public_key_info =
      key_data.add_challenge_response_key();
  public_key_info->set_public_key_spki_der(
      BlobToString(set_public_key_spki_der));
  for (auto key_algorithm : key_algorithms)
    public_key_info->add_signature_algorithm(key_algorithm);
  return key_data;
}

KeysetSignatureChallengeInfo MakeFakeKeysetChallengeInfo(
    const Blob& public_key_spki_der,
    const Blob& salt,
    ChallengeSignatureAlgorithm salt_challenge_algorithm) {
  KeysetSignatureChallengeInfo keyset_challenge_info;
  keyset_challenge_info.set_public_key_spki_der(
      BlobToString(public_key_spki_der));
  *keyset_challenge_info.mutable_sealed_secret() =
      MakeFakeSignatureSealedData(public_key_spki_der);
  keyset_challenge_info.set_salt(BlobToString(salt));
  keyset_challenge_info.set_salt_signature_algorithm(salt_challenge_algorithm);
  return keyset_challenge_info;
}

// Base fixture class that provides some common constants, helpers and mocks for
// testing a single instance of ChallengeCredentialsDecryptOperation.
class ChallengeCredentialsDecryptOperationTestBase : public testing::Test {
 protected:
  void PrepareSignatureSealingBackend(bool enabled) {
    SignatureSealingBackend* const return_value =
        enabled ? &sealing_backend_ : nullptr;
    EXPECT_CALL(tpm_, GetSignatureSealingBackend())
        .WillRepeatedly(Return(return_value));
  }

  // Create an instance of ChallengeCredentialsDecryptOperation to be tested.
  void CreateOperation(
      const std::vector<ChallengeSignatureAlgorithm>& key_algorithms,
      ChallengeSignatureAlgorithm salt_challenge_algorithm,
      const Blob& salt) {
    DCHECK(!operation_);
    const KeyData key_data = MakeKeyData(kPublicKeySpkiDer, key_algorithms);
    const KeysetSignatureChallengeInfo keyset_challenge_info =
        MakeFakeKeysetChallengeInfo(kPublicKeySpkiDer, salt,
                                    salt_challenge_algorithm);
    operation_ = std::make_unique<ChallengeCredentialsDecryptOperation>(
        &challenge_service_, &tpm_, kDelegateBlob, kDelegateSecret, kUserEmail,
        key_data, keyset_challenge_info,
        MakeChallengeCredentialsDecryptResultWriter(&operation_result_));
  }

  void StartOperation() { operation_->Start(); }

  void AbortOperation() { operation_->Abort(); }

  // Whether the tested operation completed with some result.
  bool has_result() const { return static_cast<bool>(operation_result_); }

  // Assert that the tested operation completed with a valid success result.
  void VerifySuccessfulResult() const {
    ASSERT_TRUE(operation_result_);
    VerifySuccessfulChallengeCredentialsDecryptResult(
      *operation_result_,
      kUserEmail,
      brillo::SecureBlob(kPasskey.begin(), kPasskey.end()));
  }

  // Assert that the tested operation completed with a failure result.
  void VerifyFailedResult() const {
    ASSERT_TRUE(operation_result_);
    VerifyFailedChallengeCredentialsDecryptResult(*operation_result_);
  }

  // Returns a helper object that aids mocking of the secret unsealing
  // functionality (SignatureSealingBackend::CreateUnsealingSession() et al.).
  std::unique_ptr<SignatureSealedUnsealingMocker> MakeUnsealingMocker(
      const std::vector<ChallengeSignatureAlgorithm>& key_algorithms,
      ChallengeSignatureAlgorithm unsealing_algorithm) {
    auto mocker =
        std::make_unique<SignatureSealedUnsealingMocker>(&sealing_backend_);
    mocker->set_public_key_spki_der(kPublicKeySpkiDer);
    mocker->set_key_algorithms(key_algorithms);
    mocker->set_delegate_blob(kDelegateBlob);
    mocker->set_delegate_secret(kDelegateSecret);
    mocker->set_chosen_algorithm(unsealing_algorithm);
    mocker->set_challenge_value(kUnsealingChallengeValue);
    mocker->set_challenge_signature(kUnsealingChallengeSignature);
    mocker->set_secret_value(kUnsealedSecret);
    return mocker;
  }

  // Sets up an expectation that the salt challenge request will be issued via
  // |challenge_service_|.
  void ExpectSaltChallenge(
      ChallengeSignatureAlgorithm salt_challenge_algorithm) {
    salt_challenge_mock_controller_.ExpectSignatureChallenge(
        kUserEmail, kPublicKeySpkiDer, kSalt, salt_challenge_algorithm);
  }

  // Whether the salt challenge request has been started.
  bool is_salt_challenge_requested() const {
    return salt_challenge_mock_controller_.is_challenge_requested();
  }

  // Injects a simulated successful response for the currently running salt
  // challenge request.
  void SimulateSaltChallengeResponse() {
    salt_challenge_mock_controller_.SimulateSignatureChallengeResponse(
        kSaltSignature);
  }

  // Injects a simulated failure response for the currently running salt
  // challenge request.
  void SimulateSaltChallengeFailure() {
    salt_challenge_mock_controller_.SimulateFailureResponse();
  }

  // Sets up an expectation that the secret unsealing challenge request will be
  // issued via |challenge_service_|.
  void ExpectUnsealingChallenge(
      ChallengeSignatureAlgorithm unsealing_algorithm) {
    unsealing_challenge_mock_controller_.ExpectSignatureChallenge(
        kUserEmail, kPublicKeySpkiDer, kUnsealingChallengeValue,
        unsealing_algorithm);
  }

  // Whether the secret unsealing challenge request has been started.
  bool is_unsealing_challenge_requested() const {
    return unsealing_challenge_mock_controller_.is_challenge_requested();
  }

  // Injects a simulated successful response for the currently running secret
  // unsealing challenge request.
  void SimulateUnsealingChallengeResponse() {
    unsealing_challenge_mock_controller_.SimulateSignatureChallengeResponse(
        kUnsealingChallengeSignature);
  }

  // Injects a simulated failure response for the currently running secret
  // unsealing challenge request.
  void SimulateUnsealingChallengeFailure() {
    unsealing_challenge_mock_controller_.SimulateFailureResponse();
  }

 protected:
  // Constants which are passed as fake data inputs to the
  // ChallengeCredentialsDecryptOperation operation:

  // Fake TPM delegate. It's supplied to the operation constructor. Then it's
  // verified to be passed into SignatureSealingBackend methods.
  const Blob kDelegateBlob{{1, 1, 1}};
  const Blob kDelegateSecret{{2, 2, 2}};
  // Fake user e-mail. It's supplied to the operation constructor. Then it's
  // verified to be passed alongside challenge requests made via
  // KeyChallengeService, and to be present in the resulting UsernamePasskey.
  const std::string kUserEmail = "foo@example.com";
  // Fake Subject Public Key Information of the challenged cryptographic key.
  // It's supplied to the operation as a field of both |key_data| and
  // |keyset_challenge_info| parameters. Then it's verified to be passed into
  // SignatureSealingBackend methods and to be used for challenge requests made
  // via KeyChallengeService.
  const Blob kPublicKeySpkiDer{{3, 3, 3}};
  // Fake salt value. It's supplied to the operation as a field of the
  // |keyset_challenge_info| parameter. Then it's verified to be used as the
  // challenge value for one of requests made via KeyChallengeService.
  const Blob kSalt = CombineBlobs(
      {ChallengeCredentialsOperation::GetSaltConstantPrefix(), Blob{4, 4, 4}});

  // Constants which are injected as fake data into intermediate steps of the
  // tested operation:

  // Fake signature of |kSalt| using the |salt_challenge_algorithm_| algorithm.
  // It's injected as a fake response to the salt challenge request made via
  // KeyChallengeService. Then it's implicitly verified to be used for the
  // generation of the passkey in the resulting UsernamePasskey - see the
  // |kPasskey| constant.
  const Blob kSaltSignature{{5, 5, 5}};
  // Fake challenge value for unsealing the secret. It's injected as a fake
  // value returned from SignatureSealingBackend::UnsealingSession. Then it's
  // verified to be used as the challenge value for one of requests made via
  // KeyChallengeService.
  const Blob kUnsealingChallengeValue{{6, 6, 6}};
  // Fake signature of |kUnsealingChallengeValue| using the
  // |unsealing_algorithm_| algorithm. It's injected as a fake response to the
  // unsealing challenge request made via KeyChallengeService. Then it's
  // verified to be passed to the Unseal() method of
  // SignatureSealingBackend::UnsealingSession.
  const Blob kUnsealingChallengeSignature{{7, 7, 7}};
  // Fake unsealed secret. It's injected as a fake result of the Unseal() method
  // of SignatureSealingBackend::UnsealingSession.
  const Blob kUnsealedSecret{{8, 8, 8}};

  // The expected passkey of the resulting UsernamePasskey returned from the
  // tested operation. Its value is derived from the injected fake data.
  const Blob kPasskey =
      CombineBlobs({kUnsealedSecret, CryptoLib::Sha256(kSaltSignature)});

 private:
  // Mock objects:

  StrictMock<MockTpm> tpm_;
  StrictMock<MockSignatureSealingBackend> sealing_backend_;
  StrictMock<MockKeyChallengeService> challenge_service_;
  KeyChallengeServiceMockController salt_challenge_mock_controller_{
      &challenge_service_};
  KeyChallengeServiceMockController unsealing_challenge_mock_controller_{
      &challenge_service_};

  // Result returned from the tested operation, or null if nothing yet.
  std::unique_ptr<ChallengeCredentialsDecryptResult> operation_result_;
  // The tested operation.
  std::unique_ptr<ChallengeCredentialsDecryptOperation> operation_;
};

// Base fixture class that uses a single algorithm for simplicity.
class ChallengeCredentialsDecryptOperationSingleAlgorithmTestBase
    : public ChallengeCredentialsDecryptOperationTestBase {
 protected:
  // The single algorithm to be used in this test.
  static constexpr ChallengeSignatureAlgorithm kAlgorithm =
      CHALLENGE_RSASSA_PKCS1_V1_5_SHA256;
};

// Base fixture class that uses a single algorithm and have the sealing backend
// available.
class ChallengeCredentialsDecryptOperationNoOperationConstructedBasicTest
    : public ChallengeCredentialsDecryptOperationSingleAlgorithmTestBase {
 protected:
  // The single algorithm to be used in this test.
  static constexpr ChallengeSignatureAlgorithm kAlgorithm =
      CHALLENGE_RSASSA_PKCS1_V1_5_SHA256;

  ChallengeCredentialsDecryptOperationNoOperationConstructedBasicTest() {
    PrepareSignatureSealingBackend(true /* enabled */);
  }
};

}  // namespace

// Test failure of the operation due to the input salt being empty.
TEST_F(ChallengeCredentialsDecryptOperationNoOperationConstructedBasicTest,
       EmptySaltFailure) {
  CreateOperation({kAlgorithm} /* key_algorithms */,
                  kAlgorithm /* salt_challenge_algorithm */, Blob() /* salt */);
  StartOperation();
  VerifyFailedResult();
}

// Test failure of the operation due to the input salt not starting with the
// expected constant prefix.
TEST_F(ChallengeCredentialsDecryptOperationNoOperationConstructedBasicTest,
       BadSaltNotPrefixedFailure) {
  Blob salt = kSalt;
  salt[ChallengeCredentialsOperation::GetSaltConstantPrefix().size() - 1] ^= 1;
  CreateOperation({kAlgorithm} /* key_algorithms */,
                  kAlgorithm /* salt_challenge_algorithm */, salt);
  StartOperation();
  VerifyFailedResult();
}

// Test failure of the operation due to the input salt containing nothing
// besides the expected constant prefix.
TEST_F(ChallengeCredentialsDecryptOperationNoOperationConstructedBasicTest,
       BadSaltNothingBesidesPrefixFailure) {
  CreateOperation(
      {kAlgorithm} /* key_algorithms */,
      kAlgorithm /* salt_challenge_algorithm */,
      ChallengeCredentialsOperation::GetSaltConstantPrefix() /* salt */);
  StartOperation();
  VerifyFailedResult();
}

namespace {

// Basic tests that use a single algorithm and have the sealing backend
// available.
class ChallengeCredentialsDecryptOperationBasicTest
    : public ChallengeCredentialsDecryptOperationSingleAlgorithmTestBase {
 protected:
  ChallengeCredentialsDecryptOperationBasicTest() {
    PrepareSignatureSealingBackend(true /* enabled */);
    CreateOperation({kAlgorithm} /* key_algorithms */,
                    kAlgorithm /* salt_challenge_algorithm */, kSalt);
  }
};

}  // namespace

// Test success of the operation in scenario when the salt challenge response
// comes before the unsealing challenge response.
TEST_F(ChallengeCredentialsDecryptOperationBasicTest,
       SuccessSaltThenUnsealing) {
  ExpectSaltChallenge(kAlgorithm /* salt_challenge_algorithm */);
  ExpectUnsealingChallenge(kAlgorithm /* unsealing_algorithm */);
  MakeUnsealingMocker({kAlgorithm} /* key_algorithms */,
                      kAlgorithm /* unsealing_algorithm */)
      ->SetUpSuccessfulMock();

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  EXPECT_TRUE(is_unsealing_challenge_requested());

  SimulateSaltChallengeResponse();
  EXPECT_FALSE(has_result());

  SimulateUnsealingChallengeResponse();
  VerifySuccessfulResult();
}

// Test success of the operation in scenario when the unsealing challenge
// response comes before the salt challenge response.
TEST_F(ChallengeCredentialsDecryptOperationBasicTest,
       SuccessUnsealingThenSalt) {
  ExpectSaltChallenge(kAlgorithm /* salt_challenge_algorithm */);
  ExpectUnsealingChallenge(kAlgorithm /* unsealing_algorithm */);
  MakeUnsealingMocker({kAlgorithm} /* key_algorithms */,
                      kAlgorithm /* unsealing_algorithm */)
      ->SetUpSuccessfulMock();

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  EXPECT_TRUE(is_unsealing_challenge_requested());

  SimulateUnsealingChallengeResponse();
  EXPECT_FALSE(has_result());

  SimulateSaltChallengeResponse();
  VerifySuccessfulResult();
}

// Test failure of the operation due to failure of unsealing session creation.
TEST_F(ChallengeCredentialsDecryptOperationBasicTest,
       UnsealingSessionCreationFailure) {
  EXPECT_FALSE(has_result());

  ExpectSaltChallenge(kAlgorithm /* salt_challenge_algorithm */);
  MakeUnsealingMocker({kAlgorithm} /* key_algorithms */,
                      kAlgorithm /* unsealing_algorithm */)
      ->SetUpCreationFailingMock(true /* mock_repeatedly */);

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  VerifyFailedResult();

  // Responding to the salt challenge shouldn't have any effect.
  SimulateSaltChallengeResponse();
}

// Test failure of the operation due to failure of unsealing.
TEST_F(ChallengeCredentialsDecryptOperationBasicTest, UnsealingFailure) {
  ExpectSaltChallenge(kAlgorithm /* salt_challenge_algorithm */);
  ExpectUnsealingChallenge(kAlgorithm /* unsealing_algorithm */);
  MakeUnsealingMocker({kAlgorithm} /* key_algorithms */,
                      kAlgorithm /* unsealing_algorithm */)
      ->SetUpUsealingFailingMock();

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  EXPECT_TRUE(is_unsealing_challenge_requested());
  EXPECT_FALSE(has_result());

  SimulateUnsealingChallengeResponse();
  VerifyFailedResult();

  // Responding to the salt challenge shouldn't have any effect.
  SimulateSaltChallengeResponse();
}

// Test failure of the operation due to failure of salt challenge request.
TEST_F(ChallengeCredentialsDecryptOperationBasicTest, SaltChallengeFailure) {
  ExpectSaltChallenge(kAlgorithm /* salt_challenge_algorithm */);
  ExpectUnsealingChallenge(kAlgorithm /* unsealing_algorithm */);
  MakeUnsealingMocker({kAlgorithm} /* key_algorithms */,
                      kAlgorithm /* unsealing_algorithm */)
      ->SetUpUnsealingNotCalledMock();

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  EXPECT_TRUE(is_unsealing_challenge_requested());
  EXPECT_FALSE(has_result());

  SimulateSaltChallengeFailure();
  VerifyFailedResult();

  // Responding to the unsealing challenge shouldn't have any effect.
  SimulateUnsealingChallengeResponse();
}

// Test failure of the operation due to failure of unsealing challenge request.
TEST_F(ChallengeCredentialsDecryptOperationBasicTest,
       UnsealingChallengeFailure) {
  ExpectSaltChallenge(kAlgorithm /* salt_challenge_algorithm */);
  ExpectUnsealingChallenge(kAlgorithm /* unsealing_algorithm */);
  MakeUnsealingMocker({kAlgorithm} /* key_algorithms */,
                      kAlgorithm /* unsealing_algorithm */)
      ->SetUpUnsealingNotCalledMock();

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  EXPECT_TRUE(is_unsealing_challenge_requested());
  EXPECT_FALSE(has_result());

  SimulateUnsealingChallengeFailure();
  VerifyFailedResult();

  // Responding to the salt challenge shouldn't have any effect.
  SimulateSaltChallengeResponse();
}

// Test failure of the operation due to its abortion before any of the
// challenges is completed.
TEST_F(ChallengeCredentialsDecryptOperationBasicTest, AbortBeforeChallenges) {
  ExpectSaltChallenge(kAlgorithm /* salt_challenge_algorithm */);
  ExpectUnsealingChallenge(kAlgorithm /* unsealing_algorithm */);
  MakeUnsealingMocker({kAlgorithm} /* key_algorithms */,
                      kAlgorithm /* unsealing_algorithm */)
      ->SetUpUnsealingNotCalledMock();

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  EXPECT_TRUE(is_unsealing_challenge_requested());
  EXPECT_FALSE(has_result());

  AbortOperation();
  VerifyFailedResult();

  // Responding to the challenges shouldn't have any effect.
  SimulateSaltChallengeResponse();
  SimulateUnsealingChallengeResponse();
}

// Test failure of the operation due to its abortion after the salt challenge
// completes.
TEST_F(ChallengeCredentialsDecryptOperationBasicTest, AbortAfterSaltChallenge) {
  ExpectSaltChallenge(kAlgorithm /* salt_challenge_algorithm */);
  ExpectUnsealingChallenge(kAlgorithm /* unsealing_algorithm */);
  MakeUnsealingMocker({kAlgorithm} /* key_algorithms */,
                      kAlgorithm /* unsealing_algorithm */)
      ->SetUpUnsealingNotCalledMock();

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  EXPECT_TRUE(is_unsealing_challenge_requested());

  SimulateSaltChallengeResponse();
  EXPECT_FALSE(has_result());

  AbortOperation();
  VerifyFailedResult();

  // Responding to the unsealing challenge shouldn't have any effect.
  SimulateUnsealingChallengeResponse();
}

// Test failure of the operation due to its abortion after the unsealing
// completes.
TEST_F(ChallengeCredentialsDecryptOperationBasicTest, AbortAfterUnsealing) {
  ExpectSaltChallenge(kAlgorithm /* salt_challenge_algorithm */);
  ExpectUnsealingChallenge(kAlgorithm /* unsealing_algorithm */);
  MakeUnsealingMocker({kAlgorithm} /* key_algorithms */,
                      kAlgorithm /* unsealing_algorithm */)
      ->SetUpSuccessfulMock();

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  EXPECT_TRUE(is_unsealing_challenge_requested());

  SimulateUnsealingChallengeResponse();
  EXPECT_FALSE(has_result());

  AbortOperation();
  VerifyFailedResult();

  // Responding to the salt challenge shouldn't have any effect.
  SimulateSaltChallengeResponse();
}

namespace {

// Tests with simulation of SignatureSealingBackend absence.
class ChallengeCredentialsDecryptOperationNoBackendTest
    : public ChallengeCredentialsDecryptOperationSingleAlgorithmTestBase {
 protected:
  ChallengeCredentialsDecryptOperationNoBackendTest() {
    PrepareSignatureSealingBackend(false /* enabled */);
    CreateOperation({kAlgorithm} /* key_algorithms */,
                    kAlgorithm /* salt_challenge_algorithm */, kSalt);
  }
};

}  // namespace

// Test failure of the operation due to the absence of the sealing backend.
TEST_F(ChallengeCredentialsDecryptOperationNoBackendTest, Failure) {
  EXPECT_FALSE(has_result());

  StartOperation();
  VerifyFailedResult();
}

namespace {

// Test parameters for ChallengeCredentialsDecryptOperationAlgorithmsTest.
struct AlgorithmsTestParam {
  std::vector<ChallengeSignatureAlgorithm> key_algorithms;
  ChallengeSignatureAlgorithm salt_challenge_algorithm;
  ChallengeSignatureAlgorithm unsealing_algorithm;
};

// Tests various combinations of multiple algorithms.
class ChallengeCredentialsDecryptOperationAlgorithmsTest
    : public ChallengeCredentialsDecryptOperationTestBase,
      public testing::WithParamInterface<AlgorithmsTestParam> {
 protected:
  ChallengeCredentialsDecryptOperationAlgorithmsTest() {
    PrepareSignatureSealingBackend(true /* enabled */);
    CreateOperation(GetParam().key_algorithms,
                    GetParam().salt_challenge_algorithm, kSalt);
  }
};

}  // namespace

// Test success of the operation with the specified combination of algorithms.
TEST_P(ChallengeCredentialsDecryptOperationAlgorithmsTest, Success) {
  ExpectSaltChallenge(GetParam().salt_challenge_algorithm);
  ExpectUnsealingChallenge(GetParam().unsealing_algorithm);
  MakeUnsealingMocker(GetParam().key_algorithms, GetParam().unsealing_algorithm)
      ->SetUpSuccessfulMock();

  StartOperation();
  EXPECT_TRUE(is_salt_challenge_requested());
  EXPECT_TRUE(is_unsealing_challenge_requested());

  SimulateSaltChallengeResponse();
  EXPECT_FALSE(has_result());

  SimulateUnsealingChallengeResponse();
  VerifySuccessfulResult();
}

// Test that SHA-1 algorithms are the least preferred and chosen only if there's
// no other option.
INSTANTIATE_TEST_CASE_P(
    LowPriorityOfSha1,
    ChallengeCredentialsDecryptOperationAlgorithmsTest,
    Values(
        AlgorithmsTestParam{
            {CHALLENGE_RSASSA_PKCS1_V1_5_SHA1,
             CHALLENGE_RSASSA_PKCS1_V1_5_SHA256} /* key_algorithms */,
            CHALLENGE_RSASSA_PKCS1_V1_5_SHA256 /* salt_challenge_algorithm */,
            CHALLENGE_RSASSA_PKCS1_V1_5_SHA256 /* unsealing_algorithm */},
        AlgorithmsTestParam{
            {CHALLENGE_RSASSA_PKCS1_V1_5_SHA1} /* key_algorithms */,
            CHALLENGE_RSASSA_PKCS1_V1_5_SHA1 /* salt_challenge_algorithm */,
            CHALLENGE_RSASSA_PKCS1_V1_5_SHA1 /* unsealing_algorithm */}));

// Test prioritization of algorithms according to their order in the input.
INSTANTIATE_TEST_CASE_P(
    InputPrioritization,
    ChallengeCredentialsDecryptOperationAlgorithmsTest,
    Values(
        AlgorithmsTestParam{
            {CHALLENGE_RSASSA_PKCS1_V1_5_SHA256,
             CHALLENGE_RSASSA_PKCS1_V1_5_SHA512} /* key_algorithms */,
            CHALLENGE_RSASSA_PKCS1_V1_5_SHA256 /* salt_challenge_algorithm */,
            CHALLENGE_RSASSA_PKCS1_V1_5_SHA256 /* unsealing_algorithm */},
        AlgorithmsTestParam{
            {CHALLENGE_RSASSA_PKCS1_V1_5_SHA512,
             CHALLENGE_RSASSA_PKCS1_V1_5_SHA256} /* key_algorithms */,
            CHALLENGE_RSASSA_PKCS1_V1_5_SHA512 /* salt_challenge_algorithm */,
            CHALLENGE_RSASSA_PKCS1_V1_5_SHA512 /* unsealing_algorithm */}));

}  // namespace cryptohome
