// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_blocks/auth_block_utility_impl.h"

#include <stdint.h>

#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <base/check.h>
#include <base/logging.h>
#include <brillo/cryptohome.h>
#include <chromeos/constants/cryptohome.h>
#include <libhwsec-foundation/status/status_chain_or.h>

#include "cryptohome/auth_blocks/async_challenge_credential_auth_block.h"
#include "cryptohome/auth_blocks/auth_block.h"
#include "cryptohome/auth_blocks/auth_block_state.h"
#include "cryptohome/auth_blocks/auth_block_type.h"
#include "cryptohome/auth_blocks/auth_block_utils.h"
#include "cryptohome/auth_blocks/challenge_credential_auth_block.h"
#include "cryptohome/auth_blocks/double_wrapped_compat_auth_block.h"
#include "cryptohome/auth_blocks/libscrypt_compat_auth_block.h"
#include "cryptohome/auth_blocks/pin_weaver_auth_block.h"
#include "cryptohome/auth_blocks/sync_to_async_auth_block_adapter.h"
#include "cryptohome/auth_blocks/tpm_bound_to_pcr_auth_block.h"
#include "cryptohome/auth_blocks/tpm_ecc_auth_block.h"
#include "cryptohome/auth_blocks/tpm_not_bound_to_pcr_auth_block.h"
#include "cryptohome/auth_factor/auth_factor_type.h"
#include "cryptohome/credentials.h"
#include "cryptohome/crypto.h"
#include "cryptohome/crypto_error.h"
#include "cryptohome/cryptohome_metrics.h"
#include "cryptohome/error/location_utils.h"
#include "cryptohome/error/utilities.h"
#include "cryptohome/key_objects.h"
#include "cryptohome/keyset_management.h"
#include "cryptohome/tpm.h"
#include "cryptohome/vault_keyset.h"

using cryptohome::error::ContainsActionInStack;
using cryptohome::error::CryptohomeCryptoError;
using cryptohome::error::ErrorAction;
using cryptohome::error::ErrorActionSet;
using hwsec_foundation::status::MakeStatus;
using hwsec_foundation::status::OkStatus;
using hwsec_foundation::status::StatusChain;

namespace cryptohome {

AuthBlockUtilityImpl::AuthBlockUtilityImpl(KeysetManagement* keyset_management,
                                           Crypto* crypto,
                                           Platform* platform)
    : keyset_management_(keyset_management),
      crypto_(crypto),
      platform_(platform),
      challenge_credentials_helper_(nullptr),
      key_challenge_service_(nullptr) {
  DCHECK(keyset_management);
  DCHECK(crypto_);
  DCHECK(platform_);
}

AuthBlockUtilityImpl::AuthBlockUtilityImpl(
    KeysetManagement* keyset_management,
    Crypto* crypto,
    Platform* platform,
    ChallengeCredentialsHelper* credentials_helper,
    std::unique_ptr<KeyChallengeService> key_challenge_service,
    const std::string& account_id)
    : keyset_management_(keyset_management),
      crypto_(crypto),
      platform_(platform),
      challenge_credentials_helper_(credentials_helper),
      key_challenge_service_(std::move(key_challenge_service)),
      account_id_(account_id) {
  DCHECK(keyset_management);
  DCHECK(crypto_);
  DCHECK(platform_);
  DCHECK(challenge_credentials_helper_);
  DCHECK(key_challenge_service_);
}
AuthBlockUtilityImpl::~AuthBlockUtilityImpl() = default;

bool AuthBlockUtilityImpl::GetLockedToSingleUser() const {
  return platform_->FileExists(base::FilePath(kLockedToSingleUserFile));
}

CryptoStatus AuthBlockUtilityImpl::CreateKeyBlobsWithAuthBlock(
    AuthBlockType auth_block_type,
    const Credentials& credentials,
    const std::optional<brillo::SecureBlob>& reset_secret,
    AuthBlockState& out_state,
    KeyBlobs& out_key_blobs) const {
  CryptoStatusOr<std::unique_ptr<SyncAuthBlock>> auth_block =
      GetAuthBlockWithType(auth_block_type);
  if (!auth_block.ok()) {
    LOG(ERROR) << "Failed to retrieve auth block.";
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(kLocAuthBlockUtilNoAuthBlockInCreateKeyBlobs))
        .Wrap(std::move(auth_block).status());
  }
  ReportCreateAuthBlock(auth_block_type);

