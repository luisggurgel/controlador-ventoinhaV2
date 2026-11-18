#pragma once

#include "nitro_types.hpp"
#include "sensors.hpp"

#include <string>
#include <vector>

namespace nitrosense {

struct ControlResult {
  bool ok = false;
  bool written_to_hardware = false;
  std::string payload;
  std::string target_path;
  std::string error_message;
  std::string info_message;
};

struct FanCurveConfig {
  bool enabled = false;
  std::string preset_name = "balanced";
  std::vector<FanCurvePoint> points = {
      {35, 20},
      {50, 35},
      {65, 55},
      {75, 75},
      {85, 95},
      {90, 100},
  };
};

std::vector<FanCurvePoint> get_curve_preset(const std::string& preset_name);
std::string default_curve_config_path();
bool save_fan_curve_config(const FanCurveConfig& cfg, const std::string& file_path = "");
FanCurveConfig load_fan_curve_config(const std::string& file_path = "");

bool write_sysfs_value(const std::string& file_path, const std::string& value, const EnvOverrides& env);

ControlResult apply_fan_speed(int cpu_pct, int gpu_pct, FanMode mode,
                              const EnvOverrides& env = load_env_overrides());

ControlResult apply_thermal_profile(const std::string& profile_id,
                                    const EnvOverrides& env = load_env_overrides());

ControlResult apply_hardware_setting(const std::string& key, int value,
                                     const EnvOverrides& env = load_env_overrides());

std::string control_result_to_json(const ControlResult& res);

int run_fan_curve_daemon(const EnvOverrides& env = load_env_overrides(), int interval_ms = 2000);

}  // namespace nitrosense
