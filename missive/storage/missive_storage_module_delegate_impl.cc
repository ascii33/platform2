// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "missive/storage/missive_storage_module_delegate_impl.h"

#include <utility>

#include <base/bind.h>
#include <base/callback.h>
#include <base/logging.h>
#include <base/memory/ptr_util.h>

#include "missive/proto/record.pb.h"
#include "missive/proto/record_constants.pb.h"
#include "missive/util/status.h"

namespace reporting {

MissiveStorageModuleDelegateImpl::MissiveStorageModuleDelegateImpl(
    AddRecordCallback add_record, FlushCallback flush)
    : add_record_(std::move(add_record)), flush_(std::move(flush)) {}

MissiveStorageModuleDelegateImpl::~MissiveStorageModuleDelegateImpl() = default;

void MissiveStorageModuleDelegateImpl::AddRecord(
    Priority priority,
    Record record,
    base::OnceCallback<void(Status)> callback) {
  add_record_.Run(priority, std::move(record), std::move(callback));
}

void MissiveStorageModuleDelegateImpl::Flush(
    Priority priority, base::OnceCallback<void(Status)> callback) {
  flush_.Run(priority, std::move(callback));
}

void MissiveStorageModuleDelegateImpl::ReportSuccess(
    const SequenceInformation& sequence_information, bool force) {
  // Intended for upload, but called directly to MissiveClient.
  DLOG(FATAL) << "Should never be called";
}

void MissiveStorageModuleDelegateImpl::UpdateEncryptionKey(
    const SignedEncryptionInfo& signed_encryption_key) {
  // Intended for upload, but called directly to MissiveClient.
  DLOG(FATAL) << "Should never be called";
}

}  // namespace reporting
