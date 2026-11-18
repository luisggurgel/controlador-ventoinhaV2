#include "control.hpp"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace nitrosense {

namespace {

volatile sig_atomic_t g_daemon_running = 1;

void daemon_sig_handler(int) {
  g_daemon_running = 0;
}

std::string trim_copy(const std::string& input) {
  size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) ++start;
  size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
  return input.substr(start, end - start);
}

bool is_safe_sysfs_token(const std::string& val) {
  if (val.empty()) return false;
  return std::all_of(val.begin(), val.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == ',' || c == '-' || c == '_';
  });
}

}  // namespace

std::vector<FanCurvePoint> get_curve_preset(const std::string& preset_name) {
  if (preset_name == "silent") {
    return {{35, 0}, {50, 25}, {65, 40}, {75, 60}, {85, 85}, {95, 100}};
  }
  if (preset_name == "gaming") {
    return {{35, 30}, {50, 50}, {65, 70}, {75, 85}, {85, 100}};
  }
  if (preset_name == "aggressive") {
    return {{35, 45}, {45, 65}, {60, 85}, {75, 100}, {85, 100}};
  }
  return {{35, 20}, {50, 35}, {65, 55}, {75, 75}, {85, 95}, {90, 100}};
}

std::string default_curve_config_path() {
  const char* env_p = std::getenv("NITROSENSE_CURVE_CONF");
  if (env_p && *env_p != '\0') return std::string(env_p);
  if (::geteuid() == 0) {
    return "/etc/nitrosense-btop/curve.conf";
  }
  const char* home = std::getenv("HOME");
  if (home && *home != '\0') {
    return (fs::path(home) / ".config" / "nitrosense-btop" / "curve.conf").string();
  }
  return "/tmp/nitrosense-btop-curve.conf";
}

bool save_fan_curve_config(const FanCurveConfig& cfg, const std::string& file_path) {
  const std::string target = file_path.empty() ? default_curve_config_path() : file_path;
  std::error_code ec;
  fs::path p(target);
  if (!p.parent_path().empty()) {
    fs::create_directories(p.parent_path(), ec);
  }
  std::ofstream out(target, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;

  out << "enabled=" << (cfg.enabled ? 1 : 0) << "\n";
  out << "preset=" << cfg.preset_name << "\n";
  out << "points=";
  std::vector<FanCurvePoint> sorted = cfg.points;
  std::sort(sorted.begin(), sorted.end(),
            [](const FanCurvePoint& a, const FanCurvePoint& b) { return a.temp < b.temp; });
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (i > 0) out << ";";
    out << sorted[i].temp << ":" << sorted[i].speed;
  }
  out << "\n";
  out.flush();
  return !out.fail();
}

FanCurveConfig load_fan_curve_config(const std::string& file_path) {
  FanCurveConfig cfg;
  const std::string target = file_path.empty() ? default_curve_config_path() : file_path;
  std::ifstream in(target);
  if (!in.is_open()) return cfg;

  std::string line;
  while (std::getline(in, line)) {
    line = trim_copy(line);
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim_copy(line.substr(0, eq));
    std::string v = trim_copy(line.substr(eq + 1));
    if (k == "enabled") {
      cfg.enabled = (v == "1" || v == "true");
    } else if (k == "preset" && !v.empty()) {
      cfg.preset_name = v;
    } else if (k == "points" && !v.empty()) {
      std::vector<FanCurvePoint> parsed;
      std::istringstream iss(v);
      std::string item;
      while (std::getline(iss, item, ';')) {
        size_t col = item.find(':');
        if (col == std::string::npos) continue;
        try {
          int t = std::clamp(std::stoi(item.substr(0, col)), 25, 105);
          int s = std::clamp(std::stoi(item.substr(col + 1)), 0, 100);
          parsed.push_back(FanCurvePoint{t, s});
        } catch (...) {
        }
      }
      if (parsed.size() >= 2) {
        std::sort(parsed.begin(), parsed.end(),
                  [](const FanCurvePoint& a, const FanCurvePoint& b) { return a.temp < b.temp; });
        cfg.points = std::move(parsed);
      }
    }
  }
  return cfg;
}

