// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/crypto/recovery_crypto_hsm_cbor_serialization.h"

#include <string>
#include <utility>
#include <vector>

#include <base/logging.h>
#include <base/optional.h>
#include <brillo/secure_blob.h>
#include <chromeos/cbor/reader.h>
#include <chromeos/cbor/values.h>
#include <chromeos/cbor/writer.h>

namespace cryptohome {

namespace {

bool SerializeHsmPayloadToCborHelper(const cbor::Value::MapValue& cbor_map,
                                     brillo::SecureBlob* payload_cbor) {
  base::Optional<std::vector<uint8_t>> serialized =
      cbor::Writer::Write(cbor::Value(std::move(cbor_map)));
  if (!serialized) {
    LOG(ERROR) << "Failed to serialize HSM plain Text to CBOR.";
    return false;
  }
  payload_cbor->assign(serialized.value().begin(), serialized.value().end());
  return true;
}

base::Optional<cbor::Value> ReadHsmCborPayload(
    const brillo::SecureBlob& payload_cbor) {
  cbor::Reader::DecoderError error_code;
  base::Optional<cbor::Value> cbor_response =
      cbor::Reader::Read(payload_cbor, &error_code);
  if (!cbor_response) {
    LOG(ERROR) << "Unable to create HSM cbor reader.";
    return base::nullopt;
  }
  if (error_code != cbor::Reader::DecoderError::CBOR_NO_ERROR) {
    LOG(ERROR) << "Error when parsing HSM cbor payload: "
               << cbor::Reader::ErrorCodeToString(error_code);
    return base::nullopt;
  }
  if (!cbor_response->is_map()) {
    LOG(ERROR) << "HSM cbor input is not a map.";
    return base::nullopt;
  }
  return cbor_response;
}

}  // namespace

// !!! DO NOT MODIFY !!!
// All the consts below are used as keys in the CBOR blog exchanged with the
// server and must be synced with the server/HSM implementation (or the other
// party will not be able to decrypt the data).
const char kRecoveryCryptoHsmSchemaVersion[] = "schema_version";
const char kMediatorShare[] = "mediator_share";
const char kMediatedPoint[] = "mediated_point";
const char kKeyAuthValue[] = "key_auth_value";
const char kDealerPublicKey[] = "dealer_pub_key";
const char kPublisherPublicKey[] = "publisher_pub_key";
const char kChannelPublicKey[] = "channel_pub_key";
const char kRsaPublicKey[] = "epoch_rsa_sig_pkey";
const char kOnboardingMetaData[] = "onboarding_meta_data";

const int kProtocolVersion = 1;

bool SerializeHsmAssociatedDataToCbor(
    const brillo::SecureBlob& publisher_pub_key,
    const brillo::SecureBlob& channel_pub_key,
    const brillo::SecureBlob& rsa_public_key,
    const brillo::SecureBlob& onboarding_meta_data,
    brillo::SecureBlob* ad_cbor) {
  cbor::Value::MapValue ad_map;

  ad_map.emplace(kRecoveryCryptoHsmSchemaVersion,
                 /*schema_version=*/kProtocolVersion);
  ad_map.emplace(kPublisherPublicKey, publisher_pub_key);
  ad_map.emplace(kChannelPublicKey, channel_pub_key);
  ad_map.emplace(kRsaPublicKey, rsa_public_key);
  ad_map.emplace(kOnboardingMetaData, onboarding_meta_data);

  base::Optional<std::vector<uint8_t>> serialized =
      cbor::Writer::Write(cbor::Value(std::move(ad_map)));
  if (!serialized) {
    LOG(ERROR) << "Failed to serialize HSM Associated Data to CBOR";
    return false;
  }
  ad_cbor->assign(serialized.value().begin(), serialized.value().end());
  return true;
}

bool SerializeHsmPlainTextToCbor(const brillo::SecureBlob& mediator_share,
                                 const brillo::SecureBlob& dealer_pub_key,
                                 const brillo::SecureBlob& key_auth_value,
                                 brillo::SecureBlob* plain_text_cbor) {
  cbor::Value::MapValue pt_map;

  pt_map.emplace(kDealerPublicKey, dealer_pub_key);
  pt_map.emplace(kMediatorShare, mediator_share);
  pt_map.emplace(kKeyAuthValue, key_auth_value);
  return SerializeHsmPayloadToCborHelper(pt_map, plain_text_cbor);
}

bool SerializeHsmResponsePayloadToCbor(const brillo::SecureBlob& mediated_point,
                                       const brillo::SecureBlob& dealer_pub_key,
                                       const brillo::SecureBlob& key_auth_value,
                                       brillo::SecureBlob* response_cbor) {
  cbor::Value::MapValue pt_map;

  pt_map.emplace(kDealerPublicKey, dealer_pub_key);
  pt_map.emplace(kMediatedPoint, mediated_point);
  pt_map.emplace(kKeyAuthValue, key_auth_value);
  return SerializeHsmPayloadToCborHelper(pt_map, response_cbor);
}

bool DeserializeHsmPlainTextFromCbor(
    const brillo::SecureBlob& hsm_plain_text_cbor,
    brillo::SecureBlob* mediator_share,
    brillo::SecureBlob* dealer_pub_key,
    brillo::SecureBlob* key_auth_value) {
  const auto& cbor = ReadHsmCborPayload(hsm_plain_text_cbor);
  if (!cbor) {
    return false;
  }

  const cbor::Value::MapValue& response_map = cbor->GetMap();
  const auto dealer_pub_key_entry =
      response_map.find(cbor::Value(kDealerPublicKey));
  if (dealer_pub_key_entry == response_map.end()) {
    LOG(ERROR) << "No dealer public key in the HSM response map.";
    return false;
  }
  if (!dealer_pub_key_entry->second.is_bytestring()) {
    LOG(ERROR) << "Wrongly formatted dealer key in the HSM response map.";
    return false;
  }

  const auto mediator_share_entry =
      response_map.find(cbor::Value(kMediatorShare));
  if (mediator_share_entry == response_map.end()) {
    LOG(ERROR) << "No share entry in the HSM response map.";
    return false;
  }
  if (!mediator_share_entry->second.is_bytestring()) {
    LOG(ERROR) << "Wrongly formatted share entry in the HSM response map.";
    return false;
  }

  const auto key_auth_value_entry =
      response_map.find(cbor::Value(kKeyAuthValue));
  if (key_auth_value_entry == response_map.end()) {
    LOG(ERROR) << "No key auth value in the HSM response map.";
    return false;
  }
  if (!key_auth_value_entry->second.is_bytestring()) {
    LOG(ERROR) << "Wrongly formatted key auth value in the HSM response map.";
    return false;
  }

  dealer_pub_key->assign(dealer_pub_key_entry->second.GetBytestring().begin(),
                         dealer_pub_key_entry->second.GetBytestring().end());
  mediator_share->assign(mediator_share_entry->second.GetBytestring().begin(),
                         mediator_share_entry->second.GetBytestring().end());
  key_auth_value->assign(key_auth_value_entry->second.GetBytestring().begin(),
                         key_auth_value_entry->second.GetBytestring().end());
  return true;
}

bool DeserializeHsmResponsePayloadFromCbor(
    const brillo::SecureBlob& response_payload_cbor,
    brillo::SecureBlob* mediated_point,
    brillo::SecureBlob* dealer_pub_key,
    brillo::SecureBlob* key_auth_value) {
  const auto& cbor = ReadHsmCborPayload(response_payload_cbor);
  if (!cbor) {
    return false;
  }

  const cbor::Value::MapValue& response_map = cbor->GetMap();
  const auto dealer_pub_key_entry =
      response_map.find(cbor::Value(kDealerPublicKey));
  if (dealer_pub_key_entry == response_map.end()) {
    LOG(ERROR) << "No dealer public key in the HSM response map.";
    return false;
  }
  if (!dealer_pub_key_entry->second.is_bytestring()) {
    LOG(ERROR) << "Wrongly formatted dealer key in the HSM response map.";
    return false;
  }

  const auto mediator_share_entry =
      response_map.find(cbor::Value(kMediatedPoint));
  if (mediator_share_entry == response_map.end()) {
    LOG(ERROR) << "No share entry in the HSM response map.";
    return false;
  }
  if (!mediator_share_entry->second.is_bytestring()) {
    LOG(ERROR) << "Wrongly formatted share entry in the HSM response map.";
    return false;
  }

  const auto key_auth_value_entry =
      response_map.find(cbor::Value(kKeyAuthValue));
  if (key_auth_value_entry == response_map.end()) {
    LOG(ERROR) << "No key auth value in the HSM response map.";
    return false;
  }
  if (!key_auth_value_entry->second.is_bytestring()) {
    LOG(ERROR) << "Wrongly formatted key auth value in the HSM response map.";
    return false;
  }

  dealer_pub_key->assign(dealer_pub_key_entry->second.GetBytestring().begin(),
                         dealer_pub_key_entry->second.GetBytestring().end());
  mediated_point->assign(mediator_share_entry->second.GetBytestring().begin(),
                         mediator_share_entry->second.GetBytestring().end());
  key_auth_value->assign(key_auth_value_entry->second.GetBytestring().begin(),
                         key_auth_value_entry->second.GetBytestring().end());
  return true;
}

bool GetHsmCborMapByKeyForTesting(const brillo::SecureBlob& input_cbor,
                                  const std::string& map_key,
                                  brillo::SecureBlob* value) {
  const auto& cbor = ReadHsmCborPayload(input_cbor);
  if (!cbor) {
    return false;
  }
  const cbor::Value::MapValue& cbor_map = cbor->GetMap();
  const auto value_entry = cbor_map.find(cbor::Value(map_key));
  if (value_entry == cbor_map.end()) {
    LOG(ERROR) << "No keyed entry in the HSM response map.";
    return false;
  }
  if (!value_entry->second.is_bytestring()) {
    LOG(ERROR) << "Keyed entry in the HSM response has a wrong format.";
    return false;
  }
  value->assign(value_entry->second.GetBytestring().begin(),
                value_entry->second.GetBytestring().end());
  return true;
}

bool GetHsmPayloadSchemaVersionForTesting(const brillo::SecureBlob& input_cbor,
                                          int* value) {
  const auto& cbor = ReadHsmCborPayload(input_cbor);
  if (!cbor) {
    return false;
  }
  const cbor::Value::MapValue& cbor_map = cbor->GetMap();
  const auto value_entry =
      cbor_map.find(cbor::Value(kRecoveryCryptoHsmSchemaVersion));
  if (value_entry == cbor_map.end()) {
    LOG(ERROR) << "No schema version encoded in the HSM cbor.";
    return false;
  }
  if (!value_entry->second.is_integer()) {
    LOG(ERROR) << "Schema version in HSM payload is incorrectly encoded.";
    return false;
  }
  *value = value_entry->second.GetInteger();
  return true;
}

}  // namespace cryptohome