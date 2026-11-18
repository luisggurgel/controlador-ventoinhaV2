#include "nitro_types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace nitrosense {

namespace {

std::string trim_copy(const std::string& input) {
  size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }
  size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return input.substr(start, end - start);
}

std::string to_lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

template <typename T>
void write_opt_num(std::ostringstream& os, const std::optional<T>& val) {
  if (val.has_value()) {
    os << *val;
  } else {
    os << "null";
  }
}

void write_opt_bool(std::ostringstream& os, const std::optional<bool>& val) {
  if (val.has_value()) {
    os << (*val ? "true" : "false");
  } else {
    os << "null";
  }
}

void write_opt_str(std::ostringstream& os, const std::optional<std::string>& val) {
  if (val.has_value()) {
    os << "\"" << json_escape(*val) << "\"";
  } else {
    os << "null";
  }
}

void write_nullable_str(std::ostringstream& os, const std::string& s) {
  if (s.empty()) {
    os << "null";
  } else {
    os << "\"" << json_escape(s) << "\"";
  }
}

}  // namespace

std::string map_ec_mode_to_profile(int mode) {
  switch (mode) {
    case 0:
      return "turbo";
    case 1:
      return "performance";
    case 2:
      return "balanced";
    case 3:
      return "quiet";
    case 4:
      return "eco";
    default:
      return "balanced";
  }
}

std::string map_platform_profile_to_id(const std::string& raw) {
  const std::string clean = to_lower_copy(trim_copy(raw));
  if (clean == "low-power") return "eco";
  if (clean == "quiet") return "quiet";
  if (clean == "balanced") return "balanced";
  if (clean == "balanced-performance") return "performance";
  if (clean == "performance") return "turbo";
  return "balanced";
}

std::string profile_id_to_platform_profile(const std::string& profile_id) {
  const std::string clean = to_lower_copy(trim_copy(profile_id));
  if (clean == "eco" || clean == "low-power") return "low-power";
  if (clean == "quiet") return "quiet";
  if (clean == "balanced") return "balanced";
  if (clean == "performance" || clean == "balanced-performance") return "balanced-performance";
  if (clean == "turbo") return "performance";
  return "";
}

std::string format_fan_speed_sysfs(int cpu_pct, int gpu_pct) {
  auto clamp = [](int v) {
    if (v <= 0) return 0;
    if (v >= 100) return 100;
    return v;
  };
  return std::to_string(clamp(cpu_pct)) + "," + std::to_string(clamp(gpu_pct));
}

FanPair parse_fan_speed_sysfs(const std::string& raw) {
  FanPair result{0, 0};
  const std::string clean = trim_copy(raw);
  if (clean.empty()) return result;
  const size_t comma = clean.find(',');
  if (comma == std::string::npos) return result;
  try {
    int c = std::stoi(clean.substr(0, comma));
    int g = std::stoi(clean.substr(comma + 1));
    result.cpu = std::clamp(c, 0, 100);
    result.gpu = std::clamp(g, 0, 100);
  } catch (...) {
    return {0, 0};
  }
  return result;
}

int calculate_rpm_from_percent(int percent) {
  if (percent <= 0) return 0;
  const int clamped = std::clamp(percent, 0, 100);
  const double ratio = static_cast<double>(clamped) / 100.0;
  return static_cast<int>(std::lround(IDLE_FAN_RPM_FLOOR + ratio * (MAX_CPU_RPM - IDLE_FAN_RPM_FLOOR)));
}

