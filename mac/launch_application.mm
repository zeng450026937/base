// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "base/mac/launch_application.h"

#include "base/apple/bridging.h"
#include "base/command_line.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/mac/foundation_util.h"
#include "base/mac/launch_services_spi.h"
#include "base/mac/mac_util.h"
#include "base/metrics/histogram_functions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/types/expected.h"

namespace base::mac {

namespace {

// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum class LaunchResult {
  kSuccess = 0,
  kSuccessDespiteError = 1,
  kFailure = 2,
  kMaxValue = kFailure,
};

void LogLaunchResult(LaunchResult result) {
  UmaHistogramEnumeration("Mac.LaunchApplicationResult", result);
}

NSArray* CommandLineArgsToArgsArray(const CommandLineArgs& command_line_args) {
  if (const CommandLine* command_line =
          absl::get_if<CommandLine>(&command_line_args)) {
    const auto& argv = command_line->argv();
    size_t argc = argv.size();
    DCHECK_GT(argc, 0lu);

    NSMutableArray* args_array = [NSMutableArray arrayWithCapacity:argc - 1];
    // NSWorkspace automatically adds the binary path as the first argument and
    // thus it should not be included in the list.
    for (size_t i = 1; i < argc; ++i) {
      [args_array addObject:base::SysUTF8ToNSString(argv[i])];
    }

    return args_array;
  }

  if (const std::vector<std::string>* string_vector =
          absl::get_if<std::vector<std::string>>(&command_line_args)) {
    NSMutableArray* args_array =
        [NSMutableArray arrayWithCapacity:string_vector->size()];
    for (const auto& arg : *string_vector) {
      [args_array addObject:base::SysUTF8ToNSString(arg)];
    }

    return args_array;
  }

  return @[];
}

NSWorkspaceOpenConfiguration* GetOpenConfiguration(
    LaunchApplicationOptions options,
    const CommandLineArgs& command_line_args) {
  NSWorkspaceOpenConfiguration* config =
      [NSWorkspaceOpenConfiguration configuration];

  config.activates = options.activate;
  config.createsNewApplicationInstance = options.create_new_instance;
  config.promptsUserIfNeeded = options.prompt_user_if_needed;
  config.arguments = CommandLineArgsToArgsArray(command_line_args);

  return config;
}

NSDictionary* GetOpenOptions(LaunchApplicationOptions options,
                             const CommandLineArgs& command_line_args) {
  NSDictionary* dict = @{
    base::apple::CFToNSPtrCast(_kLSOpenOptionArgumentsKey) :
        CommandLineArgsToArgsArray(command_line_args),
    base::apple::CFToNSPtrCast(_kLSOpenOptionHideKey) :
        @(options.hidden_in_background),
    base::apple::CFToNSPtrCast(_kLSOpenOptionBackgroundLaunchKey) :
        @(options.hidden_in_background),
    base::apple::CFToNSPtrCast(_kLSOpenOptionAddToRecentsKey) :
        @(!options.hidden_in_background),
    base::apple::CFToNSPtrCast(_kLSOpenOptionActivateKey) : @(options.activate),
    base::apple::CFToNSPtrCast(_kLSOpenOptionPreferRunningInstanceKey) :
        @(!options.create_new_instance),
  };
  return dict;
}

// Sometimes macOS 11 and 12 report an error launching even though the launch
// succeeded anyway. This helper returns true for the error codes we have
// observed where scanning the list of running applications appears to be a
// usable workaround for this.
bool ShouldScanRunningAppsForError(NSError* error) {
  if (!error) {
    return false;
  }
  if (error.domain == NSCocoaErrorDomain &&
      error.code == NSFileReadUnknownError) {
    return true;
  }
  if (error.domain == NSOSStatusErrorDomain && error.code == procNotFound) {
    return true;
  }
  return false;
}

void LogResultAndInvokeCallback(const base::FilePath& app_bundle_path,
                                bool create_new_instance,
                                LaunchApplicationCallback callback,
                                NSRunningApplication* app,
                                NSError* error) {
  // Sometimes macOS 11 and 12 report an error launching even though the
  // launch succeeded anyway. To work around such cases, check if we can
  // find a running application matching the app we were trying to launch.
  // Only do this if `options.create_new_instance` is false though, as
  // otherwise we wouldn't know which instance to return.
  if (IsAtLeastOS11() && IsAtMostOS12() && !create_new_instance && !app &&
      ShouldScanRunningAppsForError(error)) {
    NSArray<NSRunningApplication*>* all_apps =
        NSWorkspace.sharedWorkspace.runningApplications;
    for (NSRunningApplication* running_app in all_apps) {
      if (NSURLToFilePath(running_app.bundleURL) == app_bundle_path) {
        LOG(ERROR) << "Launch succeeded despite error: "
                   << base::SysNSStringToUTF8(error.localizedDescription);
        app = running_app;
        break;
      }
    }
    if (app) {
      error = nil;
    }
    LogLaunchResult(app ? LaunchResult::kSuccessDespiteError
                        : LaunchResult::kFailure);
  } else {
    LogLaunchResult(app ? LaunchResult::kSuccess : LaunchResult::kFailure);
  }

  if (error) {
    LOG(ERROR) << base::SysNSStringToUTF8(error.localizedDescription);
    std::move(callback).Run(nil, error);
  } else {
    std::move(callback).Run(app, nil);
  }
}

}  // namespace

