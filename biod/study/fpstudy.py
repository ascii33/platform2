#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright 2021 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""A tool to manage the fingerprint study."""

from __future__ import print_function

import argparse
import glob
import logging
import os
import shutil
import stat
import sys
import tempfile

import gnupg


def find_files(path: str, ext: str) -> list:
    """
    Find all files under the given path that have the specified file extension.

    The path may be a directory or a single file. If the path is a single file,
    the extension will be checked.
    """

    files = []
    if os.path.isdir(path):
        files = glob.glob(path + '/**/*' + ext, recursive=True)
    elif os.path.isfile(path):
        _, path_ext = os.path.splitext(path)
        if path_ext != ext:
            raise Exception(f'The given path "{path}" is not a "{ext}" file')
        files = [path]
    else:
        raise Exception(
            f'The given path "{path}" is not a directory or file')

    return files


def decrypt(private_key: str, private_key_pass: str, files: list) -> bool:
    """Decrypt the given file."""

    # Enable basic stdout logging for gnupg.
    h = logging.StreamHandler()
    l = logging.getLogger('gnupg')
    l.setLevel(logging.INFO)
    l.addHandler(h)

    with tempfile.TemporaryDirectory() as gnupghome:
        os.chmod(gnupghome, stat.S_IRWXU)
        # Creating this directory makes old gnupg versions happy.
        os.makedirs(f'{gnupghome}/private-keys-v1.d', mode=stat.S_IRWXU)

        gpg = gnupg.GPG(gnupghome=gnupghome,
                        verbose=False,
                        options=[
                            '--no-options',
                            '--no-default-recipient',
                            '--trust-model', 'always',
                        ])

        with open(private_key, mode='rb') as key_file:
            key_data = key_file.read()
            if gpg.import_keys(key_data).count != 1:
                raise Exception(f'Failed to import key {private_key}.')

        for file in files:
            file_parts = os.path.splitext(file)
            assert file_parts[1] == '.gpg'
            file_output = file_parts[0]
            print(f'Decrypting file {file} to {file_output}.')
            with open(file, mode='rb') as file_input_stream:
                ret = gpg.decrypt_file(file_input_stream,
                                       always_trust=True,
                                       passphrase=private_key_pass,
                                       output=file_output)
                if not ret.ok:
                    raise Exception(f'Failed to decrypt file {file}.')

                if not os.path.exists(file_output):
                    raise Exception(
                        f'Output file {file_output} was not created.'
                    )

        # Shred all remnants GPG keys in the temp directory.
        os.system(f'find {gnupghome} -type f | xargs shred -v')
    return True


def cmd_decrypt(args: argparse.Namespace) -> int:
    """Handle the subcommand decrypt."""
    if not os.path.isfile(args.key):
        print(f'Error - The given key file {args.key} does not exist.')
        return 1

    try:
        files = find_files(args.path, '.gpg')
    except Exception as e:
        print(f'Error - {e}')
        return 1
    if not files:
        print('Error - The given path does not contain gpg files.')
        return 1

    if not files:
        print('Error - The given dir path does not contain encrypted files.')
        return 1

    if not shutil.which('shred'):
        print('Error - The shred utility does not exist.')
        return 1

    if not decrypt(args.key, args.password, files):
        return 1


def main(argv: list) -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(
        dest='subcommand', required=True, title='subcommands')

    # Parser for "decrypt" subcommand.
    parser_decrypt = subparsers.add_parser('decrypt')
    parser_decrypt.add_argument('key', help='Path to the GPG private key')
    parser_decrypt.add_argument('path',
                                help='Path to directory of encrypted captures '
                                'or single encrypted file')
    parser_decrypt.add_argument('--password', default=None,
                                help='Password for private key')
    parser_decrypt.set_defaults(func=cmd_decrypt)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
