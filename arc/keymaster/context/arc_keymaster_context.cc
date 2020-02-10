// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "arc/keymaster/context/arc_keymaster_context.h"

#include <algorithm>
#include <string>

#include <base/logging.h>
#include <base/stl_util.h>
#include <keymaster/android_keymaster_utils.h>
#include <keymaster/key_blob_utils/integrity_assured_key_blob.h>
#include <keymaster/key_blob_utils/software_keyblobs.h>

#include "arc/keymaster/context/chaps_client.h"
#include "arc/keymaster/context/openssl_utils.h"

namespace arc {
namespace keymaster {
namespace context {

namespace {

bool DeserializeKeyMaterialToBlob(const std::string& key_material,
                                  ::keymaster::KeymasterKeyBlob* output) {
  if (!output->Reset(key_material.size()))
    return false;

  uint8_t* end = std::copy(key_material.begin(), key_material.end(),
                           output->writable_data());
  CHECK_EQ(end - output->key_material, output->key_material_size);
  return true;
}

void SerializeAuthorizationSet(const ::keymaster::AuthorizationSet& auth_set,
                               std::string* output) {
  output->resize(auth_set.SerializedSize());
  uint8_t* buffer = reinterpret_cast<uint8_t*>(base::data(*output));
  auth_set.Serialize(buffer, buffer + output->size());
}

bool DeserializeAuthorizationSet(const std::string& serialized_auth_set,
                                 ::keymaster::AuthorizationSet* output) {
  std::string mutable_auth_set(serialized_auth_set);
  const uint8_t* buffer =
      reinterpret_cast<uint8_t*>(base::data(mutable_auth_set));
  return output->Deserialize(&buffer, buffer + serialized_auth_set.size());
}

brillo::Blob SerializeAuthorizationSetToBlob(
    const ::keymaster::AuthorizationSet& authorization_set) {
  brillo::Blob blob(authorization_set.SerializedSize());
  authorization_set.Serialize(blob.data(), blob.data() + blob.size());
  return blob;
}

KeyData PackToArcKeyData(const ::keymaster::KeymasterKeyBlob& key_material,
                         const ::keymaster::AuthorizationSet& hw_enforced,
                         const ::keymaster::AuthorizationSet& sw_enforced) {
  KeyData key_data;

  // Copy key material.
  key_data.mutable_arc_key()->set_key_material(key_material.key_material,
                                               key_material.key_material_size);

  // Serialize hardware enforced authorization set.
  SerializeAuthorizationSet(hw_enforced, key_data.mutable_hw_enforced_tags());

  // Serialize software enforced authorization set.
  SerializeAuthorizationSet(sw_enforced, key_data.mutable_sw_enforced_tags());

  return key_data;
}

bool UnpackFromArcKeyData(const KeyData& key_data,
                          ::keymaster::KeymasterKeyBlob* key_material,
                          ::keymaster::AuthorizationSet* hw_enforced,
                          ::keymaster::AuthorizationSet* sw_enforced) {
  // Currently only known key data source is ARC.
  if (key_data.data_case() != KeyData::DataCase::kArcKey)
    return false;

  // Deserialize key material.
  if (!DeserializeKeyMaterialToBlob(key_data.arc_key().key_material(),
                                    key_material)) {
    return false;
  }

  // Deserialize hardware enforced authorization set.
  if (!DeserializeAuthorizationSet(key_data.hw_enforced_tags(), hw_enforced))
    return false;

  // Deserialize software enforced authorization set.
  return DeserializeAuthorizationSet(key_data.sw_enforced_tags(), sw_enforced);
}

}  // anonymous namespace

ArcKeymasterContext::ArcKeymasterContext(const scoped_refptr<::dbus::Bus>& bus)
    : context_adaptor_(bus) {}

ArcKeymasterContext::~ArcKeymasterContext() = default;

keymaster_error_t ArcKeymasterContext::CreateKeyBlob(
    const ::keymaster::AuthorizationSet& key_description,
    const keymaster_key_origin_t origin,
    const ::keymaster::KeymasterKeyBlob& key_material,
    ::keymaster::KeymasterKeyBlob* key_blob,
    ::keymaster::AuthorizationSet* hw_enforced,
    ::keymaster::AuthorizationSet* sw_enforced) const {
  keymaster_error_t error =
      SetKeyBlobAuthorizations(key_description, origin, os_version_,
                               os_patchlevel_, hw_enforced, sw_enforced);
  if (error != KM_ERROR_OK)
    return error;

  ::keymaster::AuthorizationSet hidden;
  error = BuildHiddenAuthorizations(key_description, &hidden,
                                    ::keymaster::softwareRootOfTrust);
  if (error != KM_ERROR_OK)
    return error;

  return SerializeKeyDataBlob(key_material, hidden, *hw_enforced, *sw_enforced,
                              key_blob);
}

keymaster_error_t ArcKeymasterContext::ParseKeyBlob(
    const ::keymaster::KeymasterKeyBlob& key_blob,
    const ::keymaster::AuthorizationSet& additional_params,
    ::keymaster::UniquePtr<::keymaster::Key>* key) const {
  ::keymaster::AuthorizationSet hw_enforced;
  ::keymaster::AuthorizationSet sw_enforced;
  ::keymaster::KeymasterKeyBlob key_material;
  keymaster_error_t error;

  ::keymaster::AuthorizationSet hidden;
  error = BuildHiddenAuthorizations(additional_params, &hidden,
                                    ::keymaster::softwareRootOfTrust);
  if (error != KM_ERROR_OK)
    return error;

  error = DeserializeBlob(key_blob, hidden, &key_material, &hw_enforced,
                          &sw_enforced);
  if (error != KM_ERROR_OK)
    return error;

  keymaster_algorithm_t algorithm;
  if (!hw_enforced.GetTagValue(::keymaster::TAG_ALGORITHM, &algorithm) &&
      !sw_enforced.GetTagValue(::keymaster::TAG_ALGORITHM, &algorithm)) {
    return KM_ERROR_INVALID_ARGUMENT;
  }
  auto factory = GetKeyFactory(algorithm);
  return factory->LoadKey(move(key_material), additional_params,
                          move(hw_enforced), move(sw_enforced), key);
}

keymaster_error_t ArcKeymasterContext::DeserializeBlob(
    const ::keymaster::KeymasterKeyBlob& key_blob,
    const ::keymaster::AuthorizationSet& hidden,
    ::keymaster::KeymasterKeyBlob* key_material,
    ::keymaster::AuthorizationSet* hw_enforced,
    ::keymaster::AuthorizationSet* sw_enforced) const {
  keymaster_error_t error;

  error = DeserializeKeyDataBlob(key_blob, hidden, key_material, hw_enforced,
                                 sw_enforced);
  if (error == KM_ERROR_OK)
    return error;

  // Still need to parse insecure blobs when upgrading to the encrypted format.
  // TODO(b/151146402) drop support for insecure blobs.
  return DeserializeIntegrityAssuredBlob(key_blob, hidden, key_material,
                                         hw_enforced, sw_enforced);
}

keymaster_error_t ArcKeymasterContext::SerializeKeyDataBlob(
    const ::keymaster::KeymasterKeyBlob& key_material,
    const ::keymaster::AuthorizationSet& hidden,
    const ::keymaster::AuthorizationSet& hw_enforced,
    const ::keymaster::AuthorizationSet& sw_enforced,
    ::keymaster::KeymasterKeyBlob* key_blob) const {
  if (!key_blob)
    return KM_ERROR_OUTPUT_PARAMETER_NULL;

  KeyData key_data = PackToArcKeyData(key_material, hw_enforced, sw_enforced);

  // Serialize key data into the output |key_blob|.
  if (!SerializeKeyData(key_data, hidden, key_blob)) {
    LOG(ERROR) << "Failed to serialize KeyData.";
    return KM_ERROR_UNKNOWN_ERROR;
  }

  return KM_ERROR_OK;
}

keymaster_error_t ArcKeymasterContext::DeserializeKeyDataBlob(
    const ::keymaster::KeymasterKeyBlob& key_blob,
    const ::keymaster::AuthorizationSet& hidden,
    ::keymaster::KeymasterKeyBlob* key_material,
    ::keymaster::AuthorizationSet* hw_enforced,
    ::keymaster::AuthorizationSet* sw_enforced) const {
  if (!key_material || !hw_enforced || !sw_enforced)
    return KM_ERROR_OUTPUT_PARAMETER_NULL;

  // Deserialize a KeyData object from the given |key_blob|.
  base::Optional<KeyData> key_data = DeserializeKeyData(key_blob, hidden);
  if (!key_data.has_value() || key_data->data_case() == KeyData::DATA_NOT_SET) {
    LOG(ERROR) << "Failed to parse a KeyData from key blob.";
    return KM_ERROR_INVALID_KEY_BLOB;
  }

  // Unpack Keymaster structures from KeyData.
  if (!UnpackFromArcKeyData(key_data.value(), key_material, hw_enforced,
                            sw_enforced)) {
    LOG(ERROR) << "Failed to unpack key blob.";
    return KM_ERROR_INVALID_KEY_BLOB;
  }

  return KM_ERROR_OK;
}

bool ArcKeymasterContext::SerializeKeyData(
    const KeyData& key_data,
    const ::keymaster::AuthorizationSet& hidden,
    ::keymaster::KeymasterKeyBlob* key_blob) const {
  // Fetch key.
  ChapsClient chaps(context_adaptor_.GetWeakPtr());
  base::Optional<brillo::SecureBlob> encryption_key =
      chaps.ExportOrGenerateEncryptionKey();
  if (!encryption_key.has_value())
    return false;

  // Initialize a KeyData blob. Allocated blobs should offer the same guarantees
  // as brillo::SecureBlob (b/151103358).
  brillo::SecureBlob data(key_data.ByteSizeLong());
  key_data.SerializeWithCachedSizesToArray(data.data());

  // Encrypt the KeyData blob. As of Android R KeyStore's client ID and data
  // used in |auth_data| is empty. We still bind to it to comply with VTS tests.
  brillo::Blob auth_data = SerializeAuthorizationSetToBlob(hidden);
  base::Optional<brillo::Blob> encrypted =
      Aes256GcmEncrypt(encryption_key.value(), auth_data, data);
  if (!encrypted.has_value())
    return false;

  // Copy |encrypted| to output |key_blob|.
  if (!key_blob->Reset(encrypted->size()))
    return false;
  std::copy(encrypted->begin(), encrypted->end(), key_blob->writable_data());
  return true;
}

base::Optional<KeyData> ArcKeymasterContext::DeserializeKeyData(
    const ::keymaster::KeymasterKeyBlob& key_blob,
    const ::keymaster::AuthorizationSet& hidden) const {
  // Fetch key.
  ChapsClient chaps(context_adaptor_.GetWeakPtr());
  base::Optional<brillo::SecureBlob> encryption_key =
      chaps.ExportOrGenerateEncryptionKey();
  if (!encryption_key.has_value())
    return base::nullopt;

  // Decrypt the KeyData blob.
  brillo::Blob encrypted(key_blob.begin(), key_blob.end());
  brillo::Blob auth_data = SerializeAuthorizationSetToBlob(hidden);
  base::Optional<brillo::SecureBlob> unencrypted =
      Aes256GcmDecrypt(encryption_key.value(), auth_data, encrypted);
  if (!unencrypted.has_value())
    return base::nullopt;

  // Parse the |unencrypted| blob into a KeyData object and return it.
  KeyData key_data;
  if (!key_data.ParseFromArray(unencrypted->data(), unencrypted->size()))
    return base::nullopt;

  return key_data;
}

namespace internal {

brillo::Blob TestSerializeAuthorizationSetToBlob(
    const ::keymaster::AuthorizationSet& authorization_set) {
  return SerializeAuthorizationSetToBlob(authorization_set);
}

}  // namespace internal

}  // namespace context
}  // namespace keymaster
}  // namespace arc
