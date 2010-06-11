// Copyright (c) 2009-2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "username_passkey.h"

#include <openssl/sha.h>

#include <base/logging.h>
#include <chromeos/utility.h>

#include "crypto.h"

namespace cryptohome {
using std::string;

UsernamePasskey::UsernamePasskey(const char *username,
                                 const chromeos::Blob& passkey)
    : username_(username, strlen(username)),
      passkey_() {
  passkey_.assign(passkey.begin(), passkey.end());
}

UsernamePasskey::~UsernamePasskey() {
}

void UsernamePasskey::GetFullUsername(char *name_buffer, int length) const {
  strncpy(name_buffer, username_.c_str(), length);
}

std::string UsernamePasskey::GetFullUsernameString() const {
  return username_;
}

void UsernamePasskey::GetPartialUsername(char *name_buffer, int length) const {
  size_t at_index = username_.find('@');
  string partial = username_.substr(0, at_index);
  strncpy(name_buffer, partial.c_str(), length);
}

string UsernamePasskey::GetObfuscatedUsername(
    const chromeos::Blob &system_salt) const {
  CHECK(!username_.empty());

  SHA_CTX ctx;
  unsigned char md_value[SHA_DIGEST_LENGTH];

  SHA1_Init(&ctx);
  SHA1_Update(&ctx, &system_salt[0], system_salt.size());
  SHA1_Update(&ctx, username_.c_str(), username_.length());
  SHA1_Final(md_value, &ctx);

  chromeos::Blob md_blob(md_value,
               md_value + (SHA_DIGEST_LENGTH * sizeof(unsigned char)));

  return chromeos::AsciiEncode(md_blob);
}

void UsernamePasskey::GetPasskey(SecureBlob* passkey) const {
  *passkey = passkey_;
}

}  // namespace cryptohome
