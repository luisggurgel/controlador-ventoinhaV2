#include "tui.hpp"

#include "control.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace nitrosense {

namespace {

volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_resized = 1;

void handle_signal(int sig) {
  if (sig == SIGWINCH) {
    g_resized = 1;
  } else {
    g_running = 0;
  }
}

struct TermGuard {
  termios orig{};
  bool active = false;

  TermGuard() {
    if (::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &orig) == 0) {
      termios raw = orig;
      raw.c_lflag &= static_cast<unsigned int>(~(ICANON | ECHO));
      raw.c_cc[VMIN] = 0;
      raw.c_cc[VTIME] = 0;
      ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
      active = true;
    }
    const char* enter_seq = "\033[?1049h\033[?25l\033[2J";
    ssize_t unused = ::write(STDOUT_FILENO, enter_seq, std::strlen(enter_seq));
    (void)unused;
  }

  ~TermGuard() {
    const char* exit_seq = "\033[0m\033[?25h\033[?1049l";
    ssize_t unused = ::write(STDOUT_FILENO, exit_seq, std::strlen(exit_seq));
    (void)unused;
    if (active) {
      ::tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    }
  }
};

std::string fg_rgb(int r, int g, int b) {
  return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}
std::string bg_rgb(int r, int g, int b) {
  return "\033[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}
const char* RESET = "\033[0m";
const char* BOLD = "\033[1m";

std::string thermal_color(double pct_or_temp, bool is_temp) {
  double ratio = is_temp ? std::clamp((pct_or_temp - 35.0) / 60.0, 0.0, 1.0)
                         : std::clamp(pct_or_temp / 100.0, 0.0, 1.0);
  if (ratio < 0.45) return fg_rgb(34, 197, 94);
  if (ratio < 0.72) return fg_rgb(234, 179, 8);
  if (ratio < 0.88) return fg_rgb(249, 115, 22);
  return fg_rgb(239, 68, 68);
}

// UTF-8 safe visible character count and truncation (prevents mid-glyph splits)
size_t utf8_len(const std::string& s) {
  size_t count = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

std::string utf8_truncate(const std::string& s, size_t max_chars) {
  std::string out;
  size_t count = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t clen = 1;
    if ((c & 0xE0) == 0xC0) clen = 2;
    else if ((c & 0xF0) == 0xE0) clen = 3;
    else if ((c & 0xF8) == 0xF0) clen = 4;
    if (count + 1 > max_chars) break;
    if (i + clen > s.size()) break;
    out.append(s, i, clen);
    i += clen;
    ++count;
  }
  return out;
}

std::string utf8_pad(const std::string& s, int width) {
  if (width <= 0) return "";
  std::string t = utf8_truncate(s, static_cast<size_t>(width));
  size_t vlen = utf8_len(t);
  if (static_cast<int>(vlen) < width) {
    t.append(static_cast<size_t>(width) - vlen, ' ');
  }
  return t;
}

std::string utf8_braille(uint8_t mask) {
  char32_t cp = 0x2800 + mask;
  std::string out;
  out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
  out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
  out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  return out;
}

std::vector<std::string> render_braille_rows(const std::deque<double>& values, int char_width, int char_rows,
                                             double max_val, bool is_temp_palette = false) {
  std::vector<std::string> rows(static_cast<size_t>(char_rows));
  if (char_width <= 0 || char_rows <= 0) return rows;

  const int pixel_cols = char_width * 2;
  const int pixel_rows = char_rows * 4;
  std::vector<double> samples(static_cast<size_t>(pixel_cols), 0.0);

  if (!values.empty()) {
    const int n = static_cast<int>(values.size());
    const int start_src = std::max(0, n - pixel_cols);
    const int dst_offset = pixel_cols - (n - start_src);
    for (int i = start_src; i < n; ++i) {
      samples[static_cast<size_t>(dst_offset + (i - start_src))] = values[static_cast<size_t>(i)];
    }
  }

  static constexpr uint8_t LEFT_DOTS[4] = {0x01, 0x02, 0x04, 0x40};
  static constexpr uint8_t RIGHT_DOTS[4] = {0x08, 0x10, 0x20, 0x80};

  for (int r = 0; r < char_rows; ++r) {
    std::ostringstream line;
    for (int c = 0; c < char_width; ++c) {
      uint8_t mask = 0;
      double peak = 0.0;
      for (int side = 0; side < 2; ++side) {
        double v = samples[static_cast<size_t>(c * 2 + side)];
        peak = std::max(peak, v);
        double norm = std::clamp(v / std::max(1.0, max_val), 0.0, 1.0);
        int filled_px = static_cast<int>(std::lround(norm * pixel_rows));
        for (int py = 0; py < 4; ++py) {
          int global_y_from_bottom = (char_rows - 1 - r) * 4 + (3 - py);
          if (global_y_from_bottom < filled_px) {
            mask |= (side == 0 ? LEFT_DOTS[py] : RIGHT_DOTS[py]);
          }
        }
      }
      if (mask == 0) {
        line << fg_rgb(38, 44, 58) << utf8_braille(0x40 | 0x80);
      } else {
        line << thermal_color(peak, is_temp_palette) << utf8_braille(mask);
      }
    }
    rows[static_cast<size_t>(r)] = line.str();
  }
  return rows;
}

std::string render_bar(double pct, int width) {
  if (width <= 0) return "";
  double clamped = std::clamp(pct, 0.0, 100.0);
  int filled = static_cast<int>(std::lround((clamped / 100.0) * width));
  std::ostringstream os;
  os << thermal_color(clamped, false);
  for (int i = 0; i < filled; ++i) os << "█";
  os << fg_rgb(42, 48, 64);
  for (int i = filled; i < width; ++i) os << "░";
  return os.str();
}

std::string format_uptime(uint64_t sec) {
  uint64_t h = sec / 3600;
  uint64_t m = (sec % 3600) / 60;
  uint64_t s = sec % 60;
  std::ostringstream os;
  os << std::setfill('0') << std::setw(2) << h << ":" << std::setw(2) << m << ":" << std::setw(2) << s;
  return os.str();
}

std::string move_cursor(int row, int col) {
  return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

void draw_box(std::ostringstream& out, int row, int col, int w, int h, const std::string& title,
              const std::string& border_color, const std::string& title_color,
              int fill_r = 18, int fill_g = 21, int fill_b = 29) {
  if (w < 4 || h < 2) return;
  const int inner = w - 2;
  const std::string fill = bg_rgb(fill_r, fill_g, fill_b);

  out << move_cursor(row, col) << fill << border_color << "╭─";
  int used = 1;
  std::string safe_title = utf8_truncate(title, static_cast<size_t>(std::max(0, inner - 3)));
  int tlen = static_cast<int>(utf8_len(safe_title));
  if (tlen > 0 && inner > tlen + 2) {
    out << title_color << BOLD << " " << safe_title << " " << RESET << fill << border_color;
    used += tlen + 2;
  }
  for (int i = used; i < inner; ++i) out << "─";
  out << "╮";

  const std::string blank(static_cast<size_t>(inner), ' ');
  for (int r = 1; r < h - 1; ++r) {
    out << move_cursor(row + r, col) << fill << border_color << "│" << blank << "│";
  }

  out << move_cursor(row + h - 1, col) << fill << border_color << "╰";
  for (int i = 0; i < inner; ++i) out << "─";
  out << "╯";
}

const char* sort_label(ProcSortMode m) {
  switch (m) {
    case ProcSortMode::Cpu:
      return "cpu%";
    case ProcSortMode::Mem:
      return "mem%";
    case ProcSortMode::Rss:
      return "rss";
    case ProcSortMode::Pid:
      return "pid";
  }
  return "cpu%";
}

std::string to_lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::vector<ProcessInfo> filtered_sorted_procs(const TuiState& state) {
  std::vector<ProcessInfo> procs;
  const std::string q = to_lower_copy(state.proc_filter);
  for (const auto& p : state.snap.processes) {
    if (!q.empty()) {
      if (to_lower_copy(p.command).find(q) == std::string::npos &&
          to_lower_copy(p.user).find(q) == std::string::npos &&
          std::to_string(p.pid).find(q) == std::string::npos) {
        continue;
      }
    }
    procs.push_back(p);
  }

  std::sort(procs.begin(), procs.end(), [&](const ProcessInfo& a, const ProcessInfo& b) {
    bool gt = false;
    switch (state.sort_mode) {
      case ProcSortMode::Cpu:
        gt = (a.cpu_pct != b.cpu_pct) ? (a.cpu_pct > b.cpu_pct) : (a.mem_mb > b.mem_mb);
        break;
      case ProcSortMode::Mem:
        gt = (a.mem_pct != b.mem_pct) ? (a.mem_pct > b.mem_pct) : (a.cpu_pct > b.cpu_pct);
        break;
      case ProcSortMode::Rss:
        gt = (a.mem_mb != b.mem_mb) ? (a.mem_mb > b.mem_mb) : (a.cpu_pct > b.cpu_pct);
        break;
      case ProcSortMode::Pid:
        gt = a.pid < b.pid;
        break;
    }
    return state.sort_reverse ? !gt : gt;
  });
  return procs;
}

void draw_curve_overlay(std::ostringstream& out, const TuiState& state, int cols, int rows) {
  const int mw = std::min(cols - 4, 74);
  const int mh = std::min(rows - 4, 18);
  if (mw < 48 || mh < 12) return;
  const int r0 = (rows - mh) / 2 + 1;
  const int c0 = (cols - mw) / 2 + 1;

  draw_box(out, r0, c0, mw, mh, "curva dinâmica PWM (0..8600 RPM)", fg_rgb(52, 211, 153),
           fg_rgb(52, 211, 153), 12, 15, 22);

  const std::string panel_bg = bg_rgb(12, 15, 22);
  const std::string muted = fg_rgb(148, 163, 184);
  const std::string hi = fg_rgb(241, 245, 249);

  out << move_cursor(r0 + 1, c0 + 2) << panel_bg << muted << "Preset: " << fg_rgb(250, 204, 21)
      << BOLD << state.curve_cfg.preset_name << RESET << panel_bg << muted
      << "   Estado: "
      << (state.curve_cfg.enabled ? fg_rgb(34, 197, 94) + "ATIVA (daemon/TUI)"
                                  : fg_rgb(148, 163, 184) + "DESATIVADA")
      << "   [P] Mudar Preset";

  const int max_knots = std::min(static_cast<int>(state.curve_cfg.points.size()), mh - 6);
  for (int i = 0; i < max_knots; ++i) {
    const auto& pt = state.curve_cfg.points[static_cast<size_t>(i)];
    const bool sel = (i == state.curve_knot_selected);
    const int rpm = calculate_rpm_from_percent(pt.speed);
    const int bar_w = std::max(10, mw - 42);
    out << move_cursor(r0 + 3 + i, c0 + 2) << (sel ? bg_rgb(36, 46, 66) : panel_bg)
        << (sel ? fg_rgb(250, 204, 21) + "▶ " : muted + "  ")
        << std::setw(3) << pt.temp << "°C → " << std::setw(3) << pt.speed << "% "
        << "(" << std::setw(4) << rpm << " RPM)  " << render_bar(pt.speed, bar_w);
  }

  out << move_cursor(r0 + mh - 3, c0 + 2) << panel_bg << muted
      << "[↑/↓] Selecionar ponto  [←/→] Ajustar ±5%  [P] Preset (silent/bal/gaming/agg)";
  out << move_cursor(r0 + mh - 2, c0 + 2) << panel_bg << fg_rgb(56, 189, 248)
      << "[Enter/w] Salvar & Ativar Curva   [d] Desativar Curva   [Esc/c] Fechar";
}

void draw_help_overlay(std::ostringstream& out, const TuiState& state, int cols, int rows) {
  const int mw = std::min(cols - 4, 82);
  const int mh = std::min(rows - 4, 20);
  if (mw < 50 || mh < 14) return;
  const int r0 = (rows - mh) / 2 + 1;
  const int c0 = (cols - mw) / 2 + 1;

  draw_box(out, r0, c0, mw, mh, "ajuda & diagnóstico ANV15-52-52VN", fg_rgb(56, 189, 248),
           fg_rgb(56, 189, 248), 12, 15, 22);

  const std::string pbg = bg_rgb(12, 15, 22);
  const std::string muted = fg_rgb(148, 163, 184);
  const std::string val = fg_rgb(241, 245, 249);

  std::vector<std::string> lines = {
      "HW Alvo   : " + std::string(MODEL_NAME) + " • " + std::string(CPU_SPEC_NAME),
      "GPU / Max : " + std::string(GPU_SPEC_NAME) + " • MAX 8600 RPM",
      "Quirk EC  : " + std::string(EC_QUIRK_NAME) + " (" + EC_WINDOW_BASE + "+" + EC_PROFILE_OFFSET + ")",
      "Host DMI  : " + (state.snap.dmi_product_name.empty() ? std::string("N/A") : state.snap.dmi_product_name) +
          " | OS: " + state.snap.os_name,
      "nitro_sense: " + (state.snap.paths.sense_dir.empty() ? std::string("ausente") : state.snap.paths.sense_dir),
      "hwmon acer : " + (state.snap.paths.hwmon_acer_dir.empty() ? std::string("ausente") : state.snap.paths.hwmon_acer_dir),
      "--------------------------------------------------------------------------",
      "[1..5] Perfis ACPI: Eco | Quiet | Balanced | Performance | Turbo (8600 RPM)",
      "[a] Auto (0,0)  [m] Max 8600 RPM  [v] Vincular Fans  [[/]] CPU  [{/}] GPU",
      "[c] Editor Curva Dinâmica   [b] Limite Bateria 80%   [o] LCD Overdrive 165Hz",
      "[↑/↓/j/k] Navegar Proc   [/] Filtrar   [s/r] Ordenar/Reverter   [K] SIGTERM",
      "[+/-] Taxa Poll (" + std::to_string(state.refresh_ms) + "ms)   [h/Esc] Fechar   [q] Sair",
  };

  for (int i = 0; i < std::min(static_cast<int>(lines.size()), mh - 2); ++i) {
    out << move_cursor(r0 + 1 + i, c0 + 2) << pbg << (i < 6 ? val : muted)
        << utf8_pad(lines[static_cast<size_t>(i)], mw - 4);
  }
}

void draw_kill_confirm_overlay(std::ostringstream& out, const TuiState& state, int cols, int rows) {
  const int mw = std::min(cols - 4, 58);
  const int mh = 6;
  const int r0 = (rows - mh) / 2 + 1;
  const int c0 = (cols - mw) / 2 + 1;

  draw_box(out, r0, c0, mw, mh, "confirmar SIGTERM", fg_rgb(239, 68, 68),
           fg_rgb(248, 113, 113), 18, 12, 16);

  const std::string pbg = bg_rgb(18, 12, 16);
  out << move_cursor(r0 + 2, c0 + 2) << pbg << fg_rgb(241, 245, 249)
      << utf8_pad("Enviar SIGTERM ao PID " + std::to_string(state.kill_target_pid) + " (" +
                      state.kill_target_cmd + ")?",
                  mw - 4);
  out << move_cursor(r0 + 3, c0 + 2) << pbg << fg_rgb(250, 204, 21)
      << utf8_pad("[y] Sim, encerrar processo    [n/Esc] Cancelar", mw - 4);
}

}  // namespace

std::string render_btop_frame(const TuiState& state, int cols, int rows) {
  cols = std::max(72, cols);
  rows = std::max(22, rows);

  const auto& snap = state.snap;
  const std::string panel_bg = bg_rgb(18, 21, 29);
  const std::string c_border = fg_rgb(56, 65, 86);
  const std::string c_muted = fg_rgb(132, 144, 168);
  const std::string c_val = fg_rgb(241, 245, 249);

  std::ostringstream frame;
  frame << "\033[H" << bg_rgb(14, 16, 21);

  // Row 1: Top authentic btop strip
  {
    std::ostringstream top_l, top_r;
    top_l << " NITROSENSE-BTOP │ ANV15-52-52VN │ i5-13420H + RTX 4060 8G";
    top_r << "host:" << snap.hostname << " │ up " << format_uptime(snap.cpu.uptime_sec)
          << " │ " << (state.refresh_ms / 1000.0) << "s │ "
          << (snap.is_hardware_detected ? "LINUWU_SENSE" : snap.driver_status) << " ";
    std::string rtxt = top_r.str();
    int left_space = std::max(10, cols - static_cast<int>(utf8_len(rtxt)));
    frame << move_cursor(1, 1) << bg_rgb(20, 24, 34) << fg_rgb(239, 68, 68) << BOLD
          << utf8_pad(" NITROSENSE-BTOP", std::min(18, left_space)) << RESET << bg_rgb(20, 24, 34)
          << fg_rgb(148, 163, 184)
          << utf8_pad(" │ ANV15-52-52VN │ i5-13420H + RTX 4060 8G", std::max(0, left_space - 18))
          << (snap.is_hardware_detected ? fg_rgb(34, 197, 94) : fg_rgb(234, 179, 8)) << rtxt
          << RESET;
  }

  const int body_top = 2;
  const int footer_row = rows;
  const int body_h = rows - 2;

  const int cpu_h = std::clamp(body_h / 3 + 1, 7, 13);
  const int mid_top = body_top + cpu_h;
  const int mid_h = std::clamp(body_h / 3, 7, 11);
  const int bot_top = mid_top + mid_h;
  const int bot_h = std::max(5, footer_row - bot_top);

  // BOX 1: CPU + Braille Plot + Per-Core Bars
  draw_box(frame, body_top, 1, cols, cpu_h, "¹ cpu • " + snap.cpu.model_name, c_border,
           fg_rgb(248, 113, 113));
  {
    const int left_w = std::max(26, (cols - 5) * 56 / 100);
    const int right_col = 3 + left_w + 2;
    const int right_w = std::max(18, cols - right_col - 1);

    const int plot_rows = std::max(2, cpu_h - 3);
    const int braille_w = std::max(10, left_w - 5);
    auto b_rows = render_braille_rows(state.cpu_hist, braille_w, plot_rows, 100.0, false);
    for (int r = 0; r < plot_rows; ++r) {
      std::string ylbl = (r == 0) ? "100% " : (r == plot_rows - 1 ? "  0% " : "     ");
      frame << move_cursor(body_top + 1 + r, 3) << panel_bg << c_muted << ylbl
            << b_rows[static_cast<size_t>(r)];
    }

    std::ostringstream sum;
    sum << std::fixed << std::setprecision(1);
    sum << "CPU " << (snap.cpu.total_usage_pct.has_value() ? std::to_string(*snap.cpu.total_usage_pct) + "%" : "N/A")
        << "  Temp: " << (snap.cpu.temp_c.has_value() ? std::to_string(*snap.cpu.temp_c) + "°C" : "N/A")
        << "  Pkg: " << (snap.cpu.power_watts.has_value() ? std::to_string(*snap.cpu.power_watts) + "W" : "N/A")
        << "  Load: " << std::setprecision(2) << snap.cpu.load1 << " " << snap.cpu.load5 << " " << snap.cpu.load15;
    frame << move_cursor(body_top + cpu_h - 2, 3) << panel_bg << c_val
          << utf8_pad(sum.str(), left_w);

    // Right column: multi-column core grid if wide enough
    const int max_lines = cpu_h - 2;
    const int core_count = static_cast<int>(snap.cpu.core_usages_pct.size());
    const int num_cols = (core_count > max_lines && right_w >= 42) ? 2 : 1;
    const int col_w = (right_w - (num_cols - 1) * 2) / num_cols;

    for (int i = 0; i < core_count; ++i) {
      int r = i % max_lines;
      int c_idx = i / max_lines;
      if (c_idx >= num_cols) break;

      int x = right_col + c_idx * (col_w + 2);
      double u = snap.cpu.core_usages_pct[static_cast<size_t>(i)];
      int mhz = i < static_cast<int>(snap.cpu.core_freqs_mhz.size())
                    ? snap.cpu.core_freqs_mhz[static_cast<size_t>(i)]
                    : 0;
      int bar_w = std::max(4, col_w - (mhz > 0 && col_w >= 24 ? 17 : 11));
      std::ostringstream cl;
      cl << panel_bg << c_muted << "C" << std::setw(2) << std::setfill('0') << i << " " << render_bar(u, bar_w)
         << " " << thermal_color(u, false) << std::setw(5) << std::setfill(' ') << std::fixed
         << std::setprecision(1) << u << "%";
      if (mhz > 0 && col_w >= 24) {
        cl << c_muted << " " << std::setw(4) << mhz;
      }
      frame << move_cursor(body_top + 1 + r, x) << cl.str();
    }
  }

  // BOX 2 (Left) + BOX 3 (Right)
  const int half_w = cols / 2;
  const int right_half_w = cols - half_w;
  draw_box(frame, mid_top, 1, half_w, mid_h, "² mem & power", c_border, fg_rgb(56, 189, 248));
  draw_box(frame, mid_top, half_w + 1, right_half_w, mid_h, "³ fans (8600 rpm) & gpu", c_border,
           fg_rgb(52, 211, 153));

  {
    const double total_gb = static_cast<double>(snap.memory.total_kb) / (1024.0 * 1024.0);
    const double used_gb = static_cast<double>(snap.memory.used_kb) / (1024.0 * 1024.0);
    const double avail_gb = static_cast<double>(snap.memory.available_kb) / (1024.0 * 1024.0);
    const double cached_gb = static_cast<double>(snap.memory.cached_kb) / (1024.0 * 1024.0);
    const double mem_pct = snap.memory.total_kb > 0
                               ? (static_cast<double>(snap.memory.used_kb) * 100.0) /
                                     static_cast<double>(snap.memory.total_kb)
                               : 0.0;
    const double sw_tot = static_cast<double>(snap.memory.swap_total_kb) / (1024.0 * 1024.0);
    const double sw_usd = static_cast<double>(snap.memory.swap_used_kb) / (1024.0 * 1024.0);
    const double sw_pct = snap.memory.swap_total_kb > 0
                              ? (static_cast<double>(snap.memory.swap_used_kb) * 100.0) /
                                    static_cast<double>(snap.memory.swap_total_kb)
                              : 0.0;
    const int bar_w = std::max(6, half_w - 26);

    std::ostringstream l1, l2, l3, l4, l5;
    l1 << std::fixed << std::setprecision(1) << panel_bg << c_muted << "RAM  " << render_bar(mem_pct, bar_w)
       << " " << c_val << used_gb << "/" << total_gb << "G";
    l2 << std::fixed << std::setprecision(1) << panel_bg << c_muted << "Avail: " << c_val << avail_gb
       << "G  " << c_muted << "Cache: " << c_val << cached_gb << "G";
    l3 << std::fixed << std::setprecision(1) << panel_bg << c_muted << "Swap " << render_bar(sw_pct, bar_w)
       << " " << c_val << sw_usd << "/" << sw_tot << "G";
    l4 << panel_bg << c_muted << "AC: "
       << (snap.power.ac_connected.has_value() ? (*snap.power.ac_connected ? "ONLINE" : "BAT") : "N/A")
       << "  Bat: " << (snap.power.battery_pct.has_value() ? std::to_string(*snap.power.battery_pct) + "%" : "N/A")
       << "  USB: " << (snap.settings.usb_power_off_charging.has_value()
                            ? std::to_string(*snap.settings.usb_power_off_charging) + "%"
                            : "N/A");
    l5 << panel_bg << c_muted << "[b]Lim80:"
       << (snap.settings.battery_limiter.has_value() ? (*snap.settings.battery_limiter ? "ON" : "OFF") : "N/A")
       << " [o]LCD:" << (snap.settings.lcd_overdrive.has_value() ? (*snap.settings.lcd_overdrive ? "ON" : "OFF") : "N/A")
       << " [t]KB:" << (snap.settings.keyboard_backlight_timeout.has_value()
                             ? (*snap.settings.keyboard_backlight_timeout ? "ON" : "OFF")
                             : "N/A");

    frame << move_cursor(mid_top + 1, 3) << l1.str();
    if (mid_h > 4) frame << move_cursor(mid_top + 2, 3) << l2.str();
    if (mid_h > 5) frame << move_cursor(mid_top + 3, 3) << l3.str();
    if (mid_h > 6) frame << move_cursor(mid_top + 4, 3) << l4.str();
    if (mid_h > 7) frame << move_cursor(mid_top + 5, 3) << l5.str();
  }

  {
    const int rx = half_w + 3;
    const int bar_w = std::max(6, right_half_w - 30);
    const double cpu_fan_pct = snap.fans.cpu_rpm.has_value()
                                   ? (static_cast<double>(*snap.fans.cpu_rpm) * 100.0 / MAX_CPU_RPM)
                                   : static_cast<double>(snap.fans.cpu_target_pct.value_or(0));
    const double gpu_fan_pct = snap.fans.gpu_rpm.has_value()
                                   ? (static_cast<double>(*snap.fans.gpu_rpm) * 100.0 / MAX_GPU_RPM)
                                   : static_cast<double>(snap.fans.gpu_target_pct.value_or(0));

    std::ostringstream r1, r2, r3, r4, r5;
    r1 << panel_bg << c_muted << "CPU Fan " << render_bar(cpu_fan_pct, bar_w) << " " << c_val
       << (snap.fans.cpu_rpm.has_value() ? std::to_string(*snap.fans.cpu_rpm) + " RPM" : "N/A")
       << c_muted << " (" << (snap.fans.cpu_target_pct.has_value() ? std::to_string(*snap.fans.cpu_target_pct) + "%" : "—")
       << ")";
    r2 << panel_bg << c_muted << "GPU Fan " << render_bar(gpu_fan_pct, bar_w) << " " << c_val
       << (snap.fans.gpu_rpm.has_value() ? std::to_string(*snap.fans.gpu_rpm) + " RPM" : "N/A")
       << c_muted << " (" << (snap.fans.gpu_target_pct.has_value() ? std::to_string(*snap.fans.gpu_target_pct) + "%" : "—")
       << ")";
    r3 << panel_bg << c_muted << "GPU: " << c_val << utf8_truncate(snap.gpu.name, static_cast<size_t>(std::max(12, right_half_w - 10)));
    r4 << panel_bg << c_muted << "Temp:"
       << (snap.gpu.temp_c.has_value() ? thermal_color(*snap.gpu.temp_c, true) + std::to_string(static_cast<int>(*snap.gpu.temp_c)) + "°C"
                                      : c_val + "N/A")
       << c_muted << " Util:"
       << (snap.gpu.usage_pct.has_value() ? std::to_string(static_cast<int>(*snap.gpu.usage_pct)) + "%" : "N/A")
       << " Pwr:"
       << (snap.gpu.power_watts.has_value() ? std::to_string(static_cast<int>(*snap.gpu.power_watts)) + "W" : "N/A")
       << " Curva:" << (state.curve_cfg.enabled ? fg_rgb(34, 197, 94) + state.curve_cfg.preset_name
                                                : c_muted + "OFF");

    // Inline profile selector strip
    r5 << panel_bg << c_muted << "ACPI: ";
    static constexpr const char* P_KEYS[5] = {"1:Eco", "2:Quiet", "3:Bal", "4:Perf", "5:Turbo"};
    static constexpr const char* P_IDS[5] = {"eco", "quiet", "balanced", "performance", "turbo"};
    for (int k = 0; k < 5; ++k) {
      bool act = snap.active_profile.has_value() && *snap.active_profile == P_IDS[k];
      if (act) {
        r5 << fg_rgb(248, 113, 113) << BOLD << "[" << P_KEYS[k] << "]" << RESET << panel_bg << " ";
      } else {
        r5 << c_muted << P_KEYS[k] << " ";
      }
    }

    frame << move_cursor(mid_top + 1, rx) << r1.str();
    if (mid_h > 4) frame << move_cursor(mid_top + 2, rx) << r2.str();
    if (mid_h > 5) frame << move_cursor(mid_top + 3, rx) << r3.str();
    if (mid_h > 6) frame << move_cursor(mid_top + 4, rx) << r4.str();
    if (mid_h > 7) frame << move_cursor(mid_top + 5, rx) << r5.str();
  }

  // BOX 4: Process Table with Highlight & Live Filter
  {
    std::ostringstream btitle;
    btitle << "⁴ proc (sort:" << sort_label(state.sort_mode) << (state.sort_reverse ? "↑" : "↓") << ")";
    if (!state.proc_filter.empty() || state.overlay == OverlayMode::FilterPrompt) {
      btitle << " [filtro: " << state.proc_filter << (state.overlay == OverlayMode::FilterPrompt ? "_" : "") << "]";
    } else {
      btitle << " • " << state.status_banner;
    }
    draw_box(frame, bot_top, 1, cols, bot_h, btitle.str(), c_border, fg_rgb(250, 204, 21));

    const int inner_w = cols - 4;
    std::ostringstream hdr;
    hdr << panel_bg << fg_rgb(148, 163, 184) << BOLD << std::left << std::setw(7) << "PID"
        << std::setw(11) << "USER" << std::setw(3) << "S" << std::right << std::setw(7) << "CPU%"
        << std::setw(7) << "MEM%" << std::setw(9) << "RSS(M)" << "  " << std::left << "COMMAND" << RESET;
    frame << move_cursor(bot_top + 1, 3) << hdr.str();

    auto procs = filtered_sorted_procs(state);
    const int max_rows = std::max(0, bot_h - 3);
    const int n_procs = static_cast<int>(procs.size());
    const int sel = std::clamp(state.proc_selected, 0, std::max(0, n_procs - 1));
    int scroll = std::clamp(state.proc_scroll, 0, std::max(0, n_procs - max_rows));
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + max_rows && max_rows > 0) scroll = sel - max_rows + 1;

    for (int i = 0; i < max_rows && (scroll + i) < n_procs; ++i) {
      const auto& p = procs[static_cast<size_t>(scroll + i)];
      const bool is_sel = (scroll + i == sel);
      const int cmd_w = std::max(8, inner_w - 46);
      const std::string row_bg = is_sel ? bg_rgb(38, 48, 70) : panel_bg;
      std::string cmd = utf8_pad(p.command, cmd_w);

      std::ostringstream pl;
      pl << row_bg << (is_sel ? fg_rgb(250, 204, 21) : fg_rgb(203, 213, 225)) << std::left << std::setw(7)
         << p.pid << std::setw(11) << utf8_truncate(p.user, 10) << std::setw(3) << p.state << std::right
         << std::fixed << std::setprecision(1) << thermal_color(p.cpu_pct, false) << std::setw(7)
         << p.cpu_pct << fg_rgb(56, 189, 248) << std::setw(7) << p.mem_pct << fg_rgb(148, 163, 184)
         << std::setw(9) << p.mem_mb << "  " << c_val << std::left << cmd;
      frame << move_cursor(bot_top + 2 + i, 3) << pl.str();
    }
  }

  // Overlays when requested
  if (state.overlay == OverlayMode::CurveEditor) {
    draw_curve_overlay(frame, state, cols, rows);
  } else if (state.overlay == OverlayMode::HelpSpecs) {
    draw_help_overlay(frame, state, cols, rows);
  } else if (state.overlay == OverlayMode::KillConfirm) {
    draw_kill_confirm_overlay(frame, state, cols, rows);
  }

  // Footer Keycap Bar
  std::string fbar =
      " [1-5] Eco..Turbo │ [a] Auto │ [m] 8600 │ [[/]] CPU │ [{/}] GPU │ [c] Curva │ [b] Lim80 │ [/] Filtro │ [s] Sort │ [h] Ajuda │ [q] Sair";
  frame << move_cursor(footer_row, 1) << bg_rgb(20, 24, 34) << fg_rgb(148, 163, 184)
        << utf8_pad(fbar, cols) << RESET;

  return frame.str();
}

