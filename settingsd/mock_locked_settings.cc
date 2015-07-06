// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "settingsd/mock_locked_settings.h"

#include "settingsd/mock_settings_document.h"

namespace settingsd {

MockLockedVersionComponent::MockLockedVersionComponent() {}

MockLockedVersionComponent::~MockLockedVersionComponent() {}

std::string MockLockedVersionComponent::GetSourceId() const {
  return source_id_;
}

void MockLockedVersionComponent::SetSourceId(const std::string& source_id) {
  source_id_ = source_id;
}

MockLockedSettingsContainer::MockLockedSettingsContainer(
    std::unique_ptr<const SettingsDocument> payload)
    : payload_(std::move(payload)) {}

MockLockedSettingsContainer::~MockLockedSettingsContainer() {}

std::vector<const LockedVersionComponent*>
MockLockedSettingsContainer::GetVersionComponents() const {
  std::vector<const LockedVersionComponent*> result;
  for (auto& entry : version_component_blobs_)
    result.push_back(entry.second.get());
  return result;
}

std::unique_ptr<const SettingsDocument>
MockLockedSettingsContainer::DecodePayloadInternal() {
  return std::unique_ptr<const SettingsDocument>(std::move(payload_));
}

MockLockedVersionComponent* MockLockedSettingsContainer::GetVersionComponent(
    const std::string& source_id) {
  std::unique_ptr<MockLockedVersionComponent>& blob =
      version_component_blobs_[source_id];
  if (!blob)
    blob.reset(new MockLockedVersionComponent());
  return blob.get();
}

}  // namespace settingsd
