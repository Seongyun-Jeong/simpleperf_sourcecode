/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>

#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <ziparchive/zip_writer.h>

#include "cmd_api_impl.h"
#include "command.h"
#include "environment.h"
#include "event_type.h"
#include "utils.h"
#include "workload.h"

namespace simpleperf {
namespace {

const std::string SIMPLEPERF_DATA_DIR = "simpleperf_data";

class PrepareCommand : public Command {
 public:
  PrepareCommand()
      : Command("api-prepare", "Prepare recording via app api",
                // clang-format off
"Usage: simpleperf api-prepare [options]\n"
"--app <package_name>    the android application to record via app api\n"
"--days <days>           By default, the recording permission is reset after device reboot.\n"
"                        But on Android >= 13, we can use this option to set how long we want\n"
"                        the permission to last. It can last after device reboot.\n"
                // clang-format on
        ) {}
  bool Run(const std::vector<std::string>& args);

 private:
  bool ParseOptions(const std::vector<std::string>& args);
  std::optional<uint32_t> GetAppUid();

  std::string app_name_;
  uint64_t days_ = 0;
};

bool PrepareCommand::Run(const std::vector<std::string>& args) {
  if (!ParseOptions(args)) {
    return false;
  }
  // Enable profiling.
  if (GetAndroidVersion() >= 13 && !app_name_.empty() && days_ != 0) {
    // Enable app recording via persist properties.
    uint64_t duration_in_sec;
    uint64_t expiration_time;
    if (__builtin_mul_overflow(days_, 24 * 3600, &duration_in_sec) ||
        __builtin_add_overflow(time(nullptr), duration_in_sec, &expiration_time)) {
      expiration_time = UINT64_MAX;
    }
    std::optional<uint32_t> uid = GetAppUid();
    if (!uid) {
      return false;
    }
    if (!android::base::SetProperty("persist.simpleperf.profile_app_uid",
                                    std::to_string(uid.value())) ||
        !android::base::SetProperty("persist.simpleperf.profile_app_expiration_time",
                                    std::to_string(expiration_time))) {
      LOG(ERROR) << "failed to set system properties";
      return false;
    }
  } else {
    // Enable app recording via security.perf_harden.
    if (!CheckPerfEventLimit()) {
      return false;
    }
  }

  // Create tracepoint_events file.
  return EventTypeManager::Instance().WriteTracepointsToFile("/data/local/tmp/tracepoint_events");
}

bool PrepareCommand::ParseOptions(const std::vector<std::string>& args) {
  OptionValueMap options;
  std::vector<std::pair<OptionName, OptionValue>> ordered_options;
  static const OptionFormatMap option_formats = {
      {"--app", {OptionValueType::STRING, OptionType::SINGLE, AppRunnerType::NOT_ALLOWED}},
      {"--days", {OptionValueType::UINT, OptionType::SINGLE, AppRunnerType::NOT_ALLOWED}},
  };
  if (!PreprocessOptions(args, option_formats, &options, &ordered_options, nullptr)) {
    return false;
  }

  if (auto value = options.PullValue("--app"); value) {
    app_name_ = *value->str_value;
  }
  if (!options.PullUintValue("--days", &days_)) {
    return false;
  }
  return true;
}

std::optional<uint32_t> PrepareCommand::GetAppUid() {
  std::unique_ptr<FILE, decltype(&pclose)> fp(popen("pm list packages -U", "re"), pclose);
  std::string content;
  if (!fp || !android::base::ReadFdToString(fileno(fp.get()), &content)) {
    PLOG(ERROR) << "failed to run `pm list packages -U`";
    return std::nullopt;
  }
  std::regex re(R"(package:([\w\.]+)\s+uid:(\d+))");
  std::sregex_iterator match_it(content.begin(), content.end(), re);
  std::sregex_iterator match_end;
  while (match_it != match_end) {
    std::smatch match = *match_it++;
    std::string name = match.str(1);
    uint32_t uid;
    if (!android::base::ParseUint(match.str(2), &uid)) {
      continue;
    }
    if (name == app_name_) {
      return uid;
    }
  }
  LOG(ERROR) << "failed to find package " << app_name_;
  return std::nullopt;
}

class CollectCommand : public Command {
 public:
  CollectCommand()
      : Command("api-collect", "Collect recording data generated by app api",
                // clang-format off
"Usage: simpleperf api-collect [options]\n"
"--app <package_name>    the android application having recording data\n"
"-o record_zipfile_path  the path to store recording data\n"
"                        Default is simpleperf_data.zip.\n"
#if 0
// Below options are only used internally and shouldn't be visible to the public.
"--in-app               We are already running in the app's context.\n"
"--out-fd <fd>          Write output to a file descriptor.\n"
"--stop-signal-fd <fd>  Stop recording when fd is readable.\n"
#endif
                // clang-format on
        ) {
  }
  bool Run(const std::vector<std::string>& args);