int run_btop_tui(const EnvOverrides& env, int refresh_ms) {
  struct sigaction sa{};
  sa.sa_handler = handle_signal;
  ::sigemptyset(&sa.sa_mask);
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGWINCH, &sa, nullptr);

  TermGuard guard;
  TelemetryCollector collector(env);
  collector.prime_cpu_sample();
  ::usleep(120000);

  TuiState state;
  state.refresh_ms = std::clamp(refresh_ms, 250, 5000);
  state.curve_cfg = load_fan_curve_config();

  auto last_poll = std::chrono::steady_clock::now() - std::chrono::milliseconds(state.refresh_ms);
  int last_curve_pct = -1;

  while (g_running) {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_poll).count() >= state.refresh_ms) {
      state.snap = collector.collect(120);
      last_poll = now;

      if (state.snap.fans.cpu_target_pct.has_value() && *state.snap.fans.cpu_target_pct > 0) {
        state.manual_cpu_pct = *state.snap.fans.cpu_target_pct;
      }
      if (state.snap.fans.gpu_target_pct.has_value() && *state.snap.fans.gpu_target_pct > 0) {
        state.manual_gpu_pct = *state.snap.fans.gpu_target_pct;
      }

      state.cpu_hist.push_back(state.snap.cpu.total_usage_pct.value_or(0.0));
      if (state.cpu_hist.size() > 240) state.cpu_hist.pop_front();
      state.cpu_temp_hist.push_back(state.snap.cpu.temp_c.value_or(0.0));
      if (state.cpu_temp_hist.size() > 240) state.cpu_temp_hist.pop_front();

      // Active custom curve controller loop when enabled
      if (state.curve_cfg.enabled && state.snap.is_hardware_detected) {
        std::optional<double> peak;
        if (state.snap.cpu.temp_c.has_value()) peak = *state.snap.cpu.temp_c;
        if (state.snap.gpu.temp_c.has_value()) {
          peak = peak.has_value() ? std::max(*peak, *state.snap.gpu.temp_c) : *state.snap.gpu.temp_c;
        }
        if (peak.has_value()) {
          int pct = interpolate_fan_curve(*peak, state.curve_cfg.points);
          if (last_curve_pct < 0 || std::abs(pct - last_curve_pct) >= 3) {
            auto r = apply_fan_speed(pct, pct, FanMode::Custom, env);
            if (r.ok) {
              last_curve_pct = pct;
              state.status_banner = "Curva " + state.curve_cfg.preset_name + " → " + std::to_string(pct) + "%";
            }
          }
        }
      }
    }

    winsize ws{};
    int cols = 120;
    int rows = 36;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
      cols = ws.ws_col;
      rows = ws.ws_row;
    }

    const std::string rendered = render_btop_frame(state, cols, rows);
    ssize_t unused = ::write(STDOUT_FILENO, rendered.data(), rendered.size());
    (void)unused;

    pollfd pfd{};
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    int pret = ::poll(&pfd, 1, 100);
    if (pret <= 0 || !(pfd.revents & POLLIN)) continue;

    char ch = 0;
    if (::read(STDIN_FILENO, &ch, 1) != 1) continue;

    // Handle active filter input prompt
    if (state.overlay == OverlayMode::FilterPrompt) {
      if (ch == '\r' || ch == '\n') {
        state.overlay = OverlayMode::None;
      } else if (ch == '\033') {
        state.proc_filter.clear();
        state.overlay = OverlayMode::None;
      } else if (ch == 127 || ch == '\b') {
        if (!state.proc_filter.empty()) state.proc_filter.pop_back();
      } else if (std::isprint(static_cast<unsigned char>(ch))) {
        if (state.proc_filter.size() < 32) state.proc_filter.push_back(ch);
      }
      continue;
    }

    // Handle Kill confirmation prompt
    if (state.overlay == OverlayMode::KillConfirm) {
      if (ch == 'y' || ch == 'Y') {
        if (state.kill_target_pid > 1 && ::kill(state.kill_target_pid, SIGTERM) == 0) {
          state.status_banner = "SIGTERM enviado ao PID " + std::to_string(state.kill_target_pid);
        } else {
          state.status_banner = "Falha ao enviar SIGTERM ao PID " + std::to_string(state.kill_target_pid);
        }
        state.overlay = OverlayMode::None;
      } else {
        state.overlay = OverlayMode::None;
      }
      continue;
    }

    // Parse Escape / Arrow / PgUp / PgDn / F1 sequences
    if (ch == '\033') {
      char seq[4] = {0};
      pollfd pseq{STDIN_FILENO, POLLIN, 0};
      if (::poll(&pseq, 1, 15) > 0 && ::read(STDIN_FILENO, &seq[0], 1) == 1) {
        if (seq[0] == '[' && ::read(STDIN_FILENO, &seq[1], 1) == 1) {
          if (seq[1] == 'A') {  // Up
            if (state.overlay == OverlayMode::CurveEditor) {
              if (state.curve_knot_selected > 0) --state.curve_knot_selected;
            } else if (state.proc_selected > 0) {
              --state.proc_selected;
            }
          } else if (seq[1] == 'B') {  // Down
            if (state.overlay == OverlayMode::CurveEditor) {
              if (state.curve_knot_selected + 1 < static_cast<int>(state.curve_cfg.points.size()))
                ++state.curve_knot_selected;
            } else {
              ++state.proc_selected;
            }
          } else if (seq[1] == 'C' && state.overlay == OverlayMode::CurveEditor) {  // Right
            auto& pt = state.curve_cfg.points[static_cast<size_t>(state.curve_knot_selected)];
            pt.speed = std::clamp(pt.speed + 5, 0, 100);
          } else if (seq[1] == 'D' && state.overlay == OverlayMode::CurveEditor) {  // Left
            auto& pt = state.curve_cfg.points[static_cast<size_t>(state.curve_knot_selected)];
            pt.speed = std::clamp(pt.speed - 5, 0, 100);
          } else if (seq[1] == '5' && ::read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == '~') {
            state.proc_selected = std::max(0, state.proc_selected - 10);
          } else if (seq[1] == '6' && ::read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == '~') {
            state.proc_selected += 10;
          }
        } else if (seq[0] == 'O' && ::read(STDIN_FILENO, &seq[1], 1) == 1 && seq[1] == 'P') {  // F1
          state.overlay = (state.overlay == OverlayMode::HelpSpecs) ? OverlayMode::None : OverlayMode::HelpSpecs;
        }
      } else {
        if (state.overlay != OverlayMode::None) state.overlay = OverlayMode::None;
        else g_running = 0;
      }
      continue;
    }

    // Handle Curve Editor hotkeys
    if (state.overlay == OverlayMode::CurveEditor) {
      if (ch == 'c' || ch == 'C') {
        state.overlay = OverlayMode::None;
      } else if (ch == 'p' || ch == 'P') {
        static constexpr const char* PRESETS[4] = {"silent", "balanced", "gaming", "aggressive"};
        int idx = 0;
        for (int i = 0; i < 4; ++i) {
          if (state.curve_cfg.preset_name == PRESETS[i]) {
            idx = (i + 1) % 4;
            break;
          }
        }
        state.curve_cfg.preset_name = PRESETS[idx];
        state.curve_cfg.points = get_curve_preset(state.curve_cfg.preset_name);
        state.curve_knot_selected = 0;
      } else if (ch == '\r' || ch == '\n' || ch == 'w' || ch == 'W') {
        state.curve_cfg.enabled = true;
        save_fan_curve_config(state.curve_cfg);
        last_curve_pct = -1;
        state.status_banner = "Curva " + state.curve_cfg.preset_name + " salva e ativada!";
        state.overlay = OverlayMode::None;
      } else if (ch == 'd' || ch == 'D') {
        state.curve_cfg.enabled = false;
        save_fan_curve_config(state.curve_cfg);
        state.status_banner = "Curva dinâmica desativada";
        state.overlay = OverlayMode::None;
      } else if (ch == 'j') {
        if (state.curve_knot_selected + 1 < static_cast<int>(state.curve_cfg.points.size()))
          ++state.curve_knot_selected;
      } else if (ch == 'k') {
        if (state.curve_knot_selected > 0) --state.curve_knot_selected;
      }
      continue;
    }

    if (state.overlay == OverlayMode::HelpSpecs) {
      if (ch == 'h' || ch == '?' || ch == 'q' || ch == 'Q') {
        if (ch == 'q' || ch == 'Q') g_running = 0;
        else state.overlay = OverlayMode::None;
      }
      continue;
    }

    // Main BTOP view keybindings
    if (ch == 'q' || ch == 'Q') {
      g_running = 0;
    } else if (ch == 'h' || ch == '?') {
      state.overlay = OverlayMode::HelpSpecs;
    } else if (ch == 'c' || ch == 'C') {
      state.overlay = OverlayMode::CurveEditor;
    } else if (ch == '/') {
      state.overlay = OverlayMode::FilterPrompt;
    } else if (ch == 'j') {
      ++state.proc_selected;
    } else if (ch == 'k' && state.proc_selected > 0) {
      --state.proc_selected;
    } else if (ch == 's' || ch == 'S') {
      if (state.sort_mode == ProcSortMode::Cpu) state.sort_mode = ProcSortMode::Mem;
      else if (state.sort_mode == ProcSortMode::Mem) state.sort_mode = ProcSortMode::Rss;
      else if (state.sort_mode == ProcSortMode::Rss) state.sort_mode = ProcSortMode::Pid;
      else state.sort_mode = ProcSortMode::Cpu;
    } else if (ch == 'r' || ch == 'R') {
      state.sort_reverse = !state.sort_reverse;
    } else if (ch == 'K') {
      auto procs = filtered_sorted_procs(state);
      if (!procs.empty()) {
        int sel = std::clamp(state.proc_selected, 0, static_cast<int>(procs.size()) - 1);
        state.kill_target_pid = procs[static_cast<size_t>(sel)].pid;
        state.kill_target_cmd = procs[static_cast<size_t>(sel)].command;
        state.overlay = OverlayMode::KillConfirm;
      }
    } else if (ch == '+' || ch == '>') {
      state.refresh_ms = std::clamp(state.refresh_ms - 250, 250, 5000);
    } else if (ch == '-' || ch == '<') {
      state.refresh_ms = std::clamp(state.refresh_ms + 250, 250, 5000);
    } else if (ch >= '1' && ch <= '5') {
      static constexpr const char* PROFS[] = {"eco", "quiet", "balanced", "performance", "turbo"};
      state.curve_cfg.enabled = false;
      auto r = apply_thermal_profile(PROFS[ch - '1'], env);
      state.status_banner = r.ok ? r.info_message : r.error_message;
      last_poll -= std::chrono::milliseconds(state.refresh_ms);
    } else if (ch == 'a' || ch == 'A') {
      state.curve_cfg.enabled = false;
      auto r = apply_fan_speed(0, 0, FanMode::Auto, env);
      state.status_banner = r.ok ? r.info_message : r.error_message;
      last_poll -= std::chrono::milliseconds(state.refresh_ms);
    } else if (ch == 'm' || ch == 'M') {
      state.curve_cfg.enabled = false;
      auto r = apply_fan_speed(100, 100, FanMode::Max, env);
      state.status_banner = r.ok ? r.info_message : r.error_message;
      last_poll -= std::chrono::milliseconds(state.refresh_ms);
    } else if (ch == 'v' || ch == 'V') {
      state.link_fans = !state.link_fans;
      state.status_banner = state.link_fans ? "Fans CPU+GPU vinculadas" : "Fans CPU/GPU independentes";
    } else if (ch == '[' || ch == ']') {
      state.curve_cfg.enabled = false;
      state.manual_cpu_pct = std::clamp(state.manual_cpu_pct + (ch == ']' ? 5 : -5), 20, 100);
      if (state.link_fans) state.manual_gpu_pct = state.manual_cpu_pct;
      auto r = apply_fan_speed(state.manual_cpu_pct, state.manual_gpu_pct, FanMode::Custom, env);
      state.status_banner = r.ok ? r.info_message : r.error_message;
    } else if (ch == '{' || ch == '}') {
      state.curve_cfg.enabled = false;
      state.manual_gpu_pct = std::clamp(state.manual_gpu_pct + (ch == '}' ? 5 : -5), 20, 100);
      if (state.link_fans) state.manual_cpu_pct = state.manual_gpu_pct;
      auto r = apply_fan_speed(state.manual_cpu_pct, state.manual_gpu_pct, FanMode::Custom, env);
      state.status_banner = r.ok ? r.info_message : r.error_message;
    } else if (ch == 'b' || ch == 'B') {
      bool next = !state.snap.settings.battery_limiter.value_or(true);
      auto r = apply_hardware_setting("batteryLimiter", next ? 1 : 0, env);
      state.status_banner = r.ok ? r.info_message : r.error_message;
      last_poll -= std::chrono::milliseconds(state.refresh_ms);
    } else if (ch == 'o' || ch == 'O') {
      bool next = !state.snap.settings.lcd_overdrive.value_or(true);
      auto r = apply_hardware_setting("lcdOverdrive", next ? 1 : 0, env);
      state.status_banner = r.ok ? r.info_message : r.error_message;
      last_poll -= std::chrono::milliseconds(state.refresh_ms);
    } else if (ch == 't' || ch == 'T') {
      bool next = !state.snap.settings.keyboard_backlight_timeout.value_or(false);
      auto r = apply_hardware_setting("keyboardBacklightTimeout", next ? 1 : 0, env);
      state.status_banner = r.ok ? r.info_message : r.error_message;
      last_poll -= std::chrono::milliseconds(state.refresh_ms);
    } else if (ch == 'u' || ch == 'U') {
      int cur = state.snap.settings.usb_power_off_charging.value_or(0);
      int next = (cur >= 30) ? 0 : (cur + 10);
      auto r = apply_hardware_setting("usbPowerOffCharging", next, env);
      state.status_banner = r.ok ? r.info_message : r.error_message;
      last_poll -= std::chrono::milliseconds(state.refresh_ms);
    }
  }

  return 0;
}

}  // namespace nitrosense
