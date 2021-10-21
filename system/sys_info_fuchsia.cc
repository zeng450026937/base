// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/system/sys_info.h"

#include <sys/statvfs.h>
#include <zircon/syscalls.h>

#include "base/containers/flat_map.h"
#include "base/files/file_util.h"
#include "base/fuchsia/fuchsia_logging.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/numerics/clamped_math.h"
#include "base/synchronization/lock.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"

namespace base {

namespace {

bool GetDiskSpaceInfo(const FilePath& path,
                      int64_t* available_bytes,
                      int64_t* total_bytes) {
  struct statvfs stats;
  if (statvfs(path.value().c_str(), &stats) != 0) {
    PLOG(ERROR) << "statvfs() for path:" << path;
    return false;
  }

  if (available_bytes) {
    ClampedNumeric<int64_t> available_blocks(stats.f_bavail);
    *available_bytes = available_blocks * stats.f_frsize;
  }

  if (total_bytes) {
    ClampedNumeric<int64_t> total_blocks(stats.f_blocks);
    *total_bytes = total_blocks * stats.f_frsize;
  }

  return true;
}

struct TotalDiskSpace {
  Lock lock;
  flat_map<FilePath, int64_t> space_map GUARDED_BY(lock);
};

TotalDiskSpace& GetTotalDiskSpace() {
  static NoDestructor<TotalDiskSpace> total_disk_space;
  return *total_disk_space;
}

// Returns the total-disk-space set for the volume containing |path|. If
// |volume_path| is non-null then it receives the path to the relevant volume.
// Returns -1, and does not modify |volume_path|, if no match is found.
int64_t GetAmountOfTotalDiskSpaceAndVolumePath(const FilePath& path,
                                               FilePath* volume_path) {
  CHECK(path.IsAbsolute());
  TotalDiskSpace& total_disk_space = GetTotalDiskSpace();

  AutoLock l(total_disk_space.lock);
  int64_t result = -1;
  FilePath matched_path;
  for (const auto& path_and_size : total_disk_space.space_map) {
    if (path_and_size.first == path || path_and_size.first.IsParent(path)) {
      // If a deeper path was already matched then ignore this entry.
      if (!matched_path.empty() && !matched_path.IsParent(path_and_size.first))
        continue;
      matched_path = path_and_size.first;
      result = path_and_size.second;
    }
  }

  if (volume_path)
    *volume_path = matched_path;
  return result;
}

}  // namespace

// static
int64_t SysInfo::AmountOfPhysicalMemoryImpl() {
  return zx_system_get_physmem();
}

// static
int64_t SysInfo::AmountOfAvailablePhysicalMemoryImpl() {
  // TODO(https://crbug.com/986608): Implement this.
  NOTIMPLEMENTED_LOG_ONCE();
  return 0;
}

// static
int SysInfo::NumberOfProcessors() {
  return zx_system_get_num_cpus();
}

// static
int64_t SysInfo::AmountOfVirtualMemory() {
  return 0;
}

// static
std::string SysInfo::OperatingSystemName() {
  return "Fuchsia";
}

// static
int64_t SysInfo::AmountOfFreeDiskSpace(const FilePath& path) {
  ScopedBlockingCall scoped_blocking_call(FROM_HERE, BlockingType::MAY_BLOCK);

  // First check whether there is a soft-quota that applies to |path|.
  FilePath volume_path;
  const int64_t total_space =
      GetAmountOfTotalDiskSpaceAndVolumePath(path, &volume_path);
  if (total_space >= 0) {
    // TODO(crbug.com/1148334): Replace this with an efficient implementation.
    const int64_t used_space = ComputeDirectorySize(volume_path);
    return std::max(0l, total_space - used_space);
  }

  // Report the actual amount of free space in |path|'s filesystem.
  int64_t available;
  if (GetDiskSpaceInfo(path, &available, nullptr))
    return available;

  return -1;
}

// static
int64_t SysInfo::AmountOfTotalDiskSpace(const FilePath& path) {
  ScopedBlockingCall scoped_blocking_call(FROM_HERE, BlockingType::MAY_BLOCK);

  if (path.empty())
    return -1;

  // Return the soft-quota that applies to |path|, if one is configured.
  int64_t total_space = GetAmountOfTotalDiskSpaceAndVolumePath(path, nullptr);
  if (total_space >= 0)
    return total_space;

  // Report the actual space in |path|'s filesystem.
  if (GetDiskSpaceInfo(path, nullptr, &total_space))
    return total_space;

  return -1;
}

// static
void SysInfo::SetAmountOfTotalDiskSpace(const FilePath& path, int64_t bytes) {
  DCHECK(path.IsAbsolute());
  TotalDiskSpace& total_disk_space = GetTotalDiskSpace();
  AutoLock l(total_disk_space.lock);
  if (bytes >= 0)
    total_disk_space.space_map[path] = bytes;
  else
    total_disk_space.space_map.erase(path);
}

// static
std::string SysInfo::OperatingSystemVersion() {
  return zx_system_get_version_string();
}

// static
void SysInfo::OperatingSystemVersionNumbers(int32_t* major_version,
                                            int32_t* minor_version,
                                            int32_t* bugfix_version) {
  // Fuchsia doesn't have OS version numbers.
  *major_version = 0;
  *minor_version = 0;
  *bugfix_version = 0;
}

// static
std::string SysInfo::OperatingSystemArchitecture() {
#if defined(ARCH_CPU_X86_64)
  return "x86_64";
#elif defined(ARCH_CPU_ARM64)
  return "aarch64";
#else
#error Unsupported architecture.
#endif
}

// static
std::string SysInfo::CPUModelName() {
  NOTIMPLEMENTED_LOG_ONCE();
  return std::string();
}

// static
size_t SysInfo::VMAllocationGranularity() {
  return getpagesize();
}

}  // namespace base
