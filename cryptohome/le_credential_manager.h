// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_LE_CREDENTIAL_MANAGER_H_
#define CRYPTOHOME_LE_CREDENTIAL_MANAGER_H_

#include <map>

#include "cryptohome/error/cryptohome_le_cred_error.h"
#include "cryptohome/le_credential_backend.h"
#include "cryptohome/le_credential_error.h"

namespace cryptohome {

// This is a pure virtual interface providing all the public methods necessary
// to work with the low entropy credential functionality.
class LECredentialManager {
 public:
  typedef std::map<uint32_t, uint32_t> DelaySchedule;

  virtual ~LECredentialManager() = default;

  // Inserts an LE credential into the system.
  //
  // The Low entropy credential is represented by |le_secret|, and the high
  // entropy and reset secrets by |he_secret| and |reset_secret| respectively.
  // The delay schedule which governs the rate at which CheckCredential()
  // attempts are allowed is provided in |delay_sched|. On success, returns
  // OkStatus and stores the newly provisioned label in |ret_label|. On
  // failure, returns status with:
  // - LE_CRED_ERROR_NO_FREE_LABEL if there is no free label.
  // - LE_CRED_ERROR_HASH_TREE if there was an error in the hash tree.
  //
  // The returned label should be placed into the metadata associated with the
  // Encrypted Vault Key (EVK). so that it can be used to look up the credential
  // later.

  virtual LECredStatus InsertCredential(
      const brillo::SecureBlob& le_secret,
      const brillo::SecureBlob& he_secret,
      const brillo::SecureBlob& reset_secret,
      const DelaySchedule& delay_sched,
      const ValidPcrCriteria& valid_pcr_criteria,
      uint64_t* ret_label) = 0;

  // Attempts authentication for a LE Credential.
  //
  // Checks whether the LE credential |le_secret| for a |label| is correct.
  // Returns LE_CRED_SUCCESS on success. Additionally, the released
  // high entropy credential is placed in |he_secret| and the reset secret is
  // placed in |reset_secret| if CR50 version with protocol > 0 is used.
  //
  // On failure, returns status with:
  // LE_CRED_ERROR_INVALID_LE_SECRET for incorrect authentication attempt.
  // LE_CRED_ERROR_TOO_MANY_ATTEMPTS for locked out credential (too many
  // incorrect attempts). LE_CRED_ERROR_HASH_TREE for error in hash tree.
  // LE_CRED_ERROR_INVALID_LABEL for invalid label.
  // LE_CRED_ERROR_INVALID_METADATA for invalid credential metadata.
  // LE_CRED_ERROR_PCR_NOT_MATCH if the PCR registers from TPM have unexpected
  // values, in which case only reboot will allow this user to authenticate.
  virtual LECredStatus CheckCredential(const uint64_t& label,
                                       const brillo::SecureBlob& le_secret,
                                       brillo::SecureBlob* he_secret,
                                       brillo::SecureBlob* reset_secret) = 0;

  // Attempts reset of a LE Credential.
  //
  // Returns LE_CRED_SUCCESS on success.
  //
  // On failure, returns status with:
  // - LE_CRED_ERROR_INVALID_RESET_SECRET for incorrect reset secret.
  // incorrect attempts).
  // - LE_CRED_ERROR_HASH_TREE for error in hash tree.
  // - LE_CRED_ERROR_INVALID_LABEL for invalid label.
  // - LE_CRED_ERROR_INVALID_METADATA for invalid credential metadata.
  virtual LECredStatus ResetCredential(
      const uint64_t& label, const brillo::SecureBlob& reset_secret) = 0;

  // Remove a credential at node with label |label|.
  //
  // Returns OkStatus on success.
  // On failure, returns status with:
  // - LE_CRED_ERROR_INVALID_LABEL for invalid label.
  // - LE_CRED_ERROR_HASH_TREE for hash tree error.
  virtual LECredStatus RemoveCredential(const uint64_t& label) = 0;

  // Returns whether the provided label needs valid PCR criteria attached.
  virtual bool NeedsPcrBinding(const uint64_t& label) = 0;

  // Returns the number of wrong authentication attempts done since the label
  // was reset or created. Returns -1 if |label| is not present in the tree or
  // the tree is corrupted.
  virtual int GetWrongAuthAttempts(const uint64_t& label) = 0;
};

};  // namespace cryptohome

#endif  // CRYPTOHOME_LE_CREDENTIAL_MANAGER_H_
