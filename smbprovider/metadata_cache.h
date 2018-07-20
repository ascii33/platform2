// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SMBPROVIDER_METADATA_CACHE_H_
#define SMBPROVIDER_METADATA_CACHE_H_

#include <map>
#include <string>

#include <base/macros.h>
#include <base/time/time.h>

#include "smbprovider/proto.h"

namespace base {
class TickClock;
}

namespace smbprovider {

// Maintains a cache of file and directory metadata. This is the data
// that is returned by stat(); name, entry type, size, date modified.
//
// The libsmbclient API can return all metadata while enumerating a
// directory, but the Chrome FileSystemProvider API makes per entry
// requests for metadata. This cache will store the results found
// when reading a directory, then use the cache to attempt to satisfy
// requests for metadata.
//
// TODO(zentaro): Follow up CL's will implement;
//    * Invalidating requested entries based on time.
//    * Purging entries based on time.
class MetadataCache {
 public:
  // |entry_lifetime| determines how long an entry remains valid in the cache.
  MetadataCache(base::TickClock* tick_clock, base::TimeDelta entry_lifetime);
  ~MetadataCache();

  MetadataCache& operator=(MetadataCache&& other) = default;

  // Adds an entry to the cache.
  void AddEntry(const DirectoryEntry& entry);

  // Finds an entry at |full_path|. If found, returns true and out_entry
  // is set. |full_path| is a full smb url.
  bool FindEntry(const std::string& full_path, DirectoryEntry* out_entry);

  // Deletes all entries from the cache.
  void ClearAll();

  // Returns true if the cache is empty.
  bool IsEmpty() const;

 private:
  struct CacheEntry {
    CacheEntry() = default;
    CacheEntry(const DirectoryEntry& entry, base::TimeTicks expiration_time)
        : entry(entry), expiration_time(expiration_time) {}
    CacheEntry& operator=(CacheEntry&& other) = default;

    DirectoryEntry entry;
    base::TimeTicks expiration_time;

    DISALLOW_COPY_AND_ASSIGN(CacheEntry);
  };

  std::map<std::string, CacheEntry> cache_;
  base::TickClock* tick_clock_;  // Not owned
  base::TimeDelta entry_lifetime_;
  DISALLOW_COPY_AND_ASSIGN(MetadataCache);
};

}  // namespace smbprovider

#endif  // SMBPROVIDER_METADATA_CACHE_H_
