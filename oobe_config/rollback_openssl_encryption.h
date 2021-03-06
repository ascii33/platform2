// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OOBE_CONFIG_ROLLBACK_OPENSSL_ENCRYPTION_H_
#define OOBE_CONFIG_ROLLBACK_OPENSSL_ENCRYPTION_H_

#include <optional>

#include <brillo/secure_blob.h>

namespace oobe_config {

struct EncryptedData {
  brillo::Blob data;
  brillo::SecureBlob key;
};

// Encrypts data with AES_256_GCM and a randomly generated key. Returns key and
// encrypted data on success and `std::nullopt` on failure.
std::optional<EncryptedData> Encrypt(const brillo::SecureBlob& plain_data);

// Decrypts data with AES_256_GCM with the given key. Returns `std::nullopt`
// on failure and the decrypted data on success.
std::optional<brillo::SecureBlob> Decrypt(const EncryptedData& encrypted_data);

}  // namespace oobe_config

#endif  // OOBE_CONFIG_ROLLBACK_OPENSSL_ENCRYPTION_H_
