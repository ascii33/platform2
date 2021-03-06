// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TRUNKS_MOCK_COMMAND_PARSER_H_
#define TRUNKS_MOCK_COMMAND_PARSER_H_

#include "trunks/command_parser.h"

#include <string>

#include <gmock/gmock.h>

#include "trunks/tpm_generated.h"

namespace trunks {

class MockCommandParser : public CommandParser {
 public:
  ~MockCommandParser() override = default;

  MOCK_METHOD(TPM_RC,
              ParseHeader,
              (std::string*, TPMI_ST_COMMAND_TAG*, UINT32*, TPM_CC*),
              (override));

  MOCK_METHOD(TPM_RC,
              ParseCommandGetCapability,
              (std::string*, TPM_CAP*, UINT32*, UINT32*),
              (override));
};

}  // namespace trunks

#endif  // TRUNKS_MOCK_COMMAND_PARSER_H_