  // |reset_secret| is not processed in the AuthBlocks, the value is copied to
  // the |out_key_blobs| directly. |reset_secret| will be added to the
  // |out_key_blobs| in the VaultKeyset, if missing.
  AuthInput user_input = {
      credentials.passkey(),
      /*locked_to_single_user*=*/std::nullopt,
      brillo::cryptohome::home::SanitizeUserName(credentials.username()),
      reset_secret};

  CryptoStatus error =
      auth_block.value()->Create(user_input, &out_state, &out_key_blobs);
  if (!error.ok()) {
    LOG(ERROR) << "Failed to create per credential secret: " << error;
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(
                   kLocAuthBlockUtilCreateFailedInCreateKeyBlobs))
        .Wrap(std::move(error));
  }

  ReportWrappingKeyDerivationType(auth_block.value()->derivation_type(),
                                  CryptohomePhase::kCreated);

  return OkStatus<CryptohomeCryptoError>();
}

bool AuthBlockUtilityImpl::CreateKeyBlobsWithAuthBlockAsync(
    AuthBlockType auth_block_type,
    const AuthInput& auth_input,
    AuthBlock::CreateCallback create_callback) {
  CryptoStatusOr<std::unique_ptr<AuthBlock>> auth_block =
      GetAsyncAuthBlockWithType(auth_block_type);
  if (!auth_block.ok()) {
    LOG(ERROR) << "Failed to retrieve auth block.";
    std::move(create_callback)
        .Run(MakeStatus<CryptohomeCryptoError>(
                 CRYPTOHOME_ERR_LOC(
                     kLocAuthBlockUtilNoAuthBlockInCreateKeyBlobsAsync))
                 .Wrap(std::move(auth_block).status()),
             nullptr, nullptr);
    return false;
  }
  ReportCreateAuthBlock(auth_block_type);

  auth_block.value()->Create(auth_input, std::move(create_callback));

  // TODO(b/225001347): Move this report to the caller. Here this is always
  // reported independent of the error status.
  ReportWrappingKeyDerivationType(auth_block.value()->derivation_type(),
                                  CryptohomePhase::kCreated);
  return true;
}

CryptoStatus AuthBlockUtilityImpl::DeriveKeyBlobsWithAuthBlock(
    AuthBlockType auth_block_type,
    const Credentials& credentials,
    const AuthBlockState& auth_state,
    KeyBlobs& out_key_blobs) const {
  DCHECK_NE(auth_block_type, AuthBlockType::kMaxValue);

  AuthInput auth_input = {credentials.passkey(),
                          /*locked_to_single_user=*/std::nullopt};

  auth_input.locked_to_single_user = GetLockedToSingleUser();

  CryptoStatusOr<std::unique_ptr<SyncAuthBlock>> auth_block =
      GetAuthBlockWithType(auth_block_type);
  if (!auth_block.ok()) {
    LOG(ERROR) << "Keyset wrapped with unknown method.";
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(kLocAuthBlockUtilNoAuthBlockInDeriveKeyBlobs))
        .Wrap(std::move(auth_block).status());
  }
  ReportDeriveAuthBlock(auth_block_type);

  CryptoStatus error =
      auth_block.value()->Derive(auth_input, auth_state, &out_key_blobs);
  if (error.ok()) {
    ReportWrappingKeyDerivationType(auth_block.value()->derivation_type(),
                                    CryptohomePhase::kMounted);
    return OkStatus<CryptohomeCryptoError>();
  }
  LOG(ERROR) << "Failed to derive per credential secret: " << error;

  // For LE credentials, if deriving the key blobs failed due to too many
  // attempts, set auth_locked=true in the corresponding keyset. Then save it
  // for future callers who can Load it w/o Decrypt'ing to check that flag.
  // When the pin is entered wrong and AuthBlock fails to derive the KeyBlobs
  // it doesn't make it into the VaultKeyset::Decrypt(); so auth_lock should
  // be set here.
  if (auth_block_type == AuthBlockType::kPinWeaver &&
      ContainsActionInStack(error, ErrorAction::kTpmLockout)) {
    // Get the corresponding encrypted vault keyset for the user and the label
    // to set the auth_locked.
    std::unique_ptr<VaultKeyset> vk = keyset_management_->GetVaultKeyset(
        brillo::cryptohome::home::SanitizeUserName(credentials.username()),
        credentials.key_data().label());

    if (vk == nullptr) {
      LOG(ERROR)
          << "No vault keyset is found on disk for the given label. Cannot "
             "decide on the AuthBlock type without vault keyset metadata.";
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(kLocAuthBlockUtilNoVaultKeysetInDeriveKeyBlobs),
          ErrorActionSet({ErrorAction::kAuth, ErrorAction::kReboot}),
          CryptoError::CE_OTHER_CRYPTO);
    }
    vk->SetAuthLocked(true);
    vk->Save(vk->GetSourceFile());
  }
  return MakeStatus<CryptohomeCryptoError>(
             CRYPTOHOME_ERR_LOC(kLocAuthBlockUtilDeriveFailedInDeriveKeyBlobs))
      .Wrap(std::move(error));
}