bool write_sysfs_value(const std::string& file_path, const std::string& value, const EnvOverrides& env) {
  if (file_path.empty() || !fs::exists(file_path)) return false;

  {
    std::ofstream out(file_path, std::ios::out | std::ios::trunc);
    if (out.is_open()) {
      out << value << "\n";
      out.flush();
      if (!out.fail()) {
        return true;
      }
    }
  }

  if (env.allow_sudo && ::geteuid() != 0 && is_safe_sysfs_token(value)) {
    std::string cmd = "printf '%s\\n' '" + value + "' | sudo -n tee '" + file_path + "' >/dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc == 0) return true;
  }

  return false;
}

ControlResult apply_fan_speed(int cpu_pct, int gpu_pct, FanMode mode, const EnvOverrides& env) {
  ControlResult res;
  const SystemPaths paths = detect_system_paths(env);

  int target_cpu = std::clamp(cpu_pct, 0, 100);
  int target_gpu = std::clamp(gpu_pct, 0, 100);
  if (mode == FanMode::Max) {
    target_cpu = 100;
    target_gpu = 100;
  } else if (mode == FanMode::Auto) {
    target_cpu = 0;
    target_gpu = 0;
  }

  res.payload = format_fan_speed_sysfs(target_cpu, target_gpu);

  if (paths.sense_dir.empty() || paths.fan_speed_path.empty()) {
    res.ok = false;
    res.written_to_hardware = false;
    res.error_message =
        "Módulo Linuwu-Sense não encontrado em /sys (nitro_sense/fan_speed ausente neste host).";
    return res;
  }

  res.target_path = paths.fan_speed_path;

  if (!paths.platform_profile_path.empty()) {
    auto current_prof = read_sysfs_trimmed(paths.platform_profile_path);
    const std::string clean = current_prof.has_value() ? trim_copy(*current_prof) : "";
    if (mode == FanMode::Max || (target_cpu == 100 && target_gpu == 100)) {
      write_sysfs_value(paths.platform_profile_path, "performance", env);
    } else if (mode == FanMode::Custom && (clean == "quiet" || clean == "low-power")) {
      write_sysfs_value(paths.platform_profile_path, "balanced", env);
    }
  }

  if (!write_sysfs_value(paths.fan_speed_path, res.payload, env)) {
    res.ok = false;
    res.written_to_hardware = false;
    res.error_message = "Permissão negada ao gravar em " + paths.fan_speed_path +
                        " (execute sudo ./install.sh para liberar tmpfiles/sudoers).";
    return res;
  }

  res.ok = true;
  res.written_to_hardware = true;
  res.info_message = "fan_speed=" + res.payload + " aplicado em hardware";
  return res;
}

ControlResult apply_thermal_profile(const std::string& profile_id, const EnvOverrides& env) {
  ControlResult res;
  const std::string platform_val = profile_id_to_platform_profile(profile_id);
  if (platform_val.empty()) {
    res.ok = false;
    res.error_message = "Perfil térmico inválido: " + profile_id;
    return res;
  }

  const SystemPaths paths = detect_system_paths(env);
  res.payload = platform_val;

  bool any_written = false;
  if (!paths.platform_profile_path.empty()) {
    res.target_path = paths.platform_profile_path;
    if (write_sysfs_value(paths.platform_profile_path, platform_val, env)) {
      any_written = true;
    } else {
      res.error_message = "Falha ao gravar em " + paths.platform_profile_path;
    }
  }

  if (!paths.fan_speed_path.empty()) {
    std::string fan_payload = "0,0";
    if (platform_val == "performance") {
      fan_payload = "100,100";
    } else if (platform_val == "balanced-performance") {
      fan_payload = "75,75";
    }
    if (write_sysfs_value(paths.fan_speed_path, fan_payload, env)) {
      any_written = true;
    }
  }

  if (!any_written) {
    res.ok = false;
    res.written_to_hardware = false;
    if (res.error_message.empty()) {
      res.error_message =
          "Interface ACPI platform_profile e nitro_sense indisponíveis neste host.";
    }
    return res;
  }

  res.ok = true;
  res.written_to_hardware = true;
  res.info_message = "Perfil aplicado: " + profile_id + " (" + platform_val + ")";
  return res;
}

