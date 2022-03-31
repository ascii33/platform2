// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VTPM_COMMANDS_VIRTUALIZER_H_
#define VTPM_COMMANDS_VIRTUALIZER_H_

#include "vtpm/commands/command.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/callback.h>
#include <trunks/command_parser.h>
#include <trunks/real_command_parser.h>
#include <trunks/real_response_serializer.h>
#include <trunks/response_serializer.h>
#include <trunks/tpm_generated.h>

namespace vtpm {

// `Virtualizer` implements the very top level of the TPM commands execution. it
// is designed to be configurable, and determines how to execute an incoming TPM
// command request with minimalist TPM-specifics. All the definition of the way
// a virtualized TPM works is abstracted into the implementation of those
// delegated objects.
class Virtualizer : public Command {
 public:
  enum Profile {
    kGLinux,
  };
  static std::unique_ptr<Virtualizer> Create(Profile profile);
  Virtualizer(trunks::CommandParser* parser,
              trunks::ResponseSerializer* serializer,
              std::unordered_map<trunks::TPM_CC, Command*> table,
              Command* fallback_command);
  void Run(const std::string& command,
           CommandResponseCallback callback) override;

 private:
  Virtualizer() = default;

  // Functional object candidates for all profiles.
  trunks::RealResponseSerializer real_response_serializer_;
  trunks::RealCommandParser real_command_parser_;

  // Functional object candidates dynamically determined by profile.
  std::vector<std::unique_ptr<Command>> commands_;

  // Functional objects used to execute the vtpm functions. The ownership of
  // the pointees of these are owned the Virtualizer w/ the fields above
  trunks::CommandParser* command_parser_ = nullptr;
  trunks::ResponseSerializer* response_serializer_ = nullptr;
  // The command table of which entries are the objects `this` delegates a TPM
  // command to.
  std::unordered_map<trunks::TPM_CC, Command*> command_table_;
  // The command object that handles TPM commands that are not supported by
  // `this`.
  Command* fallback_command_ = nullptr;
};

}  // namespace vtpm

#endif  // VTPM_COMMANDS_VIRTUALIZER_H_