bool AuthBlockUtilityImpl::DeriveKeyBlobsWithAuthBlockAsync(
    AuthBlockType auth_block_type,
    const AuthInput& auth_input,
    const AuthBlockState& auth_state,
    AuthBlock::DeriveCallback derive_callback) {
  DCHECK_NE(auth_block_type, AuthBlockType::kMaxValue);

  CryptoStatusOr<std::unique_ptr<AuthBlock>> auth_block =
      GetAsyncAuthBlockWithType(auth_block_type);
  if (!auth_block.ok()) {
    LOG(ERROR) << "Failed to retrieve auth block.";
    std::move(derive_callback)
        .Run(MakeStatus<CryptohomeCryptoError>(
                 CRYPTOHOME_ERR_LOC(
                     kLocAuthBlockUtilNoAuthBlockInDeriveKeyBlobsAsync))
                 .Wrap(std::move(auth_block).status()),
             nullptr);
    return false;
  }
  ReportCreateAuthBlock(auth_block_type);

  auth_block.value()->Derive(auth_input, auth_state,
                             std::move(derive_callback));

  return true;
}

AuthBlockType AuthBlockUtilityImpl::GetAuthBlockTypeForCreation(
    const bool is_le_credential, const bool is_challenge_credential) const {
  if (is_le_credential) {
    return AuthBlockType::kPinWeaver;
  }

  if (is_challenge_credential) {
    return AuthBlockType::kChallengeCredential;
  }

  bool use_tpm = crypto_->tpm() && crypto_->tpm()->IsOwned();
  bool with_user_auth = crypto_->CanUnsealWithUserAuth();
  bool has_ecc_key = crypto_->cryptohome_keys_manager() &&
                     crypto_->cryptohome_keys_manager()->HasCryptohomeKey(
                         CryptohomeKeyType::kECC);

  if (use_tpm && with_user_auth && has_ecc_key) {
    return AuthBlockType::kTpmEcc;
  }

  if (use_tpm && with_user_auth && !has_ecc_key) {
    return AuthBlockType::kTpmBoundToPcr;
  }

  if (use_tpm && !with_user_auth) {
    return AuthBlockType::kTpmNotBoundToPcr;
  }

  return AuthBlockType::kLibScryptCompat;
}

AuthBlockType AuthBlockUtilityImpl::GetAuthBlockTypeForDerivation(
    const std::string& label, const std::string& obfuscated_username) const {
  std::unique_ptr<VaultKeyset> vk =
      keyset_management_->GetVaultKeyset(obfuscated_username, label);
  // If there is no keyset on the disk for the given user and label (or for the
  // empty label as a wildcard), key derivation type cannot be obtained.
  if (vk == nullptr) {
    LOG(ERROR)
        << "No vault keyset is found on disk for the given label. Cannot "
           "decide on the AuthBlock type without vault keyset metadata.";
    return AuthBlockType::kMaxValue;
  }

  int32_t vk_flags = vk->GetFlags();
  AuthBlockType auth_block_type = AuthBlockType::kMaxValue;
  if (!FlagsToAuthBlockType(vk_flags, auth_block_type)) {
    LOG(WARNING) << "Failed to get the AuthBlock type for key derivation";
  }
  return auth_block_type;
}