int interpolate_fan_curve(double temp_c, const std::vector<FanCurvePoint>& curve) {
  if (curve.empty()) return 0;
  if (curve.size() == 1) return std::clamp(curve[0].speed, 0, 100);

  std::vector<FanCurvePoint> sorted = curve;
  std::sort(sorted.begin(), sorted.end(),
            [](const FanCurvePoint& a, const FanCurvePoint& b) { return a.temp < b.temp; });

  if (temp_c <= sorted.front().temp) {
    return std::clamp(sorted.front().speed, 0, 100);
  }
  if (temp_c >= sorted.back().temp) {
    return std::clamp(sorted.back().speed, 0, 100);
  }

  for (size_t i = 0; i + 1 < sorted.size(); ++i) {
    const auto& p1 = sorted[i];
    const auto& p2 = sorted[i + 1];
    if (temp_c >= p1.temp && temp_c <= p2.temp) {
      if (p2.temp == p1.temp) return std::clamp(p1.speed, 0, 100);
      const double t = (temp_c - p1.temp) / static_cast<double>(p2.temp - p1.temp);
      const double val = p1.speed + t * (p2.speed - p1.speed);
      return std::clamp(static_cast<int>(std::lround(val)), 0, 100);
    }
  }
  return std::clamp(sorted.back().speed, 0, 100);
}

std::string json_escape(const std::string& s) {
  std::ostringstream os;
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        os << "\\\"";
        break;
      case '\\':
        os << "\\\\";
        break;
      case '\b':
        os << "\\b";
        break;
      case '\f':
        os << "\\f";
        break;
      case '\n':
        os << "\\n";
        break;
      case '\r':
        os << "\\r";
        break;
      case '\t':
        os << "\\t";
        break;
      default:
        if (c < 0x20) {
          os << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c)
             << std::dec;
        } else {
          os << static_cast<char>(c);
        }
        break;
    }
  }
  return os.str();
}