 private:
  bool ParseOptions(const std::vector<std::string>& args);
  void HandleStopSignal();
  bool CollectRecordingData();
  bool RemoveRecordingData();

  std::string app_name_;
  std::string output_filepath_ = "simpleperf_data.zip";
  bool in_app_context_ = false;
  android::base::unique_fd out_fd_;
  android::base::unique_fd stop_signal_fd_;
};

bool CollectCommand::Run(const std::vector<std::string>& args) {
  if (!ParseOptions(args)) {
    return false;
  }
  if (in_app_context_) {
    HandleStopSignal();
    return CollectRecordingData() && RemoveRecordingData();
  }
  return RunInAppContext(app_name_, Name(), args, 0, output_filepath_, false);
}

bool CollectCommand::ParseOptions(const std::vector<std::string>& args) {
  OptionValueMap options;
  std::vector<std::pair<OptionName, OptionValue>> ordered_options;
  if (!PreprocessOptions(args, GetApiCollectCmdOptionFormats(), &options, &ordered_options,
                         nullptr)) {
    return false;
  }

  if (auto value = options.PullValue("--app"); value) {
    app_name_ = *value->str_value;
  }
  in_app_context_ = options.PullBoolValue("--in-app");

  if (auto value = options.PullValue("-o"); value) {
    output_filepath_ = *value->str_value;
  }
  if (auto value = options.PullValue("--out-fd"); value) {
    out_fd_.reset(static_cast<int>(value->uint_value));
  }
  if (auto value = options.PullValue("--stop-signal-fd"); value) {
    stop_signal_fd_.reset(static_cast<int>(value->uint_value));
  }

  CHECK(options.values.empty());
  CHECK(ordered_options.empty());
  if (!in_app_context_) {
    if (app_name_.empty()) {
      LOG(ERROR) << "--app is missing";
      return false;
    }
  }
  return true;
}

void CollectCommand::HandleStopSignal() {
  int fd = stop_signal_fd_.release();
  std::thread thread([fd]() {
    char c;
    static_cast<void>(read(fd, &c, 1));
    exit(1);
  });
  thread.detach();
}

bool CollectCommand::CollectRecordingData() {
  std::unique_ptr<FILE, decltype(&fclose)> fp(android::base::Fdopen(std::move(out_fd_), "w"),
                                              fclose);
  if (fp == nullptr) {
    PLOG(ERROR) << "failed to call fdopen";
    return false;
  }
  std::vector<char> buffer(64 * 1024);
  ZipWriter zip_writer(fp.get());
  for (const auto& name : GetEntriesInDir(SIMPLEPERF_DATA_DIR)) {
    // No need to collect temporary files.
    const std::string path = SIMPLEPERF_DATA_DIR + "/" + name;
    if (android::base::StartsWith(name, "TemporaryFile-") || !IsRegularFile(path)) {
      continue;
    }
    int result = zip_writer.StartEntry(name.c_str(), ZipWriter::kCompress);
    if (result != 0) {
      LOG(ERROR) << "failed to start zip entry " << name << ": "
                 << zip_writer.ErrorCodeString(result);
      return false;
    }
    android::base::unique_fd in_fd(FileHelper::OpenReadOnly(path));
    if (in_fd == -1) {
      PLOG(ERROR) << "failed to open " << path;
      return false;
    }
    while (true) {
      ssize_t nread = TEMP_FAILURE_RETRY(read(in_fd, buffer.data(), buffer.size()));
      if (nread < 0) {
        PLOG(ERROR) << "failed to read " << path;
        return false;
      }
      if (nread == 0) {
        break;
      }
      result = zip_writer.WriteBytes(buffer.data(), nread);
      if (result != 0) {
        LOG(ERROR) << "failed to write zip entry " << name << ": "
                   << zip_writer.ErrorCodeString(result);
        return false;
      }
    }
    result = zip_writer.FinishEntry();
    if (result != 0) {
      LOG(ERROR) << "failed to finish zip entry " << name << ": "
                 << zip_writer.ErrorCodeString(result);
      return false;
    }
  }
  int result = zip_writer.Finish();
  if (result != 0) {
    LOG(ERROR) << "failed to finish zip writer: " << zip_writer.ErrorCodeString(result);
    return false;
  }
  return true;
}

bool CollectCommand::RemoveRecordingData() {
  return Workload::RunCmd({"rm", "-rf", SIMPLEPERF_DATA_DIR});
}
}  // namespace

void RegisterAPICommands() {
  RegisterCommand("api-prepare", [] { return std::unique_ptr<Command>(new PrepareCommand()); });
  RegisterCommand("api-collect", [] { return std::unique_ptr<Command>(new CollectCommand()); });
}

}  // namespace simpleperf