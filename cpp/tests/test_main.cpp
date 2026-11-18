#include "control.hpp"
#include "nitro_types.hpp"
#include "sensors.hpp"
#include "tui.hpp"

#include <stdlib.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& content) {
  std::ofstream out(p, std::ios::out | std::ios::trunc);
  assert(out.is_open());
  out << content;
  out.close();
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p);
  assert(in.is_open());
  std::string line;
  std::getline(in, line);
  return line;
}

void test_specs_and_mappings() {
  using namespace nitrosense;

  assert(MAX_CPU_RPM == 8600);
  assert(MAX_GPU_RPM == 8600);
  assert(std::string(GPU_SPEC_NAME).find("RTX 4060") != std::string::npos);

  assert(map_ec_mode_to_profile(0) == "turbo");
  assert(map_ec_mode_to_profile(1) == "performance");
  assert(map_ec_mode_to_profile(2) == "balanced");
  assert(map_ec_mode_to_profile(3) == "quiet");
  assert(map_ec_mode_to_profile(4) == "eco");

  assert(map_platform_profile_to_id("low-power\n") == "eco");
  assert(map_platform_profile_to_id("quiet") == "quiet");
  assert(map_platform_profile_to_id("balanced") == "balanced");
  assert(map_platform_profile_to_id("balanced-performance") == "performance");
  assert(map_platform_profile_to_id("performance") == "turbo");

  assert(profile_id_to_platform_profile("eco") == "low-power");
  assert(profile_id_to_platform_profile("turbo") == "performance");
  assert(profile_id_to_platform_profile("invalid").empty());

  assert(format_fan_speed_sysfs(0, 0) == "0,0");
  assert(format_fan_speed_sysfs(-5, 140) == "0,100");
  assert(format_fan_speed_sysfs(65, 80) == "65,80");

  auto p1 = parse_fan_speed_sysfs("45,90\n");
  assert(p1.cpu == 45 && p1.gpu == 90);
  auto p2 = parse_fan_speed_sysfs("bad");
  assert(p2.cpu == 0 && p2.gpu == 0);

  assert(calculate_rpm_from_percent(0) == 0);
  assert(calculate_rpm_from_percent(100) == 8600);
  assert(calculate_rpm_from_percent(50) == 5200);
}

void test_curve_interpolation_and_persistence() {
  using namespace nitrosense;
  std::vector<FanCurvePoint> curve = {{40, 20}, {60, 50}, {80, 100}};

  assert(interpolate_fan_curve(30.0, curve) == 20);
  assert(interpolate_fan_curve(40.0, curve) == 20);
  assert(interpolate_fan_curve(50.0, curve) == 35);
  assert(interpolate_fan_curve(60.0, curve) == 50);
  assert(interpolate_fan_curve(70.0, curve) == 75);
  assert(interpolate_fan_curve(80.0, curve) == 100);
  assert(interpolate_fan_curve(95.0, curve) == 100);

  char tpl[] = "/tmp/nitrosense_cfg_XXXXXX";
  char* dir_raw = ::mkdtemp(tpl);
  assert(dir_raw != nullptr);
  fs::path cfg_path = fs::path(dir_raw) / "curve.conf";

  FanCurveConfig cfg;
  cfg.enabled = true;
  cfg.preset_name = "gaming";
  cfg.points = get_curve_preset("gaming");
  assert(save_fan_curve_config(cfg, cfg_path.string()));

  FanCurveConfig loaded = load_fan_curve_config(cfg_path.string());
  assert(loaded.enabled == true);
  assert(loaded.preset_name == "gaming");
  assert(loaded.points.size() == cfg.points.size());
  assert(loaded.points.back().speed == 100);
  fs::remove_all(dir_raw);
}

