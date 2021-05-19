// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MINIOS_SCREEN_LANGUAGE_DROPDOWN_H_
#define MINIOS_SCREEN_LANGUAGE_DROPDOWN_H_

#include <memory>

#include "minios/screen_base.h"

namespace minios {

class ScreenLanguageDropdown : public ScreenBase {
 public:
  ScreenLanguageDropdown(std::shared_ptr<DrawInterface> draw_utils,
                         ScreenControllerInterface* screen_controller);
  ~ScreenLanguageDropdown() = default;

  ScreenLanguageDropdown(const ScreenLanguageDropdown&) = delete;
  ScreenLanguageDropdown& operator=(const ScreenLanguageDropdown&) = delete;

  void Show() override;

  void Reset() override;

  void OnKeyPress(int key_changed) override;

  ScreenType GetScreenType() override;

 private:
  // Updates locale dropdown menu with current selection.
  void UpdateMenu();
};

}  // namespace minios

#endif  // MINIOS_SCREEN_LANGUAGE_DROPDOWN_H_
