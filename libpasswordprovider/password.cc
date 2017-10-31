// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libpasswordprovider/password.h"

#include <errno.h>
#include <sys/mman.h>

#include "base/logging.h"

namespace password_provider {

Password::~Password() {
  if (password_) {
    memset(password_, 0, buffer_alloc_size_);

    if (munlock(password_, buffer_alloc_size_)) {
      PLOG(WARNING) << "Error calling munlock.";
    }
    if (munmap(password_, buffer_alloc_size_)) {
      PLOG(ERROR) << "Error calling munmap.";
    }
  }
}

bool Password::Init() {
  // Should not allocate password memory more than once. CHECK instead of DCHECK
  // here is so that the buffer would not be left dangling with the password in
  // in it, in case that Init() is called twice.
  CHECK(!password_);

  // Memory will be page aligned, so create a buffer that takes up a whole page.
  buffer_alloc_size_ = sysconf(_SC_PAGESIZE);

  // Call mmap instead of malloc to allocate because we need memory to be page
  // aligned so that it can be locked.
  password_ = reinterpret_cast<char*>(mmap(nullptr, buffer_alloc_size_,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (password_ == MAP_FAILED) {
    PLOG(ERROR) << "Error calling mmap.";
    return false;
  }

  // At this point we should have a memory block.
  DCHECK(password_);

  // Lock buffer into physical memory.
  if (mlock(password_, buffer_alloc_size_)) {
    PLOG(ERROR) << "Error calling mlock.";
    return false;
  }

  // Mark memory as non dumpable in a core dump.
  if (madvise(password_, buffer_alloc_size_, MADV_DONTDUMP)) {
    PLOG(ERROR) << "Error calling madvise with MADV_DONTDUMP option.";
    return false;
  }

  // Mark memory as non mergeable with another page, even if the contents are
  // the same.
  if (madvise(password_, buffer_alloc_size_, MADV_UNMERGEABLE)) {
    PLOG(ERROR) << "Error calling madvise with MADV_UNMERGEABLE option.";
    return false;
  }

  // Don't make this page available to child processes.
  if (madvise(password_, buffer_alloc_size_, MADV_DONTFORK)) {
    PLOG(ERROR) << "Error calling madvise with MADV_DONTFORK option.";
    return false;
  }

  // Subtract one byte because we need to reserve space for a null terminator.
  max_size_ = buffer_alloc_size_ - 1;

  return true;
}

char* Password::GetMutableRaw() {
  DCHECK(password_);
  return password_;
}

const char* Password::GetRaw() const {
  DCHECK(password_);
  return password_;
}

void Password::SetSize(size_t size) {
  DCHECK_LE(size, max_size_);
  size_ = size;
}

}  // namespace password_provider