void test_real_sysfs_integration() {
  using namespace nitrosense;

  char tpl[] = "/tmp/nitrosense_test_XXXXXX";
  char* dir_raw = ::mkdtemp(tpl);
  assert(dir_raw != nullptr);
  fs::path root(dir_raw);

  fs::path sense_dir = root / "nitro_sense";
  fs::path hwmon_dir = root / "hwmon";
  fs::path hwmon0 = hwmon_dir / "hwmon0";
  fs::path pwr_dir = root / "power_supply";
  fs::path ac_dir = pwr_dir / "ACAD";
  fs::path bat_dir = pwr_dir / "BAT1";
  fs::path profile_file = root / "platform_profile";
  fs::path choices_file = root / "platform_profile_choices";

  fs::create_directories(sense_dir);
  fs::create_directories(hwmon0);
  fs::create_directories(ac_dir);
  fs::create_directories(bat_dir);

  write_file(sense_dir / "fan_speed", "0,0\n");
  write_file(sense_dir / "battery_limiter", "1\n");
  write_file(sense_dir / "lcd_override", "1\n");
  write_file(sense_dir / "backlight_timeout", "0\n");
  write_file(sense_dir / "usb_charging", "10\n");

  write_file(hwmon0 / "name", "acer\n");
  write_file(hwmon0 / "temp1_input", "68500\n");
  write_file(hwmon0 / "temp2_input", "61000\n");
  write_file(hwmon0 / "fan1_input", "6420\n");
  write_file(hwmon0 / "fan2_input", "8600\n");

  write_file(ac_dir / "type", "Mains\n");
  write_file(ac_dir / "online", "1\n");
  write_file(bat_dir / "type", "Battery\n");
  write_file(bat_dir / "capacity", "80\n");
  write_file(bat_dir / "status", "Not charging\n");

  write_file(profile_file, "quiet\n");
  write_file(choices_file, "low-power quiet balanced balanced-performance performance\n");

  EnvOverrides env;
  env.sense_dir_override = sense_dir.string();
  env.hwmon_dir = hwmon_dir.string();
  env.power_supply_dir = pwr_dir.string();
  env.platform_profile_path = profile_file.string();
  env.platform_profile_choices_path = choices_file.string();
  env.disable_nvidia_smi = true;
  env.allow_sudo = false;

  TelemetryCollector collector(env);
  collector.prime_cpu_sample();
  TelemetrySnapshot snap1 = collector.collect(15);

  assert(snap1.is_hardware_detected == true);
  assert(snap1.has_real_sensors == true);
  assert(snap1.driver_status == "active");
  assert(snap1.cpu.temp_c.has_value() && *snap1.cpu.temp_c == 68.5);
  assert(snap1.gpu.temp_c.has_value() && *snap1.gpu.temp_c == 61.0);
  assert(snap1.fans.cpu_rpm.has_value() && *snap1.fans.cpu_rpm == 6420);
  assert(snap1.fans.gpu_rpm.has_value() && *snap1.fans.gpu_rpm == 8600);
  assert(snap1.power.ac_connected.has_value() && *snap1.power.ac_connected == true);
  assert(snap1.power.battery_pct.has_value() && *snap1.power.battery_pct == 80);
  assert(snap1.active_profile.has_value() && *snap1.active_profile == "quiet");
  assert(!snap1.processes.empty());
  assert(snap1.memory.total_kb > 0);

  ControlResult r_fan = apply_fan_speed(80, 90, FanMode::Custom, env);
  assert(r_fan.ok && r_fan.written_to_hardware);
  assert(read_file(profile_file) == "balanced");
  assert(read_file(sense_dir / "fan_speed") == "80,90");

  ControlResult r_prof = apply_thermal_profile("turbo", env);
  assert(r_prof.ok && r_prof.written_to_hardware);
  assert(read_file(profile_file) == "performance");
  assert(read_file(sense_dir / "fan_speed") == "100,100");

  ControlResult r_set = apply_hardware_setting("batteryLimiter", 0, env);
  assert(r_set.ok && r_set.written_to_hardware);
  assert(read_file(sense_dir / "battery_limiter") == "0");

  TelemetrySnapshot snap2 = collector.collect(10);
  assert(snap2.fans.cpu_target_pct.has_value() && *snap2.fans.cpu_target_pct == 100);
  assert(snap2.fans.mode == FanMode::Max);
  assert(snap2.settings.battery_limiter.has_value() && *snap2.settings.battery_limiter == false);

  // Test TUI pure frame renderer with real snapshot & overlays
  TuiState tui;
  tui.snap = snap2;
  tui.cpu_hist.push_back(22.5);
  tui.cpu_hist.push_back(64.0);
  std::string main_frame = render_btop_frame(tui, 120, 36);
  assert(main_frame.find("NITROSENSE-BTOP") != std::string::npos);
  assert(main_frame.find("6420 RPM") != std::string::npos);
  assert(main_frame.find("8600 RPM") != std::string::npos);
  assert(main_frame.find("╭─") != std::string::npos);

  tui.overlay = OverlayMode::CurveEditor;
  std::string curve_frame = render_btop_frame(tui, 120, 36);
  assert(curve_frame.find("curva dinâmica PWM") != std::string::npos);

  tui.overlay = OverlayMode::HelpSpecs;
  std::string help_frame = render_btop_frame(tui, 120, 36);
  assert(help_frame.find("ANV15-52-52VN") != std::string::npos);

  // Compact 80x24 terminal test
  tui.overlay = OverlayMode::None;
  std::string small_frame = render_btop_frame(tui, 80, 24);
  assert(small_frame.find("NITROSENSE-BTOP") != std::string::npos);

  // Negative test: missing sense dir MUST NOT simulate
  EnvOverrides missing_env = env;
  missing_env.sense_dir_override = (root / "does_not_exist").string();
  missing_env.hwmon_dir = (root / "no_hwmon").string();
  missing_env.platform_profile_path = (root / "no_profile").string();
  ControlResult r_miss = apply_fan_speed(50, 50, FanMode::Custom, missing_env);
  assert(!r_miss.ok && !r_miss.written_to_hardware);

  TelemetryCollector missing_col(missing_env);
  TelemetrySnapshot snap_miss = missing_col.collect(5);
  assert(!snap_miss.is_hardware_detected);
  assert(!snap_miss.cpu.temp_c.has_value());
  assert(!snap_miss.fans.cpu_rpm.has_value());

  tui.snap = snap_miss;
  std::string missing_frame = render_btop_frame(tui, 100, 30);
  assert(missing_frame.find("N/A") != std::string::npos);

  fs::remove_all(root);
}

}  // namespace

int main() {
  test_specs_and_mappings();
  test_curve_interpolation_and_persistence();
  test_real_sysfs_integration();
  std::cout << "ALL C++20 NITROSENSE-BTOP TESTS PASSED (specs, curve config, sysfs R/W, TUI renderer 80x24..120x36, zero-sim).\n";
  return 0;
}