std::string snapshot_to_json(const TelemetrySnapshot& snap) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(1);
  os << "{";
  os << "\"timestamp\":" << snap.timestamp_ms << ",";
  os << "\"hostname\":\"" << json_escape(snap.hostname) << "\",";
  os << "\"kernel\":\"" << json_escape(snap.kernel_release) << "\",";
  os << "\"osName\":\"" << json_escape(snap.os_name) << "\",";
  os << "\"deviceModel\":\"" << json_escape(MODEL_NAME) << "\",";
  os << "\"dmiProductName\":";
  write_nullable_str(os, snap.dmi_product_name);
  os << ",";
  os << "\"activeProfile\":";
  write_opt_str(os, snap.active_profile);
  os << ",";
  os << "\"activeProfileRaw\":";
  write_nullable_str(os, snap.active_profile_raw);
  os << ",";

  os << "\"platformProfileChoices\":[";
  for (size_t i = 0; i < snap.platform_profile_choices.size(); ++i) {
    if (i > 0) os << ",";
    os << "\"" << json_escape(snap.platform_profile_choices[i]) << "\"";
  }
  os << "],";

  os << "\"isHardwareDetected\":" << (snap.is_hardware_detected ? "true" : "false") << ",";
  os << "\"hasRealSensors\":" << (snap.has_real_sensors ? "true" : "false") << ",";
  os << "\"driverStatus\":\"" << json_escape(snap.driver_status) << "\",";

  // Top-level convenience fields kept unambiguous (null when missing!)
  os << "\"cpuTemp\":";
  write_opt_num(os, snap.cpu.temp_c);
  os << ",";
  os << "\"gpuTemp\":";
  write_opt_num(os, snap.gpu.temp_c);
  os << ",";
  os << "\"cpuFanRpm\":";
  write_opt_num(os, snap.fans.cpu_rpm);
  os << ",";
  os << "\"gpuFanRpm\":";
  write_opt_num(os, snap.fans.gpu_rpm);
  os << ",";
  os << "\"cpuFanPercent\":";
  write_opt_num(os, snap.fans.cpu_target_pct);
  os << ",";
  os << "\"gpuFanPercent\":";
  write_opt_num(os, snap.fans.gpu_target_pct);
  os << ",";
  os << "\"fanMode\":\"" << fan_mode_to_str(snap.fans.mode) << "\",";
  os << "\"cpuUsage\":";
  write_opt_num(os, snap.cpu.total_usage_pct);
  os << ",";
  os << "\"gpuUsage\":";
  write_opt_num(os, snap.gpu.usage_pct);
  os << ",";
  os << "\"cpuPowerWatts\":";
  write_opt_num(os, snap.cpu.power_watts);
  os << ",";
  os << "\"gpuPowerWatts\":";
  write_opt_num(os, snap.gpu.power_watts);
  os << ",";
  os << "\"acConnected\":";
  write_opt_bool(os, snap.power.ac_connected);
  os << ",";
  os << "\"batteryLevel\":";
  write_opt_num(os, snap.power.battery_pct);
  os << ",";
  os << "\"batteryCharging\":";
  write_opt_bool(os, snap.power.battery_charging);
  os << ",";
  os << "\"batteryLimiterActive\":";
  write_opt_bool(os, snap.power.battery_limiter_active);
  os << ",";

  // Detailed CPU block
  os << "\"cpu\":{";
  os << "\"modelName\":\"" << json_escape(snap.cpu.model_name) << "\",";
  os << "\"coreCount\":" << snap.cpu.core_count << ",";
  os << "\"totalUsagePct\":";
  write_opt_num(os, snap.cpu.total_usage_pct);
  os << ",";
  os << "\"coreUsagesPct\":[";
  for (size_t i = 0; i < snap.cpu.core_usages_pct.size(); ++i) {
    if (i > 0) os << ",";
    os << snap.cpu.core_usages_pct[i];
  }
  os << "],";
  os << "\"coreFreqsMhz\":[";
  for (size_t i = 0; i < snap.cpu.core_freqs_mhz.size(); ++i) {
    if (i > 0) os << ",";
    os << snap.cpu.core_freqs_mhz[i];
  }
  os << "],";
  os << std::setprecision(2);
  os << "\"loadAvg\":[" << snap.cpu.load1 << "," << snap.cpu.load5 << "," << snap.cpu.load15 << "],";
  os << std::setprecision(1);
  os << "\"uptimeSec\":" << snap.cpu.uptime_sec << ",";
  os << "\"tempC\":";
  write_opt_num(os, snap.cpu.temp_c);
  os << ",";
  os << "\"powerWatts\":";
  write_opt_num(os, snap.cpu.power_watts);
  os << ",";
  os << "\"tempSource\":";
  write_nullable_str(os, snap.cpu.temp_source);
  os << "},";

  // Memory block
  os << "\"memory\":{";
  os << "\"totalKb\":" << snap.memory.total_kb << ",";
  os << "\"availableKb\":" << snap.memory.available_kb << ",";
  os << "\"usedKb\":" << snap.memory.used_kb << ",";
  os << "\"cachedKb\":" << snap.memory.cached_kb << ",";
  os << "\"buffersKb\":" << snap.memory.buffers_kb << ",";
  os << "\"swapTotalKb\":" << snap.memory.swap_total_kb << ",";
  os << "\"swapUsedKb\":" << snap.memory.swap_used_kb << "},";

  // GPU block
  os << "\"gpu\":{";
  os << "\"name\":\"" << json_escape(snap.gpu.name) << "\",";
  os << "\"nvidiaSmiAvailable\":" << (snap.gpu.nvidia_smi_available ? "true" : "false") << ",";
  os << "\"tempC\":";
  write_opt_num(os, snap.gpu.temp_c);
  os << ",";
  os << "\"usagePct\":";
  write_opt_num(os, snap.gpu.usage_pct);
  os << ",";
  os << "\"powerWatts\":";
  write_opt_num(os, snap.gpu.power_watts);
  os << ",";
  os << "\"vramUsedMb\":";
  write_opt_num(os, snap.gpu.vram_used_mb);
  os << ",";
  os << "\"vramTotalMb\":";
  write_opt_num(os, snap.gpu.vram_total_mb);
  os << ",";
  os << "\"clockMhz\":";
  write_opt_num(os, snap.gpu.clock_mhz);
  os << ",";
  os << "\"source\":";
  write_nullable_str(os, snap.gpu.source);
  os << "},";

  // Fans block
  os << "\"fans\":{";
  os << "\"cpuRpm\":";
  write_opt_num(os, snap.fans.cpu_rpm);
  os << ",";
  os << "\"gpuRpm\":";
  write_opt_num(os, snap.fans.gpu_rpm);
  os << ",";
  os << "\"cpuTargetPct\":";
  write_opt_num(os, snap.fans.cpu_target_pct);
  os << ",";
  os << "\"gpuTargetPct\":";
  write_opt_num(os, snap.fans.gpu_target_pct);
  os << ",";
  os << "\"maxRpm\":" << MAX_CPU_RPM << ",";
  os << "\"mode\":\"" << fan_mode_to_str(snap.fans.mode) << "\",";
  os << "\"rpmSource\":";
  write_nullable_str(os, snap.fans.rpm_source);
  os << "},";

  // Power block
  os << "\"power\":{";
  os << "\"acConnected\":";
  write_opt_bool(os, snap.power.ac_connected);
  os << ",";
  os << "\"batteryPct\":";
  write_opt_num(os, snap.power.battery_pct);
  os << ",";
  os << "\"batteryCharging\":";
  write_opt_bool(os, snap.power.battery_charging);
  os << ",";
  os << "\"batteryStatus\":";
  write_nullable_str(os, snap.power.battery_status);
  os << ",";
  os << "\"batteryLimiterActive\":";
  write_opt_bool(os, snap.power.battery_limiter_active);
  os << "},";

  // Hardware Settings read from sysfs
  os << "\"settings\":{";
  os << "\"batteryLimiter\":";
  write_opt_bool(os, snap.settings.battery_limiter);
  os << ",";
  os << "\"lcdOverdrive\":";
  write_opt_bool(os, snap.settings.lcd_overdrive);
  os << ",";
  os << "\"keyboardBacklightTimeout\":";
  write_opt_bool(os, snap.settings.keyboard_backlight_timeout);
  os << ",";
  os << "\"usbPowerOffCharging\":";
  write_opt_num(os, snap.settings.usb_power_off_charging);
  os << "},";

  // Sysfs paths block
  os << "\"sysfsPaths\":{";
  os << "\"sensePath\":";
  write_nullable_str(os, snap.paths.sense_dir);
  os << ",";
  os << "\"fanSpeedPath\":";
  write_nullable_str(os, snap.paths.fan_speed_path);
  os << ",";
  os << "\"platformProfilePath\":";
  write_nullable_str(os, snap.paths.platform_profile_path);
  os << ",";
  os << "\"batteryLimiterPath\":";
  write_nullable_str(os, snap.paths.battery_limiter_path);
  os << ",";
  os << "\"hwmonAcerDir\":";
  write_nullable_str(os, snap.paths.hwmon_acer_dir);
  os << ",";
  os << "\"hwmonCpuDir\":";
  write_nullable_str(os, snap.paths.hwmon_cpu_dir);
  os << "},";

  // Real Linux Processes
  os << "\"processes\":[";
  for (size_t i = 0; i < snap.processes.size(); ++i) {
    const auto& p = snap.processes[i];
    if (i > 0) os << ",";
    os << "{";
    os << "\"pid\":" << p.pid << ",";
    os << "\"user\":\"" << json_escape(p.user) << "\",";
    os << "\"state\":\"" << p.state << "\",";
    os << "\"cpuPct\":" << p.cpu_pct << ",";
    os << "\"memPct\":" << p.mem_pct << ",";
    os << "\"memMb\":" << p.mem_mb << ",";
    os << "\"threads\":" << p.threads << ",";
    os << "\"command\":\"" << json_escape(p.command) << "\"";
    os << "}";
  }
  os << "]";

  os << "}";
  return os.str();
}

}  // namespace nitrosense
