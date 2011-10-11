// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>
#include <X11/extensions/XTest.h>

#include "base/file_util.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/scoped_temp_dir.h"
#include "power_manager/file_tagger.h"

namespace power_manager {

class FileTaggerTest : public ::testing::Test {
 public:
  FileTaggerTest() {}

  virtual void SetUp() {
    // Create a temporary directory for the test files
    temp_dir_generator_.reset(new ScopedTempDir());
    ASSERT_TRUE(temp_dir_generator_->CreateUniqueTempDir());
    EXPECT_TRUE(temp_dir_generator_->IsValid());
    // Initialize the file tagger
    file_tagger_.reset(new FileTagger(temp_dir_generator_->path()));
    file_tagger_->Init();
  }

 protected:
  scoped_ptr<FileTagger> file_tagger_;
  scoped_ptr<ScopedTempDir> temp_dir_generator_;
};

struct CheckFileTaggerData {
  GMainLoop* loop;
  FileTagger* file_tagger;
};

static gboolean CheckFileTagger(gpointer data) {
  CHECK(data);
  CheckFileTaggerData* ft_data = static_cast<CheckFileTaggerData*>(data);
  CHECK(ft_data->file_tagger);
  if (ft_data->file_tagger->can_tag_files())
    g_main_loop_quit(ft_data->loop);
  return false;
}

static gboolean QuitLoop(gpointer data) {
  GMainLoop* loop = static_cast<GMainLoop*>(data);
  LOG(INFO) << "Timed out waiting for inotify to call TraceFileChangeHandler.";
  g_main_loop_quit(loop);
  return false;
}

TEST_F(FileTaggerTest, SuspendFile) {
  // Directory should be empty, so tagging is allowed at start.
  EXPECT_TRUE(file_tagger_->can_tag_files_);
  // Make sure the file does not exist.
  EXPECT_FALSE(file_util::PathExists(file_tagger_->suspend_file_));
  // Simulate suspend event and see if suspend file has been created.
  file_tagger_->HandleSuspendEvent();
  EXPECT_TRUE(file_util::PathExists(file_tagger_->suspend_file_));
  // Simulate resume event and see if suspend file has been deleted.
  file_tagger_->HandleResumeEvent();
  EXPECT_FALSE(file_util::PathExists(file_tagger_->suspend_file_));
  // Simulate suspend event again.  The suspend file should return.
  file_tagger_->HandleSuspendEvent();
  EXPECT_TRUE(file_util::PathExists(file_tagger_->suspend_file_));
}

TEST_F(FileTaggerTest, LowBatteryFile) {
  // Directory should be empty, so tagging is allowed at start.
  EXPECT_TRUE(file_tagger_->can_tag_files_);
  EXPECT_FALSE(file_util::PathExists(file_tagger_->low_battery_file_));
  // Battery is not critical (not low, or on AC power) so file should not exist.
  file_tagger_->HandleSafeBatteryEvent();
  EXPECT_FALSE(file_util::PathExists(file_tagger_->low_battery_file_));
  // Go to critical state, file should exist.
  file_tagger_->HandleLowBatteryEvent();
  EXPECT_TRUE(file_util::PathExists(file_tagger_->low_battery_file_));
  // Return to safe state, file should not exist.
  file_tagger_->HandleSafeBatteryEvent();
  EXPECT_FALSE(file_util::PathExists(file_tagger_->low_battery_file_));
}

TEST_F(FileTaggerTest, FileCache) {
  // Directory should be empty, so tagging is allowed at start.
  EXPECT_TRUE(file_tagger_->can_tag_files_);
  // Create suspend file that will block tagging later.
  file_tagger_->HandleSuspendEvent();
  EXPECT_TRUE(file_util::PathExists(file_tagger_->suspend_file_));
  EXPECT_FALSE(file_util::PathExists(file_tagger_->low_battery_file_));
  // Now destroy and re-create the file tagger object to simulate a restart
  // without cleaning up the created files.
  file_tagger_.reset(new FileTagger(temp_dir_generator_->path()));
  file_tagger_->Init();
  // Suspend file should still exist.  Low battery file should not exist.
  EXPECT_TRUE(file_util::PathExists(file_tagger_->suspend_file_));
  EXPECT_FALSE(file_util::PathExists(file_tagger_->low_battery_file_));
  // No write access at this point because a tagged file exists.
  EXPECT_FALSE(file_tagger_->can_tag_files_);
  // Simulate suspend, resume, and low battery events.  The file system should
  // not be changed.  Instead, the events should be cached.
  file_tagger_->HandleSuspendEvent();
  file_tagger_->HandleResumeEvent();
  file_tagger_->HandleSuspendEvent();
  file_tagger_->HandleLowBatteryEvent();
  EXPECT_TRUE(file_util::PathExists(file_tagger_->suspend_file_));
  EXPECT_FALSE(file_util::PathExists(file_tagger_->low_battery_file_));
  EXPECT_FALSE(file_tagger_->cached_files_.find(file_tagger_->suspend_file_) ==
               file_tagger_->cached_files_.end());
  EXPECT_FALSE(file_tagger_->cached_files_.find(file_tagger_->low_battery_file_)
               == file_tagger_->cached_files_.end());
  // Here comes the tricky part.  When suspend_file_ is deleted, inotify will
  // call the file tagger's callback and enable file tagging.  However, this is
  // nondeterministic, so use a generous timeout loop and pend until the file
  // tag flag is set.  After it has either been set or exceeded the timeout,
  // proceed with EXPECT checks.  Otherwise, the callback may not have been
  // called before the EXPECTs.
  file_util::Delete(file_tagger_->suspend_file_, false);

  CheckFileTaggerData data;
  GMainLoop* loop = g_main_loop_new(NULL, false);
  data.loop = loop;
  data.file_tagger = file_tagger_.get();
  // Allow 60 seconds for the system to notify file tagger after a file
  // has been deleted.
  const int timeout = 60;
  for (int t = 0; t < timeout && !file_tagger_->can_tag_files_; t += 1)
    g_timeout_add(t * 1000, CheckFileTagger, &data);
  g_timeout_add(timeout * 1000, QuitLoop, loop);
  g_main_loop_run(loop);

  EXPECT_TRUE(file_tagger_->can_tag_files_);
  // Now both files should exist, after cache has written them.
  EXPECT_TRUE(file_util::PathExists(file_tagger_->suspend_file_));
  EXPECT_TRUE(file_util::PathExists(file_tagger_->low_battery_file_));
}

}  // namespace power_manager
