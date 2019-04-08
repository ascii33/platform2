// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/challenge_credentials/challenge_credentials_decrypt_operation.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <base/bind.h>
#include <base/logging.h>

#include "cryptohome/cryptolib.h"
#include "cryptohome/tpm.h"
#include "cryptohome/username_passkey.h"

#include "signature_sealed_data.pb.h"  // NOLINT(build/include)

using brillo::Blob;
using brillo::BlobFromString;
using brillo::SecureBlob;

namespace cryptohome {

namespace {

std::vector<ChallengeSignatureAlgorithm> GetSealingAlgorithms(
    const ChallengePublicKeyInfo& public_key_info) {
  std::vector<ChallengeSignatureAlgorithm> sealing_algorithms;
  for (int index = 0; index < public_key_info.signature_algorithm_size();
       ++index) {
    sealing_algorithms.push_back(public_key_info.signature_algorithm(index));
  }
  return sealing_algorithms;
}

}  // namespace

ChallengeCredentialsDecryptOperation::ChallengeCredentialsDecryptOperation(
    KeyChallengeService* key_challenge_service,
    Tpm* tpm,
    const Blob& delegate_blob,
    const Blob& delegate_secret,
    const std::string& account_id,
    const KeyData& key_data,
    const KeysetSignatureChallengeInfo& keyset_challenge_info,
    const CompletionCallback& completion_callback)
    : ChallengeCredentialsOperation(key_challenge_service),
      tpm_(tpm),
      delegate_blob_(delegate_blob),
      delegate_secret_(delegate_secret),
      account_id_(account_id),
      key_data_(key_data),
      keyset_challenge_info_(keyset_challenge_info),
      completion_callback_(completion_callback),
      signature_sealing_backend_(tpm_->GetSignatureSealingBackend()) {
  DCHECK_EQ(key_data.type(), KeyData::KEY_TYPE_CHALLENGE_RESPONSE);
}

ChallengeCredentialsDecryptOperation::~ChallengeCredentialsDecryptOperation() =
    default;

void ChallengeCredentialsDecryptOperation::Start() {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (!StartProcessing()) {
    LOG(ERROR) << "Failed to start the decryption operation";
    Abort();
    // |this| can be already destroyed at this point.
  }
}

void ChallengeCredentialsDecryptOperation::Abort() {
  DCHECK(thread_checker_.CalledOnValidThread());
  // Invalidate weak pointers in order to cancel all jobs that are currently
  // waiting, to prevent them from running and consuming resources after our
  // abortion (in case |this| doesn't get destroyed immediately).
  //
  // Note that the already issued challenge requests don't get cancelled, so
  // their responses will be just ignored should they arrive later. The request
  // cancellation is not supported by the challenges IPC API currently, neither
  // it is supported by the API for smart card drivers in Chrome OS.
  weak_ptr_factory_.InvalidateWeakPtrs();
  Complete(&completion_callback_, nullptr /* username_passkey */);
  // |this| can be already destroyed at this point.
}

bool ChallengeCredentialsDecryptOperation::StartProcessing() {
  if (!signature_sealing_backend_) {
    LOG(ERROR) << "Signature sealing is disabled";
    return false;
  }
  if (!key_data_.challenge_response_key_size()) {
    LOG(ERROR) << "Missing challenge-response key information";
    return false;
  }
  if (key_data_.challenge_response_key_size() > 1) {
    LOG(ERROR)
        << "Using multiple challenge-response keys at once is unsupported";
    return false;
  }
  public_key_info_ = key_data_.challenge_response_key(0);
  if (!public_key_info_.signature_algorithm_size()) {
    LOG(ERROR) << "The key does not support any signature algorithm";
    return false;
  }
  if (public_key_info_.public_key_spki_der() !=
      keyset_challenge_info_.public_key_spki_der()) {
    LOG(ERROR) << "Wrong public key";
    return false;
  }
  if (!StartProcessingSalt())
    return false;
  // TODO(crbug.com/842791): This is buggy: |this| may be already deleted by
  // that point, in case when the salt's challenge request failed synchronously.
  return StartProcessingSealedSecret();
}

bool ChallengeCredentialsDecryptOperation::StartProcessingSalt() {
  if (!keyset_challenge_info_.has_salt()) {
    LOG(ERROR) << "Missing salt";
    return false;
  }
  const Blob salt = BlobFromString(keyset_challenge_info_.salt());
  // IMPORTANT: Verify that the salt is correctly prefixed. See the comment on
  // ChallengeCredentialsOperation::GetSaltConstantPrefix() for details. Note
  // also that, as an extra validation, we require the salt to contain at least
  // one extra byte after the prefix.
  const Blob& salt_constant_prefix = GetSaltConstantPrefix();
  if (salt.size() <= salt_constant_prefix.size() ||
      !std::equal(salt_constant_prefix.begin(), salt_constant_prefix.end(),
                  salt.begin())) {
    LOG(ERROR) << "Bad salt: not correctly prefixed";
    return false;
  }
  if (!keyset_challenge_info_.has_salt_signature_algorithm()) {
    LOG(ERROR) << "Missing signature algorithm for salt";
    return false;
  }
  MakeKeySignatureChallenge(
      account_id_, BlobFromString(public_key_info_.public_key_spki_der()), salt,
      keyset_challenge_info_.salt_signature_algorithm(),
      base::Bind(&ChallengeCredentialsDecryptOperation::OnSaltChallengeResponse,
                 weak_ptr_factory_.GetWeakPtr()));
  return true;
}

bool ChallengeCredentialsDecryptOperation::StartProcessingSealedSecret() {
  if (!keyset_challenge_info_.has_sealed_secret()) {
    LOG(ERROR) << "Missing sealed secret";
    return false;
  }
  const std::vector<ChallengeSignatureAlgorithm> key_sealing_algorithms =
      GetSealingAlgorithms(public_key_info_);
  unsealing_session_ = signature_sealing_backend_->CreateUnsealingSession(
      keyset_challenge_info_.sealed_secret(),
      BlobFromString(public_key_info_.public_key_spki_der()),
      key_sealing_algorithms, delegate_blob_, delegate_secret_);
  if (!unsealing_session_) {
    LOG(ERROR) << "Failed to start unsealing session for the secret";
    return false;
  }
  MakeKeySignatureChallenge(
      account_id_, BlobFromString(public_key_info_.public_key_spki_der()),
      unsealing_session_->GetChallengeValue(),
      unsealing_session_->GetChallengeAlgorithm(),
      base::Bind(
          &ChallengeCredentialsDecryptOperation::OnUnsealingChallengeResponse,
          weak_ptr_factory_.GetWeakPtr()));
  return true;
}

void ChallengeCredentialsDecryptOperation::OnSaltChallengeResponse(
    std::unique_ptr<Blob> salt_signature) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (!salt_signature) {
    LOG(ERROR) << "Salt signature challenge failed";
    Abort();
    // |this| can be already destroyed at this point.
    return;
  }
  salt_signature_ = std::move(salt_signature);
  ProceedIfChallengesDone();
}