CryptoStatusOr<std::unique_ptr<SyncAuthBlock>>
AuthBlockUtilityImpl::GetAuthBlockWithType(
    const AuthBlockType& auth_block_type) const {
  switch (auth_block_type) {
    case AuthBlockType::kPinWeaver:
      return std::make_unique<PinWeaverAuthBlock>(
          crypto_->le_manager(), crypto_->cryptohome_keys_manager());

    case AuthBlockType::kChallengeCredential:
      return std::make_unique<ChallengeCredentialAuthBlock>();

    case AuthBlockType::kDoubleWrappedCompat:
      return std::make_unique<DoubleWrappedCompatAuthBlock>(
          crypto_->tpm(), crypto_->cryptohome_keys_manager());

    case AuthBlockType::kTpmEcc:
      return std::make_unique<TpmEccAuthBlock>(
          crypto_->tpm(), crypto_->cryptohome_keys_manager());

    case AuthBlockType::kTpmBoundToPcr:
      return std::make_unique<TpmBoundToPcrAuthBlock>(
          crypto_->tpm(), crypto_->cryptohome_keys_manager());

    case AuthBlockType::kTpmNotBoundToPcr:
      return std::make_unique<TpmNotBoundToPcrAuthBlock>(
          crypto_->tpm(), crypto_->cryptohome_keys_manager());

    case AuthBlockType::kLibScryptCompat:
      return std::make_unique<LibScryptCompatAuthBlock>();

    case AuthBlockType::kCryptohomeRecovery:
      LOG(ERROR)
          << "CryptohomeRecovery is not a supported AuthBlockType for now.";
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(
              kLocAuthBlockUtilCHRecoveryUnsupportedInGetAuthBlockWithType),
          ErrorActionSet(
              {ErrorAction::kDevCheckUnexpectedState, ErrorAction::kAuth}),
          CryptoError::CE_OTHER_CRYPTO);

    case AuthBlockType::kMaxValue:
      LOG(ERROR) << "Unsupported AuthBlockType.";

      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(
              kLocAuthBlockUtilMaxValueUnsupportedInGetAuthBlockWithType),
          ErrorActionSet(
              {ErrorAction::kDevCheckUnexpectedState, ErrorAction::kAuth}),
          CryptoError::CE_OTHER_CRYPTO);
  }
  return MakeStatus<CryptohomeCryptoError>(
      CRYPTOHOME_ERR_LOC(
          kLocAuthBlockUtilUnknownUnsupportedInGetAuthBlockWithType),
      ErrorActionSet(
          {ErrorAction::kDevCheckUnexpectedState, ErrorAction::kAuth}),
      CryptoError::CE_OTHER_CRYPTO);
}

