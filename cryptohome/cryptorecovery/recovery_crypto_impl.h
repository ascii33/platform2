// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_CRYPTORECOVERY_RECOVERY_CRYPTO_IMPL_H_
#define CRYPTOHOME_CRYPTORECOVERY_RECOVERY_CRYPTO_IMPL_H_

#include <memory>

#include <brillo/secure_blob.h>
#include <libhwsec-foundation/crypto/elliptic_curve.h>

#include "cryptohome/cryptorecovery/cryptorecovery.pb.h"
#include "cryptohome/cryptorecovery/recovery_crypto.h"
#include "cryptohome/cryptorecovery/recovery_crypto_util.h"

namespace cryptohome {
namespace cryptorecovery {
// Cryptographic operations for cryptohome recovery performed on either CPU
// (software emulation) or TPM modules depending on the TPM backend.
class RecoveryCryptoImpl : public RecoveryCrypto {
 public:
  // Creates instance. Returns nullptr if error occurred.
  static std::unique_ptr<RecoveryCryptoImpl> Create(
      RecoveryCryptoTpmBackend* tpm_backend);

  RecoveryCryptoImpl(const RecoveryCryptoImpl&) = delete;
  RecoveryCryptoImpl& operator=(const RecoveryCryptoImpl&) = delete;

  ~RecoveryCryptoImpl() override;

  bool GenerateRecoveryRequest(
      const HsmPayload& hsm_payload,
      const RequestMetadata& request_meta_data,
      const CryptoRecoveryEpochResponse& epoch_response,
      const brillo::SecureBlob& encrypted_rsa_priv_key,
      const brillo::SecureBlob& encrypted_channel_priv_key,
      const brillo::SecureBlob& channel_pub_key,
      CryptoRecoveryRpcRequest* recovery_request,
      brillo::SecureBlob* ephemeral_pub_key) const override;
  bool GenerateHsmPayload(
      const brillo::SecureBlob& mediator_pub_key,
      const OnboardingMetadata& onboarding_metadata,
      HsmPayload* hsm_payload,
      brillo::SecureBlob* encrypted_rsa_priv_key,
      brillo::SecureBlob* encrypted_destination_share,
      brillo::SecureBlob* recovery_key,
      brillo::SecureBlob* channel_pub_key,
      brillo::SecureBlob* encrypted_channel_priv_key) const override;
  bool RecoverDestination(const brillo::SecureBlob& dealer_pub_key,
                          const brillo::SecureBlob& key_auth_value,
                          const brillo::SecureBlob& encrypted_destination_share,
                          const brillo::SecureBlob& ephemeral_pub_key,
                          const brillo::SecureBlob& mediated_publisher_pub_key,
                          brillo::SecureBlob* destination_dh) const override;
  bool DecryptResponsePayload(
      const brillo::SecureBlob& encrypted_channel_priv_key,
      const CryptoRecoveryEpochResponse& epoch_response,
      const CryptoRecoveryRpcResponse& recovery_response_proto,
      HsmResponsePlainText* response_plain_text) const override;

 private:
  RecoveryCryptoImpl(hwsec_foundation::EllipticCurve ec,
                     RecoveryCryptoTpmBackend* tpm_backend);
  bool GenerateRecoveryKey(const crypto::ScopedEC_POINT& recovery_pub_point,
                           const crypto::ScopedEC_KEY& dealer_key_pair,
                           brillo::SecureBlob* recovery_key) const;
  // Generate ephemeral public and inverse public keys {G*x, G*-x}
  bool GenerateEphemeralKey(brillo::SecureBlob* ephemeral_spki_der,
                            brillo::SecureBlob* ephemeral_inv_spki_der) const;
  bool GenerateHsmAssociatedData(const brillo::SecureBlob& channel_pub_key,
                                 const brillo::SecureBlob& rsa_pub_key,
                                 const crypto::ScopedEC_KEY& publisher_key_pair,
                                 const OnboardingMetadata& onboarding_metadata,
                                 brillo::SecureBlob* hsm_associated_data) const;

  hwsec_foundation::EllipticCurve ec_;
  RecoveryCryptoTpmBackend* const tpm_backend_;
};

}  // namespace cryptorecovery
}  // namespace cryptohome

#endif  // CRYPTOHOME_CRYPTORECOVERY_RECOVERY_CRYPTO_IMPL_H_
