// Copyright (c) 2009-2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Crypto - class for handling the keyset key management functions relating to
// cryptohome.  This includes wrapping/unwrapping the vault keyset (and
// supporting functions) and setting/clearing the user keyring for use with
// ecryptfs.

#ifndef CRYPTOHOME_CRYPTO_H_
#define CRYPTOHOME_CRYPTO_H_

#include <base/basictypes.h>
#include <base/file_path.h>

#include "secure_blob.h"
#include "vault_keyset.h"

namespace cryptohome {

// Default entropy source is used to seed openssl's random number generator
extern const std::string kDefaultEntropySource;

class Crypto : public EntropySource {
 public:

  // Default constructor, using the default entropy source
  Crypto();

  virtual ~Crypto();

  // Returns random bytes of the given length
  //
  // Parameters
  //   rand (OUT) - Where to store the random bytes
  //   length - The number of random bytes to store in rand
  void GetSecureRandom(unsigned char *rand, int length) const;

  // Unwraps an encrypted vault keyset.  The vault keyset should be the output
  // of WrapVaultKeyset().
  //
  // Parameters
  //   wrapped_keyset - The blob containing the encrypted keyset
  //   vault_wrapper - The passkey wrapper used to unwrap the keyset
  //   vault_keyset (OUT) - The unwrapped vault keyset on success
  bool UnwrapVaultKeyset(const chromeos::Blob& wrapped_keyset,
                         const chromeos::Blob& vault_wrapper,
                         VaultKeyset* vault_keyset) const;

  // Wraps (encrypts) the vault keyset with the given wrapper
  //
  // Parameters
  //   vault_keyset - The VaultKeyset to encrypt
  //   vault_wrapper - The passkey wrapper used to wrap the keyset
  //   vault_wrapper_salt - The salt to use for the vault wrapper when wrapping
  //                        the keyset
  //   wrapped_keyset - On success, the encrypted vault keyset
  bool WrapVaultKeyset(const VaultKeyset& vault_keyset,
                       const SecureBlob& vault_wrapper,
                       const SecureBlob& vault_wrapper_salt,
                       SecureBlob* wrapped_keyset) const;

  // Converts the passkey to a symmetric key used to decrypt the user's
  // cryptohome key.
  //
  // Parameters
  //   passkey - The passkey (hash, currently) to create the key from
  //   salt - The salt used in creating the key
  //   iters - The hash iterations to use in generating the key
  //   wrapper (OUT) - The wrapper
  void PasskeyToWrapper(const chromeos::Blob& passkey,
                        const chromeos::Blob& salt, int iters,
                        SecureBlob* wrapper) const;

  // Gets an existing salt, or creates one if it doesn't exist
  //
  // Parameters
  //   path - The path to the salt file
  //   length - The length of the new salt if it needs to be created
  //   force - If true, forces creation of a new salt even if the file exists
  //   salt (OUT) - The salt
  bool GetOrCreateSalt(const FilePath& path, int length, bool force,
                       SecureBlob* salt) const;

  // Adds the specified key to the ecryptfs keyring so that the cryptohome can
  // be mounted.  Clears the user keyring first.
  //
  // Parameters
  //   vault_keyset - The keyset to add
  //   key_signature (OUT) - The signature of the cryptohome key that should be
  //     used in subsequent calls to mount(2)
  //   fnek_signature (OUT) - The signature of the cryptohome filename
  //     encryption key that should be used in subsequent calls to mount(2)
  bool AddKeyset(const VaultKeyset& vault_keyset,
                 std::string* key_signature,
                 std::string* fnek_signature) const;

  // Clears the user's kernel keyring
  void ClearKeyset() const;

  // Encodes a binary blob to hex-ascii
  //
  // Parameters
  //   blob - The binary blob to convert
  //   buffer (IN/OUT) - Where to store the converted blob
  //   buffer_length - The size of the buffer
  static void AsciiEncodeToBuffer(const chromeos::Blob& blob, char* buffer,
                                  int buffer_length);

  // Converts a null-terminated password to a passkey (ascii-encoded first half
  // of the salted SHA1 hash of the password).
  //
  // Parameters
  //   password - The password to convert
  //   salt - The salt used during hashing
  //   passkey (OUT) - The passkey
  static void PasswordToPasskey(const char *password,
                                const chromeos::Blob& salt,
                                SecureBlob* passkey);

  // Overrides the default the entropy source
  void set_entropy_source(const std::string& entropy_source) {
    entropy_source_ = entropy_source;
  }

 private:
  // Adds the specified key to the user keyring
  //
  // Parameters
  //   key - The key to add
  //   key_sig - The key's (ascii) signature
  //   salt - The salt
  bool PushVaultKey(const SecureBlob& key, const std::string& key_sig,
                           const SecureBlob& salt) const;

  std::string entropy_source_;

  DISALLOW_COPY_AND_ASSIGN(Crypto);
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_CRYPTO_H_