CryptoStatusOr<std::unique_ptr<AuthBlock>>
AuthBlockUtilityImpl::GetAsyncAuthBlockWithType(
    const AuthBlockType& auth_block_type) {
  switch (auth_block_type) {
    case AuthBlockType::kPinWeaver:
      return std::make_unique<SyncToAsyncAuthBlockAdapter>(
          std::make_unique<PinWeaverAuthBlock>(
              crypto_->le_manager(), crypto_->cryptohome_keys_manager()));

    case AuthBlockType::kChallengeCredential:
      if (key_challenge_service_ && challenge_credentials_helper_ &&
          account_id_.has_value()) {
        return std::make_unique<AsyncChallengeCredentialAuthBlock>(
            crypto_->tpm(), challenge_credentials_helper_,
            std::move(key_challenge_service_), account_id_.value());
      }
      LOG(ERROR) << "No valid ChallengeCredentialsHelper, "
                    "KeyChallengeService, or account id in AuthBlockUtility";
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(
              kLocAuthBlockUtilNoChalInGetAsyncAuthBlockWithType),
          ErrorActionSet(
              {ErrorAction::kDevCheckUnexpectedState, ErrorAction::kAuth}),
          CryptoError::CE_OTHER_CRYPTO);

    case AuthBlockType::kDoubleWrappedCompat:
      return std::make_unique<SyncToAsyncAuthBlockAdapter>(
          std::make_unique<DoubleWrappedCompatAuthBlock>(
              crypto_->tpm(), crypto_->cryptohome_keys_manager()));

    case AuthBlockType::kTpmEcc:
      return std::make_unique<SyncToAsyncAuthBlockAdapter>(
          std::make_unique<TpmEccAuthBlock>(
              crypto_->tpm(), crypto_->cryptohome_keys_manager()));

    case AuthBlockType::kTpmBoundToPcr:
      return std::make_unique<SyncToAsyncAuthBlockAdapter>(
          std::make_unique<TpmBoundToPcrAuthBlock>(
              crypto_->tpm(), crypto_->cryptohome_keys_manager()));

    case AuthBlockType::kTpmNotBoundToPcr:
      return std::make_unique<SyncToAsyncAuthBlockAdapter>(
          std::make_unique<TpmNotBoundToPcrAuthBlock>(
              crypto_->tpm(), crypto_->cryptohome_keys_manager()));

    case AuthBlockType::kLibScryptCompat:
      return std::make_unique<SyncToAsyncAuthBlockAdapter>(
          std::make_unique<LibScryptCompatAuthBlock>());

    case AuthBlockType::kCryptohomeRecovery:
      LOG(ERROR)
          << "CryptohomeRecovery is not a supported AuthBlockType for now.";
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(
              kLocAuthBlockUtilCHUnsupportedInGetAsyncAuthBlockWithType),
          ErrorActionSet(
              {ErrorAction::kDevCheckUnexpectedState, ErrorAction::kAuth}),
          CryptoError::CE_OTHER_CRYPTO);

    case AuthBlockType::kMaxValue:
      LOG(ERROR) << "Unsupported AuthBlockType.";
      return MakeStatus<CryptohomeCryptoError>(
          CRYPTOHOME_ERR_LOC(
              kLocAuthBlockUtilMaxValueUnsupportedInGetAsyncAuthBlockWithType),
          ErrorActionSet(
              {ErrorAction::kDevCheckUnexpectedState, ErrorAction::kAuth}),
          CryptoError::CE_OTHER_CRYPTO);
  }
  return MakeStatus<CryptohomeCryptoError>(
      CRYPTOHOME_ERR_LOC(
          kLocAuthBlockUtilUnknownUnsupportedInGetAsyncAuthBlockWithType),
      ErrorActionSet(
          {ErrorAction::kDevCheckUnexpectedState, ErrorAction::kAuth}),
      CryptoError::CE_OTHER_CRYPTO);
}

bool AuthBlockUtilityImpl::GetAuthBlockStateFromVaultKeyset(
    const std::string& label,
    const std::string& obfuscated_username,
    AuthBlockState& out_state) const {
  std::unique_ptr<VaultKeyset> vault_keyset =
      keyset_management_->GetVaultKeyset(obfuscated_username, label);
  // If there is no keyset on the disk for the given user and label (or for the
  // empty label as a wildcard), AuthBlock state cannot be obtained.
  if (vault_keyset == nullptr) {
    LOG(ERROR)
        << "No vault keyset is found on disk for the given label. Cannot "
           "obtain AuthBlockState without vault keyset metadata.";
    return false;
  }

  return GetAuthBlockState(*vault_keyset, out_state);
}

