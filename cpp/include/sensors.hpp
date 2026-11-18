#pragma once

#include "nitro_types.hpp"

#include <chrono>
#include <map>
#include <string>

namespace nitrosense {

struct EnvOverrides {
  std::string proc_dir = "/proc";
  std::string hwmon_dir = "/sys/class/hwmon";
  std::string power_supply_dir = "/sys/class/power_supply";
  std::string platform_profile_path = "/sys/firmware/acpi/platform_profile";
  std::string platform_profile_choices_path = "/sys/firmware/acpi/platform_profile_choices";
  std::string rapl_energy_path = "/sys/class/powercap/intel-rapl:0/energy_uj";
  std::string dmi_product_path = "/sys/class/dmi/id/product_name";
  std::string os_release_path = "/etc/os-release";
  std::string sense_dir_override;
  bool disable_nvidia_smi = false;
  bool allow_sudo = true;
};

EnvOverrides load_env_overrides();
SystemPaths detect_system_paths(const EnvOverrides& env = load_env_overrides());
std::optional<std::string> read_sysfs_trimmed(const std::string& file_path);

class TelemetryCollector {
 public:
  explicit TelemetryCollector(EnvOverrides env = load_env_overrides());

  TelemetrySnapshot collect(int process_limit = 40);
  void prime_cpu_sample();

 private:
  struct CpuTickSample {
    uint64_t idle = 0;
    uint64_t total = 0;
  };

  struct ProcTickSample {
    uint64_t utime_stime = 0;
  };

  EnvOverrides env_;
  std::vector<CpuTickSample> prev_cpu_ticks_;
  uint64_t prev_total_jiffies_ = 0;
  std::map<int, ProcTickSample> prev_proc_ticks_;
  std::optional<uint64_t> prev_rapl_uj_;
  std::chrono::steady_clock::time_point prev_rapl_time_;
  int cpu_core_count_cache_ = 1;

  std::map<int, std::string> uid_user_cache_;

  void read_cpu_and_load(CpuSnapshot& out);
  void read_memory(MemorySnapshot& out);
  void read_processes(std::vector<ProcessInfo>& out, uint64_t total_mem_kb, int limit);
  std::string resolve_username(int uid);
};

}  // namespace nitrosense
