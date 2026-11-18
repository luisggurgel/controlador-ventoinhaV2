#pragma once

#include "control.hpp"
#include "sensors.hpp"

#include <deque>
#include <string>
#include <vector>

namespace nitrosense {

enum class ProcSortMode { Cpu, Mem, Rss, Pid };
enum class OverlayMode { None, CurveEditor, HelpSpecs, FilterPrompt, KillConfirm };

struct TuiState {
  TelemetrySnapshot snap;
  std::deque<double> cpu_hist;
  std::deque<double> cpu_temp_hist;
  std::deque<double> cpu_fan_hist;
  std::deque<double> gpu_fan_hist;

  ProcSortMode sort_mode = ProcSortMode::Cpu;
  bool sort_reverse = false;
  int proc_selected = 0;
  int proc_scroll = 0;
  std::string proc_filter;

  OverlayMode overlay = OverlayMode::None;
  FanCurveConfig curve_cfg;
  int curve_knot_selected = 0;
  int kill_target_pid = 0;
  std::string kill_target_cmd;

  bool link_fans = true;
  int manual_cpu_pct = 50;
  int manual_gpu_pct = 50;
  int refresh_ms = 1000;
  std::string status_banner = "Pronto • ANV15-52-52VN • MAX 8600 RPM";
};

std::string render_btop_frame(const TuiState& state, int cols, int rows);
int run_btop_tui(const EnvOverrides& env, int refresh_ms = 1000);

}  // namespace nitrosense