void AuthBlockUtilityImpl::AssignAuthBlockStateToVaultKeyset(
    const AuthBlockState& auth_state, VaultKeyset& vault_keyset) const {
  if (const auto* state =
          std::get_if<TpmNotBoundToPcrAuthBlockState>(&auth_state.state)) {
    vault_keyset.SetTpmNotBoundToPcrState(*state);
  } else if (const auto* state =
                 std::get_if<TpmBoundToPcrAuthBlockState>(&auth_state.state)) {
    vault_keyset.SetTpmBoundToPcrState(*state);
  } else if (const auto* state =
                 std::get_if<PinWeaverAuthBlockState>(&auth_state.state)) {
    vault_keyset.SetPinWeaverState(*state);
  } else if (const auto* state = std::get_if<LibScryptCompatAuthBlockState>(
                 &auth_state.state)) {
    vault_keyset.SetLibScryptCompatState(*state);
  } else if (const auto* state = std::get_if<ChallengeCredentialAuthBlockState>(
                 &auth_state.state)) {
    vault_keyset.SetChallengeCredentialState(*state);
  } else if (const auto* state =
                 std::get_if<TpmEccAuthBlockState>(&auth_state.state)) {
    vault_keyset.SetTpmEccState(*state);
  } else {
    LOG(ERROR) << "Invalid auth block state type";
    return;
  }
}

CryptoStatus AuthBlockUtilityImpl::CreateKeyBlobsWithAuthFactorType(
    AuthFactorType auth_factor_type,
    const AuthInput& auth_input,
    AuthBlockState& out_auth_block_state,
    KeyBlobs& out_key_blobs) const {
  bool is_le_credential = auth_factor_type == AuthFactorType::kPin;
  AuthBlockType auth_block_type =
      GetAuthBlockTypeForCreation(is_le_credential,
                                  /*is_challenge_credential =*/false);
  if (auth_block_type == AuthBlockType::kChallengeCredential) {
    LOG(ERROR) << "Unsupported auth factor type";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(
            kLocAuthBlockUtilChalCredUnsupportedInCreateKeyBlobsAuthFactor),
        ErrorActionSet({ErrorAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }
  // TODO(b/216804305): Stop hardcoding the auth block.
  CryptoStatusOr<std::unique_ptr<SyncAuthBlock>> auth_block =
      GetAuthBlockWithType(auth_block_type);
  return auth_block.value()->Create(auth_input, &out_auth_block_state,
                                    &out_key_blobs);
}

AuthBlockType AuthBlockUtilityImpl::GetAuthBlockTypeForDerive(
    const AuthBlockState& auth_block_state) const {
  AuthBlockType auth_block_type = AuthBlockType::kMaxValue;
  if (const auto* state = std::get_if<TpmNotBoundToPcrAuthBlockState>(
          &auth_block_state.state)) {
    auth_block_type = AuthBlockType::kTpmNotBoundToPcr;
  } else if (const auto* state = std::get_if<TpmBoundToPcrAuthBlockState>(
                 &auth_block_state.state)) {
    auth_block_type = AuthBlockType::kTpmBoundToPcr;
  } else if (const auto* state = std::get_if<PinWeaverAuthBlockState>(
                 &auth_block_state.state)) {
    auth_block_type = AuthBlockType::kPinWeaver;
  } else if (const auto* state = std::get_if<LibScryptCompatAuthBlockState>(
                 &auth_block_state.state)) {
    auth_block_type = AuthBlockType::kLibScryptCompat;
  } else if (const auto* state =
                 std::get_if<TpmEccAuthBlockState>(&auth_block_state.state)) {
    auth_block_type = AuthBlockType::kTpmEcc;
  } else if (const auto& state = std::get_if<ChallengeCredentialAuthBlockState>(
                 &auth_block_state.state)) {
    auth_block_type = AuthBlockType::kChallengeCredential;
  }

  return auth_block_type;
}

CryptoStatus AuthBlockUtilityImpl::DeriveKeyBlobs(
    const AuthInput& auth_input,
    const AuthBlockState& auth_block_state,
    KeyBlobs& out_key_blobs) const {
  AuthBlockType auth_block_type = GetAuthBlockTypeForDerive(auth_block_state);
  if (auth_block_type == AuthBlockType::kMaxValue ||
      auth_block_type == AuthBlockType::kChallengeCredential) {
    LOG(ERROR) << "Unsupported auth factor type";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocAuthBlockUtilUnsupportedInDeriveKeyBlobs),
        ErrorActionSet({ErrorAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }
  CryptoStatusOr<std::unique_ptr<SyncAuthBlock>> auth_block =
      GetAuthBlockWithType(auth_block_type);
  return auth_block.value()->Derive(auth_input, auth_block_state,
                                    &out_key_blobs);
}

}  // namespace cryptohome