ControlResult apply_hardware_setting(const std::string& key, int value, const EnvOverrides& env) {
  ControlResult res;
  const SystemPaths paths = detect_system_paths(env);

  std::string target;
  std::string str_val = std::to_string(value);
  if (key == "batteryLimiter" || key == "battery_limiter") {
    target = paths.battery_limiter_path;
    str_val = value ? "1" : "0";
  } else if (key == "lcdOverdrive" || key == "lcd_override") {
    target = paths.lcd_override_path;
    str_val = value ? "1" : "0";
  } else if (key == "keyboardBacklightTimeout" || key == "backlight_timeout") {
    target = paths.backlight_timeout_path;
    str_val = value ? "1" : "0";
  } else if (key == "usbPowerOffCharging" || key == "usb_charging") {
    target = paths.usb_charging_path;
    str_val = std::to_string(std::clamp(value, 0, 30));
  } else {
    res.ok = false;
    res.error_message = "Configuração desconhecida: " + key;
    return res;
  }

  res.payload = str_val;
  if (target.empty()) {
    res.ok = false;
    res.written_to_hardware = false;
    res.error_message = "Atributo sysfs ausente para '" + key + "' neste host.";
    return res;
  }

  res.target_path = target;
  if (!write_sysfs_value(target, str_val, env)) {
    res.ok = false;
    res.written_to_hardware = false;
    res.error_message = "Permissão negada ao gravar '" + str_val + "' em " + target;
    return res;
  }

  res.ok = true;
  res.written_to_hardware = true;
  res.info_message = key + "=" + str_val + " gravado";
  return res;
}

std::string control_result_to_json(const ControlResult& res) {
  std::ostringstream os;
  os << "{";
  os << "\"ok\":" << (res.ok ? "true" : "false") << ",";
  os << "\"writtenToHardware\":" << (res.written_to_hardware ? "true" : "false") << ",";
  os << "\"payload\":\"" << json_escape(res.payload) << "\",";
  os << "\"targetPath\":\"" << json_escape(res.target_path) << "\",";
  os << "\"message\":\"" << json_escape(res.ok ? res.info_message : res.error_message) << "\",";
  os << "\"error\":\"" << json_escape(res.error_message) << "\"";
  os << "}";
  return os.str();
}

int run_fan_curve_daemon(const EnvOverrides& env, int interval_ms) {
  ::signal(SIGINT, daemon_sig_handler);
  ::signal(SIGTERM, daemon_sig_handler);

  TelemetryCollector collector(env);
  collector.prime_cpu_sample();
  int last_applied_pct = -1;

  while (g_daemon_running) {
    FanCurveConfig cfg = load_fan_curve_config();
    if (cfg.enabled) {
      TelemetrySnapshot snap = collector.collect(1);
      if (snap.is_hardware_detected) {
        std::optional<double> peak;
        if (snap.cpu.temp_c.has_value()) peak = *snap.cpu.temp_c;
        if (snap.gpu.temp_c.has_value()) {
          peak = peak.has_value() ? std::max(*peak, *snap.gpu.temp_c) : *snap.gpu.temp_c;
        }
        if (peak.has_value()) {
          int target_pct = interpolate_fan_curve(*peak, cfg.points);
          if (last_applied_pct < 0 || std::abs(target_pct - last_applied_pct) >= 3) {
            auto r = apply_fan_speed(target_pct, target_pct, FanMode::Custom, env);
            if (r.ok) {
              last_applied_pct = target_pct;
            }
          }
        }
      }
    } else {
      last_applied_pct = -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(interval_ms, 500, 10000)));
  }
  return 0;
}

}  // namespace nitrosense