void LaunchApplication(const base::FilePath& app_bundle_path,
                       const CommandLineArgs& command_line_args,
                       const std::vector<std::string>& url_specs,
                       LaunchApplicationOptions options,
                       LaunchApplicationCallback callback) {
  __block LaunchApplicationCallback callback_block_access =
      base::BindOnce(&LogResultAndInvokeCallback, app_bundle_path,
                     options.create_new_instance, std::move(callback));

  NSURL* bundle_url = FilePathToNSURL(app_bundle_path);
  if (!bundle_url) {
    dispatch_async(dispatch_get_main_queue(), ^{
      std::move(callback_block_access)
          .Run(nil, [NSError errorWithDomain:NSCocoaErrorDomain
                                        code:NSFileNoSuchFileError
                                    userInfo:nil]);
    });
    return;
  }

  NSMutableArray* ns_urls = nil;
  if (!url_specs.empty()) {
    ns_urls = [NSMutableArray arrayWithCapacity:url_specs.size()];
    for (const auto& url_spec : url_specs) {
      [ns_urls
          addObject:[NSURL URLWithString:base::SysUTF8ToNSString(url_spec)]];
    }
  }

  if (options.hidden_in_background) {
    _LSOpenCompletionHandler action_block =
        ^void(LSASNRef asn, Boolean success, CFErrorRef cf_error) {
          NSRunningApplication* app = nil;
          if (asn) {
            app = [[NSRunningApplication alloc]
                initWithApplicationSerialNumber:asn];
          }
          NSError* error = base::apple::CFToNSPtrCast(cf_error);
          dispatch_async(dispatch_get_main_queue(), ^{
            std::move(callback_block_access).Run(app, error);
          });
        };

    _LSOpenURLsWithCompletionHandler(
        base::apple::NSToCFPtrCast(ns_urls ? ns_urls : @[]),
        mac::FilePathToCFURL(app_bundle_path),
        base::apple::NSToCFPtrCast(GetOpenOptions(options, command_line_args)),
        action_block);
    return;
  }

  void (^action_block)(NSRunningApplication*, NSError*) =
      ^void(NSRunningApplication* app, NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
          std::move(callback_block_access).Run(app, error);
        });
      };

  NSWorkspaceOpenConfiguration* configuration =
      GetOpenConfiguration(options, command_line_args);

  if (ns_urls) {
    [NSWorkspace.sharedWorkspace openURLs:ns_urls
                     withApplicationAtURL:bundle_url
                            configuration:configuration
                        completionHandler:action_block];
  } else {
    [NSWorkspace.sharedWorkspace openApplicationAtURL:bundle_url
                                        configuration:configuration
                                    completionHandler:action_block];
  }
}

}  // namespace base::mac
