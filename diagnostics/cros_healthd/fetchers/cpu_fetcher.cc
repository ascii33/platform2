// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/cros_healthd/fetchers/cpu_fetcher.h"

#include <sys/utsname.h>

#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <base/check.h>
#include <base/check_op.h>
#include <base/files/file_enumerator.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>
#include <re2/re2.h>

#include "diagnostics/cros_healthd/system/system_utilities_constants.h"
#include "diagnostics/cros_healthd/utils/error_utils.h"
#include "diagnostics/cros_healthd/utils/file_utils.h"
#include "diagnostics/cros_healthd/utils/procfs_utils.h"

namespace diagnostics {

namespace {

namespace mojo_ipc = ::chromeos::cros_healthd::mojom;

using VulnerabilityInfoMap =
    base::flat_map<std::string, mojo_ipc::VulnerabilityInfoPtr>;

// Regex used to parse kPresentFileName.
constexpr char kPresentFileRegex[] = R"((\d+)-(\d+))";

// Pattern that all C-state directories follow.
constexpr char kCStateDirectoryMatcher[] = "state*";

// Keys used to parse information from /proc/cpuinfo.
constexpr char kModelNameKey[] = "model name";
constexpr char kPhysicalIdKey[] = "physical id";
constexpr char kProcessorIdKey[] = "processor";
constexpr char kX86CpuFlagsKey[] = "flags";
constexpr char kArmCpuFlagsKey[] = "Features";

// Regex used to parse /proc/stat.
constexpr char kRelativeStatFileRegex[] = R"(cpu(\d+)\s+(\d+) \d+ (\d+) (\d+))";

// Directory containing all CPU temperature subdirectories.
const char kHwmonDir[] = "sys/class/hwmon/";
// Subdirectory of sys/class/hwmon/hwmon*/ which sometimes contains the CPU
// temperature files.
const char kDeviceDir[] = "device";
// Matches all CPU temperature subdirectories of |kHwmonDir|.
const char kHwmonDirectoryPattern[] = "hwmon*";
// Matches all files containing CPU temperatures.
const char kCPUTempFilePattern[] = "temp*_input";
// String "aeskl" indicates keylocker support.
const char kKeylockerAeskl[] = "aeskl";

// Patterns used to match the status of CPU vulnerability.
// The possible output formats can be found here:
// https://www.kernel.org/doc/html/latest/admin-guide/hw-vuln
constexpr char kKvmPrefix[] = "KVM: ";
constexpr char kNotAffectedPattern[] = "Not affected";
constexpr char kVulnerablePattern[] = "Vulnerable";
// https://github.com/torvalds/linux/blob/df0cc57e057f18e44dac8e6c18aba47ab53202f9/arch/x86/kernel/cpu/bugs.c#L1649
constexpr char kProcessorVulnerablePattern[] = "Processor vulnerable";
constexpr char kMitigationPattern[] = "Mitigation";
constexpr char kUnknownPattern[] = "Unknown";

// The different SMT control file content that indicates the state of SMT
// control.
constexpr char kSmtControlOnContent[] = "on";
constexpr char kSmtControlOffContent[] = "off";
constexpr char kSmtControlForceOffContent[] = "forceoff";
constexpr char kSmtControlNotSupportedContent[] = "notsupported";
constexpr char kSmtControlNotImplementedContent[] = "notimplemented";

// Contains the values parsed from /proc/stat for a single logical CPU.
struct ParsedStatContents {
  uint64_t user_time_user_hz;
  uint64_t system_time_user_hz;
  uint64_t idle_time_user_hz;
};

// Read system temperature sensor data and appends it to |out_contents|. Returns
// |true| iff there was at least one sensor value in given |sensor_dir|.
bool ReadTemperatureSensorInfo(
    const base::FilePath& sensor_dir,
    std::vector<mojo_ipc::CpuTemperatureChannelPtr>* out_contents) {
  bool has_data = false;

  base::FileEnumerator enumerator(
      sensor_dir, false, base::FileEnumerator::FILES, kCPUTempFilePattern);
  for (base::FilePath temperature_path = enumerator.Next();
       !temperature_path.empty(); temperature_path = enumerator.Next()) {
    // Get appropriate temp*_label file.
    std::string label_path = temperature_path.MaybeAsASCII();
    if (label_path.empty()) {
      LOG(WARNING) << "Unable to parse a path to temp*_input file as ASCII";
      continue;
    }
    base::ReplaceSubstringsAfterOffset(&label_path, 0, "input", "label");
    base::FilePath name_path = sensor_dir.Append("name");

    // Get the label describing this temperature. Use temp*_label
    // if present, fall back on name file.
    std::string label;
    if (base::PathExists(base::FilePath(label_path))) {
      ReadAndTrimString(base::FilePath(label_path), &label);
    } else if (base::PathExists(base::FilePath(name_path))) {
      ReadAndTrimString(name_path, &label);
    }

    // Read temperature in millidegree Celsius.
    int32_t temperature = 0;
    if (ReadInteger(temperature_path, base::StringToInt, &temperature)) {
      has_data = true;
      // Convert from millidegree Celsius to Celsius.
      temperature /= 1000;

      mojo_ipc::CpuTemperatureChannel channel;
      if (!label.empty())
        channel.label = label;
      channel.temperature_celsius = temperature;
      out_contents->push_back(channel.Clone());
    } else {
      LOG(WARNING) << "Unable to read CPU temp from "
                   << temperature_path.MaybeAsASCII();
    }
  }
  return has_data;
}

// Fetches and returns information about the device's CPU temperature channels.
std::vector<mojo_ipc::CpuTemperatureChannelPtr> GetCpuTemperatures(
    const base::FilePath& root_dir) {
  std::vector<mojo_ipc::CpuTemperatureChannelPtr> temps;
  // Get directories /sys/class/hwmon/hwmon*
  base::FileEnumerator hwmon_enumerator(root_dir.AppendASCII(kHwmonDir), false,
                                        base::FileEnumerator::DIRECTORIES,
                                        kHwmonDirectoryPattern);
  for (base::FilePath hwmon_path = hwmon_enumerator.Next(); !hwmon_path.empty();
       hwmon_path = hwmon_enumerator.Next()) {
    // Get temp*_input files in hwmon*/ and hwmon*/device/
    base::FilePath device_path = hwmon_path.Append(kDeviceDir);
    if (base::PathExists(device_path)) {
      // We might have hwmon*/device/, but sensor values are still in hwmon*/
      if (!ReadTemperatureSensorInfo(device_path, &temps)) {
        ReadTemperatureSensorInfo(hwmon_path, &temps);
      }
    } else {
      ReadTemperatureSensorInfo(hwmon_path, &temps);
    }
  }
  return temps;
}

// Gets the time spent in each C-state for the logical processor whose ID is
// |logical_id|. Returns std::nullopt if a required sysfs node was not found.
std::optional<std::vector<mojo_ipc::CpuCStateInfoPtr>> GetCStates(
    const base::FilePath& root_dir, int logical_id) {
  std::vector<mojo_ipc::CpuCStateInfoPtr> c_states;
  // Find all directories matching /sys/devices/system/cpu/cpuN/cpudidle/stateX.
  base::FileEnumerator c_state_it(
      GetCStateDirectoryPath(root_dir, logical_id), false,
      base::FileEnumerator::SHOW_SYM_LINKS | base::FileEnumerator::FILES |
          base::FileEnumerator::DIRECTORIES,
      kCStateDirectoryMatcher);
  for (base::FilePath c_state_dir = c_state_it.Next(); !c_state_dir.empty();
       c_state_dir = c_state_it.Next()) {
    mojo_ipc::CpuCStateInfo c_state;
    if (!ReadAndTrimString(c_state_dir, kCStateNameFileName, &c_state.name) ||
        !ReadInteger(c_state_dir, kCStateTimeFileName, &base::StringToUint64,
                     &c_state.time_in_state_since_last_boot_us)) {
      return std::nullopt;
    }
    c_states.push_back(c_state.Clone());
  }

  return c_states;
}

// Reads and parses the total number of threads available on the device. Returns
// an error if encountered, otherwise returns std::nullopt and populates
// |num_total_threads|.
std::optional<mojo_ipc::ProbeErrorPtr> GetNumTotalThreads(
    const base::FilePath& root_dir, uint32_t* num_total_threads) {
  DCHECK(num_total_threads);

  std::string cpu_present;
  auto cpu_dir = root_dir.Append(kRelativeCpuDir);
  if (!ReadAndTrimString(cpu_dir, kPresentFileName, &cpu_present)) {
    return CreateAndLogProbeError(mojo_ipc::ErrorType::kFileReadError,
                                  "Unable to read CPU present file: " +
                                      cpu_dir.Append(kPresentFileName).value());
  }

  // Two strings will be parsed directly from the regex, then converted to
  // uint32_t's. Expect |cpu_present| to contain the pattern "%d-%d", where the
  // first integer is strictly smaller than the second.
  std::string low_thread_num;
  std::string high_thread_num;
  uint32_t low_thread_int;
  uint32_t high_thread_int;
  if (!RE2::FullMatch(cpu_present, kPresentFileRegex, &low_thread_num,
                      &high_thread_num) ||
      !base::StringToUint(low_thread_num, &low_thread_int) ||
      !base::StringToUint(high_thread_num, &high_thread_int)) {
    return CreateAndLogProbeError(
        mojo_ipc::ErrorType::kParseError,
        "Unable to parse CPU present file: " + cpu_present);
  }

  DCHECK_GT(high_thread_int, low_thread_int);
  *num_total_threads = high_thread_int - low_thread_int + 1;
  return std::nullopt;
}

// Parses the contents of /proc/stat into a map of logical IDs to
// ParsedStatContents. Returns std::nullopt if an error was encountered while
// parsing.
std::optional<std::map<int, ParsedStatContents>> ParseStatContents(
    const std::string& stat_contents) {
  std::stringstream stat_sstream(stat_contents);

  // Ignore the first line, since it's aggregated data for the individual
  // logical CPUs.
  std::string line;
  std::getline(stat_sstream, line);

  // Parse lines of the format "cpu%d %d %d %d %d ...", where each line
  // corresponds to a separate logical CPU.
  std::map<int, ParsedStatContents> parsed_contents;
  int logical_cpu_id;
  std::string logical_cpu_id_str;
  std::string user_time_str;
  std::string system_time_str;
  std::string idle_time_str;
  while (std::getline(stat_sstream, line) &&
         RE2::PartialMatch(line, kRelativeStatFileRegex, &logical_cpu_id_str,
                           &user_time_str, &system_time_str, &idle_time_str)) {
    ParsedStatContents contents;
    if (!base::StringToUint64(user_time_str, &contents.user_time_user_hz) ||
        !base::StringToUint64(system_time_str, &contents.system_time_user_hz) ||
        !base::StringToUint64(idle_time_str, &contents.idle_time_user_hz) ||
        !base::StringToInt(logical_cpu_id_str, &logical_cpu_id)) {
      return std::nullopt;
    }
    DCHECK_EQ(parsed_contents.count(logical_cpu_id), 0);
    parsed_contents[logical_cpu_id] = std::move(contents);
  }

  return parsed_contents;
}

std::optional<std::map<int, ParsedStatContents>> GetParsedStatContents(
    const base::FilePath& root_dir) {
  std::string stat_contents;
  auto stat_file = GetProcStatPath(root_dir);
  if (!ReadFileToString(stat_file, &stat_contents)) {
    LOG(ERROR) << "Unable to read stat file: " << stat_file.value();
    return std::nullopt;
  }

  std::optional<std::map<int, ParsedStatContents>> parsed_stat_contents =
      ParseStatContents(stat_contents);
  if (!parsed_stat_contents.has_value()) {
    LOG(ERROR) << "Unable to parse stat contents: " << stat_contents;
    return std::nullopt;
  }
  return parsed_stat_contents;
}

std::optional<std::vector<std::string>> GetProcCpuInfoContent(
    const base::FilePath& root_dir) {
  std::string cpu_info_contents;
  auto cpu_info_file = GetProcCpuInfoPath(root_dir);
  if (!ReadFileToString(cpu_info_file, &cpu_info_contents)) {
    return std::nullopt;
  }

  return base::SplitStringUsingSubstr(cpu_info_contents, "\n\n",
                                      base::KEEP_WHITESPACE,
                                      base::SPLIT_WANT_NONEMPTY);
}

// Parses |block| to determine if the block parsed from /proc/cpuinfo is a
// processor block.
bool IsProcessorBlock(const std::string& block) {
  base::StringPairs pairs;
  base::SplitStringIntoKeyValuePairs(block, ':', '\n', &pairs);

  if (pairs.size() &&
      pairs[0].first.find(kProcessorIdKey) != std::string::npos) {
    return true;
  }

  return false;
}

// Parses |processor| to obtain |processor_id|, |physical_id|, |model_name| and
// |cpu_flags| if applicable. Returns true on success. |model_name| may be empty
// depending on the CPU architecture, and it is considered as success.
bool ParseProcessor(const std::string& processor,
                    int& processor_id,
                    int& physical_id,
                    std::string& model_name,
                    std::vector<std::string>& cpu_flags) {
  base::StringPairs pairs;
  base::SplitStringIntoKeyValuePairs(processor, ':', '\n', &pairs);
  std::string processor_id_str;
  std::string physical_id_str;
  bool flags_found = false;
  for (const auto& key_value : pairs) {
    if (key_value.first.find(kProcessorIdKey) != std::string::npos) {
      base::TrimWhitespaceASCII(key_value.second, base::TRIM_ALL,
                                &processor_id_str);
    } else if (key_value.first.find(kPhysicalIdKey) != std::string::npos) {
      base::TrimWhitespaceASCII(key_value.second, base::TRIM_ALL,
                                &physical_id_str);
    } else if (key_value.first.find(kModelNameKey) != std::string::npos) {
      base::TrimWhitespaceASCII(key_value.second, base::TRIM_ALL, &model_name);
    } else if (key_value.first.find(kX86CpuFlagsKey) != std::string::npos ||
               key_value.first.find(kArmCpuFlagsKey) != std::string::npos) {
      std::string cpu_flags_str;
      base::TrimWhitespaceASCII(key_value.second, base::TRIM_ALL,
                                &cpu_flags_str);
      cpu_flags = base::SplitString(cpu_flags_str, " ", base::TRIM_WHITESPACE,
                                    base::SPLIT_WANT_NONEMPTY);
      flags_found = true;
    }
  }

  // If the processor does not have a distinction between physical_id and
  // processor_id, make them the same value.
  if (!processor_id_str.empty() && physical_id_str.empty()) {
    physical_id_str = processor_id_str;
  }

  if (!base::StringToInt(physical_id_str, &physical_id)) {
    LOG(ERROR) << "physical id cannot be converted to integer: "
               << physical_id_str;
    return false;
  }

  if (!base::StringToInt(processor_id_str, &processor_id)) {
    LOG(ERROR) << "processor id cannot be converted to integer: "
               << processor_id_str;
    return false;
  }

  if (!flags_found) {
    LOG(ERROR) << "no cpu flags found";
    return false;
  }

  return true;
}

void ParseSocID(const base::FilePath& root_dir, std::string* model_name) {
  // Currently, only Mediatek and Qualcomm with newer kernel support this
  // feature.
  std::string content;
  base::FileEnumerator file_enum(
      root_dir.Append(kRelativeSoCDevicesDir), false,
      base::FileEnumerator::FileType::FILES |
          base::FileEnumerator::FileType::DIRECTORIES |
          base::FileEnumerator::FileType::SHOW_SYM_LINKS);
  for (auto path = file_enum.Next(); !path.empty(); path = file_enum.Next()) {
    if (!base::ReadFileToString(path.Append("soc_id"), &content)) {
      continue;
    }
    // The soc_id content should be "jep106:XXYY:ZZZZ".
    // XX represents identity code.
    // YY represents continuation code.
    // ZZZZ represents SoC ID.
    // We can use XXYY to distinguish vendor.
    //
    // https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-devices-soc
    const std::string kSoCIDPrefix = "jep106:";
    if (content.find(kSoCIDPrefix) != 0) {
      continue;
    }
    content.erase(0, kSoCIDPrefix.length());

    std::string vendor_id = content.substr(0, 4);
    std::string soc_id = content.substr(5, 4);
    // pair.first: Vendor ID.
    // pair.second: The string that we return from our API.
    const std::map<std::string, std::string> vendors{{"0426", "MediaTek"},
                                                     {"0070", "Qualcomm"}};
    auto vendor = vendors.find(vendor_id);
    if (vendor != vendors.end()) {
      *model_name = vendor->second + " " + soc_id;
    }
  }
}

void ParseCompatibleString(const base::FilePath& root_dir,
                           std::string* model_name) {
  std::string content;
  if (!base::ReadFileToString(root_dir.Append(kRelativeCompatibleFile),
                              &content)) {
    return;
  }

  // pair.first: Vendor string in compatible string.
  // pair.second: The string that we return from our API.
  const std::map<std::string, std::string> vendors{{"mediatek", "MediaTek"},
                                                   {"qualcomm", "Qualcomm"},
                                                   {"rockchip", "Rockchip"}};
  base::StringPairs pairs;
  base::SplitStringIntoKeyValuePairs(content, ',', '\0', &pairs);
  for (const auto& key_value : pairs) {
    auto vendor = vendors.find(key_value.first);
    if (vendor != vendors.end()) {
      *model_name = vendor->second + " " + key_value.second;
      return;
    }
  }
}

void GetArmSoCModelName(const base::FilePath& root_dir,
                        std::string* model_name) {
  ParseSocID(root_dir, model_name);
  if (!model_name->empty()) {
    return;
  }
  ParseCompatibleString(root_dir, model_name);
}

// Fetch Keylocker information.
std::optional<mojo_ipc::ProbeErrorPtr> FetchKeylockerInfo(
    const base::FilePath& root_dir,
    mojo_ipc::KeylockerInfoPtr* keylocker_info) {
  std::string file_contents;
  // Crypto file is common for all CPU architects. However, crypto algorithms
  // populated in crypto file could be hardware dependent.
  if (!ReadAndTrimString(root_dir, kRelativeCryptoFilePath, &file_contents)) {
    return CreateAndLogProbeError(
        mojo_ipc::ErrorType::kFileReadError,
        "Unable to read file: " +
            root_dir.Append(kRelativeCryptoFilePath).value());
  }
  // aeskl algorithm populated in crypto file is the indication that keylocker
  // driver had been loaded, the hardware had been configured and ready for use.
  std::size_t found = file_contents.find(kKeylockerAeskl);
  if (found == std::string::npos) {
    *keylocker_info = nullptr;
    return std::nullopt;
  }
  auto info = mojo_ipc::KeylockerInfo::New();
  info->keylocker_configured = true;
  *keylocker_info = std::move(info);

  return std::nullopt;
}

std::optional<VulnerabilityInfoMap> GetVulnerabilities(
    const base::FilePath& root_dir) {
  base::FilePath vulnerability_dir =
      root_dir.Append(kRelativeCpuDir).Append(kVulnerabilityDirName);
  // If vulnerabilities directory does not exist, this means the linux kernel
  // version does not support vulnerability detection yet and we will return an
  // empty map.
  std::vector<VulnerabilityInfoMap::value_type> vulnerabilities_vec;
  base::FileEnumerator it(vulnerability_dir,
                          /*recursive=*/false, base::FileEnumerator::FILES);
  for (base::FilePath vulnerability_file = it.Next();
       !vulnerability_file.empty(); vulnerability_file = it.Next()) {
    auto vulnerability = mojo_ipc::VulnerabilityInfo::New();

    if (!ReadAndTrimString(vulnerability_file, &vulnerability->message)) {
      return std::nullopt;
    }

    vulnerability->status =
        GetVulnerabilityStatusFromMessage(vulnerability->message);

    vulnerabilities_vec.push_back(
        {vulnerability_file.BaseName().value(), std::move(vulnerability)});
  }
  return VulnerabilityInfoMap(std::move(vulnerabilities_vec));
}

mojo_ipc::VirtualizationInfoPtr GetVirtualizationInfo(
    const base::FilePath& root_dir) {
  auto virtualization = mojo_ipc::VirtualizationInfo::New();

  virtualization->has_kvm_device =
      base::PathExists(root_dir.Append(kRelativeKvmFilePath));

  base::FilePath smt_dir = root_dir.Append(kRelativeCpuDir).Append(kSmtDirName);
  // If smt control directory does not exist, this means the linux kernel
  // version does not support smt and we mark it as kNotImplemented.
  if (!base::PathExists(smt_dir)) {
    virtualization->is_smt_active = false;
    virtualization->smt_control =
        mojo_ipc::VirtualizationInfo::SMTControl::kNotImplemented;
    return virtualization;
  }

  base::FilePath smt_active_path = smt_dir.Append(kSmtActiveFileName);

  uint32_t active;
  if (!ReadInteger(smt_active_path, base::StringToUint, &active) || active > 1)
    return nullptr;

  virtualization->is_smt_active = active == 1;

  std::string control;
  base::FilePath smt_control_path = smt_dir.Append(kSmtControlFileName);

  if (!ReadAndTrimString(smt_control_path, &control))
    return nullptr;

  if (control == kSmtControlOnContent) {
    virtualization->smt_control = mojo_ipc::VirtualizationInfo::SMTControl::kOn;
  } else if (control == kSmtControlOffContent) {
    virtualization->smt_control =
        mojo_ipc::VirtualizationInfo::SMTControl::kOff;
  } else if (control == kSmtControlForceOffContent) {
    virtualization->smt_control =
        mojo_ipc::VirtualizationInfo::SMTControl::kForceOff;
  } else if (control == kSmtControlNotSupportedContent) {
    virtualization->smt_control =
        mojo_ipc::VirtualizationInfo::SMTControl::kNotSupported;
  } else if (control == kSmtControlNotImplementedContent) {
    virtualization->smt_control =
        mojo_ipc::VirtualizationInfo::SMTControl::kNotImplemented;
  } else {
    return nullptr;
  }

  return virtualization;
}
}  // namespace

mojo_ipc::CpuResultPtr CpuFetcher::GetCpuInfoFromProcessorInfo() {
  base::FilePath root_dir = context_->root_dir();

  std::optional<std::map<int, ParsedStatContents>> parsed_stat_contents =
      GetParsedStatContents(root_dir);
  if (parsed_stat_contents == std::nullopt) {
    return mojo_ipc::CpuResult::NewError(CreateAndLogProbeError(
        mojo_ipc::ErrorType::kParseError,
        "Unable to parse stat file: " + GetProcStatPath(root_dir).value()));
  }

  std::map<int, ParsedStatContents> logical_ids_to_stat_contents =
      parsed_stat_contents.value();

  std::optional<std::vector<std::string>> processor_info_opt =
      GetProcCpuInfoContent(root_dir);
  if (processor_info_opt == std::nullopt) {
    return mojo_ipc::CpuResult::NewError(
        CreateAndLogProbeError(mojo_ipc::ErrorType::kFileReadError,
                               "Unable to read CPU info file: " +
                                   GetProcCpuInfoPath(root_dir).value()));
  }
  const std::vector<std::string>& processor_info = processor_info_opt.value();

  std::map<int, mojo_ipc::PhysicalCpuInfoPtr> physical_cpus;
  for (const auto& processor : processor_info) {
    if (!IsProcessorBlock(processor))
      continue;

    int processor_id;
    int physical_id;
    std::string model_name;
    std::vector<std::string> cpu_flags;
    if (!ParseProcessor(processor, processor_id, physical_id, model_name,
                        cpu_flags)) {
      return mojo_ipc::CpuResult::NewError(CreateAndLogProbeError(
          mojo_ipc::ErrorType::kParseError,
          "Unable to parse processor string: " + processor));
    }

    // Find the physical CPU corresponding to this logical CPU, if it already
    // exists. If not, make one.
    auto itr = physical_cpus.find(physical_id);
    if (itr == physical_cpus.end()) {
      mojo_ipc::PhysicalCpuInfoPtr physical_cpu =
          mojo_ipc::PhysicalCpuInfo::New();
      if (model_name.empty()) {
        // It may be Arm CPU. We will return SoC model name instead.
        GetArmSoCModelName(root_dir, &model_name);
      }
      if (!model_name.empty())
        physical_cpu->model_name = std::move(model_name);

      physical_cpu->flags = std::move(cpu_flags);

      const auto result =
          physical_cpus.insert({physical_id, std::move(physical_cpu)});
      DCHECK(result.second);
      itr = result.first;
    }

    // Populate the logical CPU info values.
    mojo_ipc::LogicalCpuInfo logical_cpu;
    const auto parsed_stat_itr =
        logical_ids_to_stat_contents.find(processor_id);
    if (parsed_stat_itr == logical_ids_to_stat_contents.end()) {
      return mojo_ipc::CpuResult::NewError(
          CreateAndLogProbeError(mojo_ipc::ErrorType::kParseError,
                                 "No parsed stat contents for logical ID: " +
                                     std::to_string(processor_id)));
    }
    logical_cpu.user_time_user_hz = parsed_stat_itr->second.user_time_user_hz;
    logical_cpu.system_time_user_hz =
        parsed_stat_itr->second.system_time_user_hz;
    logical_cpu.idle_time_user_hz = parsed_stat_itr->second.idle_time_user_hz;

    auto c_states = GetCStates(root_dir, processor_id);
    if (c_states == std::nullopt) {
      return mojo_ipc::CpuResult::NewError(CreateAndLogProbeError(
          mojo_ipc::ErrorType::kFileReadError, "Unable to read C States."));
    }
    logical_cpu.c_states = std::move(c_states.value());

    auto cpufreq_dir = GetCpuFreqDirectoryPath(root_dir, processor_id);
    if (!ReadInteger(cpufreq_dir, kCpuinfoMaxFreqFileName, &base::StringToUint,
                     &logical_cpu.max_clock_speed_khz)) {
      return mojo_ipc::CpuResult::NewError(CreateAndLogProbeError(
          mojo_ipc::ErrorType::kFileReadError,
          "Unable to read max CPU frequency file to integer: " +
              cpufreq_dir.Append(kCpuinfoMaxFreqFileName).value()));
    }

    if (!ReadInteger(cpufreq_dir, kScalingMaxFreqFileName, &base::StringToUint,
                     &logical_cpu.scaling_max_frequency_khz)) {
      return mojo_ipc::CpuResult::NewError(CreateAndLogProbeError(
          mojo_ipc::ErrorType::kFileReadError,
          "Unable to read scaling max frequency file to integer: " +
              cpufreq_dir.Append(kScalingMaxFreqFileName).value()));
    }

    if (!ReadInteger(cpufreq_dir, kScalingCurFreqFileName, &base::StringToUint,
                     &logical_cpu.scaling_current_frequency_khz)) {
      return mojo_ipc::CpuResult::NewError(CreateAndLogProbeError(
          mojo_ipc::ErrorType::kFileReadError,
          "Unable to read scaling current frequency file to integer: " +
              cpufreq_dir.Append(kScalingCurFreqFileName).value()));
    }

    // Add this logical CPU to the corresponding physical CPU.
    itr->second->logical_cpus.push_back(logical_cpu.Clone());
  }

  // Populate the final CpuInfo struct.
  mojo_ipc::CpuInfo cpu_info;
  auto thread_error = GetNumTotalThreads(root_dir, &cpu_info.num_total_threads);
  if (thread_error != std::nullopt) {
    return mojo_ipc::CpuResult::NewError(std::move(thread_error.value()));
  }

  cpu_info.architecture = GetArchitecture();
  auto& keylocker_info = cpu_info.keylocker_info;
  auto error = FetchKeylockerInfo(root_dir, &keylocker_info);
  if (error.has_value())
    return mojo_ipc::CpuResult::NewError(std::move(error.value()));

  for (const auto& temperature : GetCpuTemperatures(root_dir))
    cpu_info.temperature_channels.push_back(temperature.Clone());

  for (auto& key_value : physical_cpus) {
    cpu_info.physical_cpus.push_back(key_value.second.Clone());
  }

  cpu_info.virtualization = GetVirtualizationInfo(root_dir);
  if (cpu_info.virtualization.is_null()) {
    return mojo_ipc::CpuResult::NewError(
        CreateAndLogProbeError(mojo_ipc::ErrorType::kFileReadError,
                               "Unable to read Virtualization Information."));
  }

  cpu_info.vulnerabilities = GetVulnerabilities(root_dir);
  if (!cpu_info.vulnerabilities.has_value()) {
    return mojo_ipc::CpuResult::NewError(
        CreateAndLogProbeError(mojo_ipc::ErrorType::kFileReadError,
                               "Unable to read vulnerabilities."));
  }

  return mojo_ipc::CpuResult::NewCpuInfo(cpu_info.Clone());
}

base::FilePath GetCStateDirectoryPath(const base::FilePath& root_dir,
                                      int logical_id) {
  std::string logical_cpu_dir = "cpu" + std::to_string(logical_id);
  std::string cpuidle_dirname = "cpuidle";
  return root_dir.Append(kRelativeCpuDir)
      .Append(logical_cpu_dir)
      .Append(cpuidle_dirname);
}

// If the CPU has a governing policy, return that path, otherwise return the
// cpufreq directory for the given logical CPU.
base::FilePath GetCpuFreqDirectoryPath(const base::FilePath& root_dir,
                                       int logical_id) {
  std::string cpufreq_policy_dir =
      "cpufreq/policy" + std::to_string(logical_id);

  auto policy_path =
      root_dir.Append(kRelativeCpuDir).Append(cpufreq_policy_dir);
  if (base::PathExists(policy_path)) {
    return policy_path;
  }

  std::string logical_cpu_dir = "cpu" + std::to_string(logical_id);
  std::string cpufreq_dirname = "cpufreq";
  return root_dir.Append(kRelativeCpuDir)
      .Append(logical_cpu_dir)
      .Append(cpufreq_dirname);
}

mojo_ipc::VulnerabilityInfo::Status GetVulnerabilityStatusFromMessage(
    const std::string& message) {
  // Messages in the |iTLB multihit| vulnerability takes a different form with
  // |KVM: Vulberable|, |KVM: Mitigation: $msg| and |Processor vulnerable|. We
  // remove prefix to convert the data to common form in order to parse the
  // status correctly.
  //
  // https://www.kernel.org/doc/html/latest/admin-guide/hw-vuln/multihit.html
  std::string message_no_prefix = message;
  if (base::StartsWith(message, kKvmPrefix)) {
    message_no_prefix = message.substr(sizeof(kKvmPrefix) - 1);
  }

  if (message_no_prefix == kNotAffectedPattern) {
    return mojo_ipc::VulnerabilityInfo::Status::kNotAffected;
  }
  if (base::StartsWith(message_no_prefix, kVulnerablePattern) ||
      message_no_prefix == kProcessorVulnerablePattern) {
    return mojo_ipc::VulnerabilityInfo::Status::kVulnerable;
  }
  if (base::StartsWith(message_no_prefix, kMitigationPattern)) {
    return mojo_ipc::VulnerabilityInfo::Status::kMitigation;
  }
  if (base::StartsWith(message_no_prefix, kUnknownPattern)) {
    return mojo_ipc::VulnerabilityInfo::Status::kUnknown;
  }
  return mojo_ipc::VulnerabilityInfo::Status::kUnrecognized;
}

void CpuFetcher::HandleCallbackComplete(bool all_callback_called) {
  if (!all_callback_called) {
    LogAndSetError(mojo_ipc::ErrorType::kServiceUnavailable,
                   "Not all Fetch Cpu Virtualization Callbacks "
                   "have been sucessfully called");
  }
  if (!error_.is_null()) {
    std::move(callback_).Run(mojo_ipc::CpuResult::NewError(std::move(error_)));
    return;
  }
  std::move(callback_).Run(
      mojo_ipc::CpuResult::NewCpuInfo(std::move(cpu_info_)));
}

void CpuFetcher::LogAndSetError(chromeos::cros_healthd::mojom::ErrorType type,
                                const std::string& message) {
  LOG(ERROR) << message;
  if (error_.is_null())
    error_ = chromeos::cros_healthd::mojom::ProbeError::New(type, message);
}

void CpuFetcher::FetchImpl(ResultCallback callback) {
  callback_ = std::move(callback);

  CallbackBarrier barrier{
      base::BindOnce(&CpuFetcher::HandleCallbackComplete,
                     weak_factory_.GetWeakPtr(), /*all_callback_called=*/true),
      base::BindOnce(&CpuFetcher::HandleCallbackComplete,
                     weak_factory_.GetWeakPtr(),
                     /*all_callback_called=*/false)};

  mojo_ipc::CpuResultPtr cpu_result = GetCpuInfoFromProcessorInfo();
  if (cpu_result->is_error()) {
    // TODO(b/230046339): Use LogAndSetError after refactor
    // GetCpuInfoFromProcessorInfo.
    error_ = std::move(cpu_result->get_error());
    return;
  }
  cpu_info_ = std::move(cpu_result->get_cpu_info());
  return;
}

mojo_ipc::CpuArchitectureEnum CpuFetcher::GetArchitecture() {
  struct utsname buf;
  if (context_->system_utils()->Uname(&buf))
    return mojo_ipc::CpuArchitectureEnum::kUnknown;

  std::stringstream ss;
  ss << buf.machine;
  std::string machine = ss.str();
  if (machine == kUnameMachineX86_64)
    return mojo_ipc::CpuArchitectureEnum::kX86_64;
  else if (machine == kUnameMachineAArch64)
    return mojo_ipc::CpuArchitectureEnum::kAArch64;
  else if (machine == kUnameMachineArmv7l)
    return mojo_ipc::CpuArchitectureEnum::kArmv7l;

  return mojo_ipc::CpuArchitectureEnum::kUnknown;
}

}  // namespace diagnostics
