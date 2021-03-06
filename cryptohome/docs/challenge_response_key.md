# Challenge-response Keys

This feature allows to protect user data via signing cryptographic keys stored
on hardware tokens, rather than via passwords.

The hardware token needs to present a valid signature for the generated
challenge to unseal a secret seed value (aka "cryptohome KDF passphrase"). The
VKK key is derived from this passphrase using Scrypt KDF, and is then used, as
usual, to get VK and mount the user's cryptohome directory.

The sealing algorithm involves TPM capabilities for achieving the security
strength. The algorithm is different depending on the version of the TPM chip
installed into the Chrome OS device - see below for specific descriptions.

[TOC]

## Using Challenge-response Key for Cryptohomes

The rough idea is:

```
cryptohome_kdf_passphrase := concat(
    tpm_sealed_secret, deterministic_signature_of_salt)
```

where `tpm_sealed_secret` is the secret blob which is sealed using TPM in a way
that unsealing requires presenting valid signatures of the specified blobs, and
`deterministic_signature_of_salt` is the signature of a per-user salt obtained
using a deterministic signing algorithm.

TODO(emaxx): Fill this up while the feature gets implemented.

## Sealing/Unsealing Algorithm for TPM 2.0

This algorithm is based on the "TPM2_PolicySigned Extended Authorization Policy"
feature of TPM 2.0. In essence, it allows to encrypt the given blob of data via
the TPM's SRK and associate it with the given public key, so that the TPM will
only decrypt it back after being presented a valid signature for the
TPM-generated random nonce.

Technically, the TPM2_PolicySigned function is used when preparing the policy
session both for sealing and for unsealing. The difference is that the signature
is required only for the latter; for the former, the so-called "trial" policy
session is used and only the public key information is provided.

Additionally, the sealed data is bound to PCR0 (via the TPM2_PolicyPCR policy).

The input for the sealing operation - that is, the actual KDF passphrase - is a
randomly generated blob.

### Sealing Diagram for TPM 2.0

```
1. Generate Random Passphrase
           |
           v
2. Prepare Trial Policy Session (using
TPM2_PolicySigned et al., with supplying public key
information)
           |
           v
3. Seal (using TPM2_Create)
           |
           v
4. Store Sealed Blob Persistently
```

### Unsealing Diagram for TPM 2.0

```
1. Load Sealed Blob
           |
           v
2. Obtain TPM's nonce from Policy Session
           |
           v
3. Request via IPC Signature of nonce
           |
           v
4. Prepare Policy Session (using TPM2_PolicySigned et
al., with supplying public key information and
signature of nonce)
           |
           v
5. Unseal (using TPM2_Unseal)
```

### Cryptographic parameters for TPM 2.0 Sealing/Unsealing Algorithm

*   The protection key on the hardware token: An RSA key of size 2048 or 1024
    bits, with no specific restrictions for the public exponent. Supports the
    RSASSA-PKCS1-v1_5 signature scheme with either of the following hash
    functions: SHA-256 or SHA-384 or SHA-512 or SHA-1 (tie breaking will be
    based on the preference order as reported by the middleware, with the
    exception of always considering SHA-1 as the least preferred option).
*   The passphrase: Of length 256 bits. Generated using the TPM???s internal
    random number generator.

## Sealing/Unsealing Algorithm for TPM 1.2

This algorithm is based on the "Certified Migratable Key" (CMK) feature of TPM
1.2. In a nutshell, CMK is an RSA key generated by the TPM and stored encrypted
by its SRK. The CMK is associated at the creation time with the specified
Migration Authority key (???MA key???). The TPM allows to "migrate" the CMK onto the
specified Migration Destination Key - that is, re-encrypt it from SRK onto the
Migration Destination key - if a valid signature with the MA key is provided.
The data to be signed is derived (by SHA-1 hashing) from the public keys of all
three keys involved in the migration - that is, the CMK, the MA Key and the
Migration Destination Key.

In our application, the role of the secret data to be sealed will be played by
the private part of the CMK (or, to be precise, its SHA-256 hash - in order to
avoid any bias). The CMK will be generated randomly by the TPM during the first
sign-in. The protection key from the hardware token will be the MA Key. Finally,
the Migration Destination Key will be generated randomly on each sign-in of the
user.

Some of operations with CMKs require special privileges; for those, some
additional permissions were added to the delegate that is created during the TPM
ownership taking. The delegate is also changed to be bound to PCR0, which
implies that CMK unsealing is restricted to that PCR as well.

### Sealing Diagram for TPM 1.2

```
1. Obtain Migration Authority Approval Ticket for the
Protection Key (via TPM_CMK_ApproveMA, with supplying
protection public key)
           |
           v
2. Create CMK (via TPM_CMK_CreateKey, with supplying
protection public key and migration authority approval
ticket from step #1)
           |
           v
3. Store SRK-wrapped CMK Persistently
```

### Unsealing Diagram for TPM 1.2

```
1. Load SRK-wrapped CMK
           |
           v
2. Generate Migration Destination Key randomly
           |
           v
3. Request via IPC Signature of Blob Formed from
three public keys (protection key, migration
destination key, and CMK)
           |
           v
4. Obtain Migration Authorization Blob for
Migration Destination key (via
TPM_AuthorizeMigrationKey, with passing migration
destination public key)
           |
           v
5. Obtain CMK Migration Signature Ticket for
Signature Blob (via TPM_CMK_CreateTicket, with
supplying three public keys and the signature blob)
           |
           v
6. Obtain Migrated CMK Blob and Migration Random Blob
(via Tspi_Key_CMKCreateBlob, with supplying
SRK-wrapped CMK, three public keys, migration
authorization blob from step #4, CMK migration
signature ticket from step #5)
           |
           v
7. Decrypt and Decode the CMK Private Key (via RSA
OAEP MGF1, with supplying migration destination
private key, and via second pass of OAEP MGF1
decoding, with supplying migration random blob)
           |
           v
8. Return SHA-256 Hash of CMK Private Key
```

### Cryptographic parameters for TPM 1.2 Sealing/Unsealing Algorithm

*   The protection key on the hardware token: An RSA key of size 2048 or 1024
    bits, with the public exponent equal to 65537. Supports the
    RSASSA-PKCS1-v1_5 signature scheme with the SHA-1 hash function.
*   CMK: An RSA key, with the public exponent equal to 65537. Of length 2048
    bits. Generated by the TPM itself using its own algorithm and its internal
    random number generator.
*   The Migration Destination Key: An RSA key, with the public exponent equal
    to 65537. Of length 2048 bits. Used for encryption/decryption with the RSA
    OAEP MGF1 algorithm. Generated via OpenSSL using the system pseudorandom
    number generator.
