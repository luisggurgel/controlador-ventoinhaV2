#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nitrosense {

inline constexpr const char* MODEL_NAME = "Acer Nitro V 15 (ANV15-52-52VN)";
inline constexpr const char* DMI_PRODUCT_NAME = "Nitro ANV15-52";
inline constexpr const char* CPU_SPEC_NAME = "Intel Core i5-13420H (8C/12T, 4.60 GHz)";
inline constexpr const char* GPU_SPEC_NAME = "NVIDIA GeForce RTX 4060 Laptop GPU (8 GB GDDR6)";
inline constexpr const char* RAM_SPEC_NAME = "16 GB DDR5 5200 MHz";
inline constexpr const char* DISPLAY_SPEC_NAME = "15.6\" IPS FHD 165 Hz";
inline constexpr const char* BATTERY_SPEC_NAME = "57 Wh Li-Ion (AC 135W)";
inline constexpr const char* EC_QUIRK_NAME = "quirk_acer_nitro_anv15_52";
inline constexpr const char* EC_WINDOW_BASE = "0xFE708000";
inline constexpr const char* EC_PROFILE_OFFSET = "0x607";
inline constexpr const char* EC_ACPI_METHOD = "\\_SB.PC00.LPCB.EC0._Q30";

inline constexpr int MAX_CPU_RPM = 8600;
inline constexpr int MAX_GPU_RPM = 8600;
inline constexpr int IDLE_FAN_RPM_FLOOR = 1800;

struct FanCurvePoint {
  int temp;
  int speed;
};

enum class FanMode { Auto, Custom, Max };

inline const char* fan_mode_to_str(FanMode m) {
  switch (m) {
    case FanMode::Auto:
      return "auto";
    case FanMode::Custom:
      return "custom";
    case FanMode::Max:
      return "max";
  }
  return "auto";
}

struct CpuSnapshot {
  std::string model_name;
  int core_count = 0;
  std::optional<double> total_usage_pct;
  std::vector<double> core_usages_pct;
  std::vector<int> core_freqs_mhz;
  double load1 = 0.0;
  double load5 = 0.0;
  double load15 = 0.0;
  uint64_t uptime_sec = 0;
  std::optional<double> temp_c;
  std::optional<double> power_watts;
  std::string temp_source;
};

struct MemorySnapshot {
  uint64_t total_kb = 0;
  uint64_t available_kb = 0;
  uint64_t used_kb = 0;
  uint64_t cached_kb = 0;
  uint64_t buffers_kb = 0;
  uint64_t swap_total_kb = 0;
  uint64_t swap_used_kb = 0;
};

struct GpuSnapshot {
  std::string name;
  bool nvidia_smi_available = false;
  std::optional<double> temp_c;
  std::optional<double> usage_pct;
  std::optional<double> power_watts;
  std::optional<int> vram_used_mb;
  std::optional<int> vram_total_mb;
  std::optional<int> clock_mhz;
  std::string source;
};

struct FanSnapshot {
  std::optional<int> cpu_rpm;
  std::optional<int> gpu_rpm;
  std::optional<int> cpu_target_pct;
  std::optional<int> gpu_target_pct;
  FanMode mode = FanMode::Auto;
  std::string rpm_source;
};

struct PowerSnapshot {
  std::optional<bool> ac_connected;
  std::optional<int> battery_pct;
  std::optional<bool> battery_charging;
  std::string battery_status;
  std::optional<bool> battery_limiter_active;
};

struct HardwareSettingsSnapshot {
  std::optional<bool> battery_limiter;
  std::optional<bool> lcd_overdrive;
  std::optional<bool> keyboard_backlight_timeout;
  std::optional<int> usb_power_off_charging;
};

struct SystemPaths {
  std::string sense_dir;
  std::string fan_speed_path;
  std::string platform_profile_path;
  std::string platform_profile_choices_path;
  std::string hwmon_acer_dir;
  std::string hwmon_cpu_dir;
  std::string battery_limiter_path;
  std::string lcd_override_path;
  std::string backlight_timeout_path;
  std::string usb_charging_path;
  std::string battery_calibration_path;
};

struct ProcessInfo {
  int pid = 0;
  std::string user;
  char state = '?';
  double cpu_pct = 0.0;
  double mem_pct = 0.0;
  double mem_mb = 0.0;
  int threads = 1;
  std::string command;
};

struct TelemetrySnapshot {
  int64_t timestamp_ms = 0;
  std::string hostname;
  std::string kernel_release;
  std::string os_name;
  std::string dmi_product_name;
  std::optional<std::string> active_profile;
  std::string active_profile_raw;
  std::vector<std::string> platform_profile_choices;
  bool is_hardware_detected = false;
  bool has_real_sensors = false;
  std::string driver_status;  // "active" | "partial_linux_sensors" | "no_acer_ec_hardware"
  CpuSnapshot cpu;
  MemorySnapshot memory;
  GpuSnapshot gpu;
  FanSnapshot fans;
  PowerSnapshot power;
  HardwareSettingsSnapshot settings;
  SystemPaths paths;
  std::vector<ProcessInfo> processes;
};

struct FanPair {
  int cpu = 0;
  int gpu = 0;
};

std::string map_ec_mode_to_profile(int mode);
std::string map_platform_profile_to_id(const std::string& raw);
std::string profile_id_to_platform_profile(const std::string& profile_id);
std::string format_fan_speed_sysfs(int cpu_pct, int gpu_pct);
FanPair parse_fan_speed_sysfs(const std::string& raw);
int calculate_rpm_from_percent(int percent);
int interpolate_fan_curve(double temp_c, const std::vector<FanCurvePoint>& curve);
std::string json_escape(const std::string& s);
std::string snapshot_to_json(const TelemetrySnapshot& snap);

}  // namespace nitrosense
