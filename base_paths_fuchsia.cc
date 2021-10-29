// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/base_paths.h"

#include <stdlib.h>

#include "base/base_paths_fuchsia.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/fuchsia/file_utils.h"
#include "base/notreached.h"
#include "base/path_service.h"
#include "base/process/process.h"

namespace base {

bool PathProviderFuchsia(int key, FilePath* result) {
  switch (key) {
    case FILE_MODULE:
      NOTIMPLEMENTED_LOG_ONCE() << " for FILE_MODULE.";
      return false;
    case FILE_EXE:
      *result = CommandLine::ForCurrentProcess()->GetProgram();
      return true;
    case DIR_APP_DATA:
      *result = base::FilePath(base::kPersistedDataDirectoryPath);
      return true;
    case DIR_ASSETS:
    case DIR_SOURCE_ROOT:
      *result = base::FilePath(base::kPackageRootDirectoryPath);
      return true;
    case DIR_USER_DESKTOP:
      // TODO(crbug.com/1231928): Implement this case.
      NOTIMPLEMENTED_LOG_ONCE() << " for DIR_USER_DESKTOP.";
      return false;
    case DIR_HOME:
      // TODO(crbug.com/1231928) Provide a proper base::GetHomeDir()
      // implementation for Fuchsia and remove this case statement. See also
      // crbug.com/1261284. For now, log, return false, and let the base
      // implementation handle it. This will end up returning a temporary
      // directory.
      NOTIMPLEMENTED_LOG_ONCE() << "for DIR_HOME. Will use temporary dir.";
      return false;
  }
  return false;
}

}  // namespace base