void ChallengeCredentialsDecryptOperation::OnUnsealingChallengeResponse(
    std::unique_ptr<Blob> challenge_signature) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (!challenge_signature) {
    LOG(ERROR) << "Unsealing signature challenge failed";
    Abort();
    // |this| can be already destroyed at this point.
    return;
  }
  SecureBlob unsealed_secret;
  if (!unsealing_session_->Unseal(*challenge_signature, &unsealed_secret)) {
    LOG(ERROR) << "Failed to unseal the secret";
    Abort();
    // |this| can be already destroyed at this point.
    return;
  }
  unsealed_secret_ = std::make_unique<SecureBlob>(unsealed_secret);
  ProceedIfChallengesDone();
}

void ChallengeCredentialsDecryptOperation::ProceedIfChallengesDone() {
  if (!salt_signature_ || !unsealed_secret_)
    return;
  auto username_passkey = std::make_unique<UsernamePasskey>(account_id_.c_str(),
                                                            ConstructPasskey());
  username_passkey->set_key_data(key_data_);
  Complete(&completion_callback_, std::move(username_passkey));
  // |this| can be already destroyed at this point.
}

SecureBlob ChallengeCredentialsDecryptOperation::ConstructPasskey() const {
  DCHECK(unsealed_secret_);
  DCHECK(salt_signature_);
  // Use a digest of the salt signature, to make the resulting passkey
  // reasonably short, and to avoid any potential bias.
  const SecureBlob salt_signature_hash =
      CryptoLib::Sha256ToSecureBlob(*salt_signature_);
  return SecureBlob::Combine(*unsealed_secret_, salt_signature_hash);
}

}  // namespace cryptohome
