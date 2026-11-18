#include "sensors.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace nitrosense {

namespace {

std::string env_or(const char* name, const std::string& fallback) {
  const char* val = std::getenv(name);
  if (val && *val != '\0') return std::string(val);
  return fallback;
}

std::string trim_str(const std::string& s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::string to_lower_str(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool dir_exists(const std::string& p) {
  std::error_code ec;
  return fs::exists(p, ec) && fs::is_directory(p, ec);
}

bool file_exists(const std::string& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

std::optional<int> parse_int_opt(const std::optional<std::string>& raw) {
  if (!raw.has_value()) return std::nullopt;
  try {
    size_t idx = 0;
    int val = std::stoi(*raw, &idx);
    if (idx == 0) return std::nullopt;
    return val;
  } catch (...) {
    return std::nullopt;
  }
}

std::string find_hwmon_by_names(const std::string& hwmon_base,
                                const std::vector<std::string>& names) {
  if (!dir_exists(hwmon_base)) return "";
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(hwmon_base, ec)) {
    if (ec) break;
    const std::string name_path = (entry.path() / "name").string();
    auto raw_name = read_sysfs_trimmed(name_path);
    if (!raw_name.has_value()) continue;
    const std::string lower = to_lower_str(*raw_name);
    for (const auto& target : names) {
      if (lower == target) {
        return entry.path().string();
      }
    }
  }
  return "";
}

std::optional<double> read_millidegrees(const std::string& file_path) {
  auto raw = read_sysfs_trimmed(file_path);
  auto val = parse_int_opt(raw);
  if (!val.has_value() || *val <= 0) return std::nullopt;
  return std::round((static_cast<double>(*val) / 1000.0) * 10.0) / 10.0;
}

bool command_nvidia_smi_exists() {
  static int cached = -1;
  if (cached != -1) return cached == 1;
  const std::vector<std::string> candidates = {
      "/usr/bin/nvidia-smi",
      "/bin/nvidia-smi",
      "/usr/local/bin/nvidia-smi",
  };
  for (const auto& p : candidates) {
    if (::access(p.c_str(), X_OK) == 0) {
      cached = 1;
      return true;
    }
  }
  cached = 0;
  return false;
}

std::string read_os_pretty_name(const std::string& os_release_path) {
  std::ifstream in(os_release_path);
  if (!in.is_open()) return "Linux";
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("PRETTY_NAME=", 0) == 0) {
      std::string val = line.substr(12);
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        val = val.substr(1, val.size() - 2);
      }
      return val;
    }
  }
  return "Linux";
}

}  // namespace

EnvOverrides load_env_overrides() {
  EnvOverrides env;
  env.proc_dir = env_or("NITROSENSE_PROC_DIR", "/proc");
  env.hwmon_dir = env_or("NITROSENSE_HWMON_DIR", "/sys/class/hwmon");
  env.power_supply_dir = env_or("NITROSENSE_POWER_SUPPLY_DIR", "/sys/class/power_supply");
  env.platform_profile_path =
      env_or("NITROSENSE_PLATFORM_PROFILE", "/sys/firmware/acpi/platform_profile");
  env.platform_profile_choices_path =
      env_or("NITROSENSE_PLATFORM_PROFILE_CHOICES", "/sys/firmware/acpi/platform_profile_choices");
  env.rapl_energy_path =
      env_or("NITROSENSE_RAPL_PATH", "/sys/class/powercap/intel-rapl:0/energy_uj");
  env.dmi_product_path = env_or("NITROSENSE_DMI_PRODUCT", "/sys/class/dmi/id/product_name");
  env.os_release_path = env_or("NITROSENSE_OS_RELEASE", "/etc/os-release");
  env.sense_dir_override = env_or("NITROSENSE_SYSFS_DIR", "");
  env.disable_nvidia_smi = env_or("NITROSENSE_DISABLE_NVIDIA_SMI", "0") == "1";
  env.allow_sudo = env_or("NITROSENSE_NO_SUDO", "0") != "1";
  return env;
}

std::optional<std::string> read_sysfs_trimmed(const std::string& file_path) {
  if (file_path.empty()) return std::nullopt;
  std::ifstream in(file_path);
  if (!in.is_open()) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string content = trim_str(ss.str());
  if (content.empty() && in.bad()) return std::nullopt;
  return content;
}

SystemPaths detect_system_paths(const EnvOverrides& env) {
  SystemPaths paths;

  std::vector<std::string> candidates;
  if (!env.sense_dir_override.empty()) {
    candidates.push_back(env.sense_dir_override);
  } else {
    candidates = {
        "/sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi/nitro_sense",
        "/sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi/predator_sense",
        "/sys/devices/platform/acer-wmi/nitro_sense",
        "/sys/devices/platform/acer-wmi/predator_sense",
        "/sys/bus/platform/devices/acer-wmi/nitro_sense",
        "/sys/bus/platform/devices/acer-wmi/predator_sense",
    };
  }

  for (const auto& c : candidates) {
    if (dir_exists(c)) {
      paths.sense_dir = c;
      break;
    }
  }

  if (!paths.sense_dir.empty()) {
    auto join_if = [&](const char* attr) {
      std::string p = (fs::path(paths.sense_dir) / attr).string();
      return file_exists(p) ? p : std::string();
    };
    paths.fan_speed_path = join_if("fan_speed");
    paths.battery_limiter_path = join_if("battery_limiter");
    paths.lcd_override_path = join_if("lcd_override");
    paths.backlight_timeout_path = join_if("backlight_timeout");
    paths.usb_charging_path = join_if("usb_charging");
    paths.battery_calibration_path = join_if("battery_calibration");
  }

  if (file_exists(env.platform_profile_path)) {
    paths.platform_profile_path = env.platform_profile_path;
  }
  if (file_exists(env.platform_profile_choices_path)) {
    paths.platform_profile_choices_path = env.platform_profile_choices_path;
  }

  paths.hwmon_acer_dir = find_hwmon_by_names(env.hwmon_dir, {"acer"});
  paths.hwmon_cpu_dir = find_hwmon_by_names(env.hwmon_dir, {"coretemp", "k10temp", "acpitz", "zenpower"});
  return paths;
}

TelemetryCollector::TelemetryCollector(EnvOverrides env) : env_(std::move(env)) {
  uid_user_cache_[0] = "root";
}

std::string TelemetryCollector::resolve_username(int uid) {
  auto it = uid_user_cache_.find(uid);
  if (it != uid_user_cache_.end()) return it->second;
  struct passwd* pw = ::getpwuid(static_cast<uid_t>(uid));
  std::string name = pw && pw->pw_name ? std::string(pw->pw_name) : std::to_string(uid);
  uid_user_cache_[uid] = name;
  return name;
}

void TelemetryCollector::prime_cpu_sample() {
  CpuSnapshot dummy_cpu;
  read_cpu_and_load(dummy_cpu);
  std::vector<ProcessInfo> dummy_procs;
  read_processes(dummy_procs, 1024, 1);
  auto rapl_raw = read_sysfs_trimmed(env_.rapl_energy_path);
  if (rapl_raw.has_value()) {
    try {
      prev_rapl_uj_ = std::stoull(*rapl_raw);
      prev_rapl_time_ = std::chrono::steady_clock::now();
    } catch (...) {
    }
  }
}

void TelemetryCollector::read_cpu_and_load(CpuSnapshot& out) {
  // CPU model name & core freqs from /proc/cpuinfo
  {
    std::ifstream cpuinfo((fs::path(env_.proc_dir) / "cpuinfo").string());
    std::string line;
    while (std::getline(cpuinfo, line)) {
      if (out.model_name.empty() && line.rfind("model name", 0) == 0) {
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
          out.model_name = trim_str(line.substr(colon + 1));
        }
      } else if (line.rfind("cpu MHz", 0) == 0) {
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
          try {
            double mhz = std::stod(trim_str(line.substr(colon + 1)));
            out.core_freqs_mhz.push_back(static_cast<int>(std::lround(mhz)));
          } catch (...) {
          }
        }
      }
    }
  }

  // Load average from /proc/loadavg
  {
    auto raw_load = read_sysfs_trimmed((fs::path(env_.proc_dir) / "loadavg").string());
    if (raw_load.has_value()) {
      std::istringstream iss(*raw_load);
      iss >> out.load1 >> out.load5 >> out.load15;
    }
  }

  // Uptime from /proc/uptime
  {
    auto raw_up = read_sysfs_trimmed((fs::path(env_.proc_dir) / "uptime").string());
    if (raw_up.has_value()) {
      std::istringstream iss(*raw_up);
      double up_sec = 0.0;
      if (iss >> up_sec) {
        out.uptime_sec = static_cast<uint64_t>(up_sec);
      }
    }
  }

  // Per-core & total utilization from /proc/stat
  std::ifstream stat_file((fs::path(env_.proc_dir) / "stat").string());
  if (!stat_file.is_open()) return;

  std::vector<CpuTickSample> current_ticks;
  std::string line;
  while (std::getline(stat_file, line)) {
    if (line.rfind("cpu", 0) != 0) continue;
    std::istringstream iss(line);
    std::string label;
    iss >> label;
    if (label != "cpu" && !(label.size() > 3 && std::isdigit(static_cast<unsigned char>(label[3])))) {
      continue;
    }
    uint64_t vals[10] = {0};
    int idx = 0;
    uint64_t v = 0;
    while (idx < 10 && (iss >> v)) {
      vals[idx++] = v;
    }
    uint64_t idle = vals[3] + vals[4];
    uint64_t total = 0;
    for (int i = 0; i < idx; ++i) total += vals[i];
    current_ticks.push_back(CpuTickSample{idle, total});
  }

  if (!current_ticks.empty()) {
    out.core_count = current_ticks.size() > 1
                         ? static_cast<int>(current_ticks.size() - 1)
                         : static_cast<int>(out.core_freqs_mhz.size());
    if (out.core_count <= 0) {
      long sc = ::sysconf(_SC_NPROCESSORS_ONLN);
      out.core_count = sc > 0 ? static_cast<int>(sc) : 1;
    }
    cpu_core_count_cache_ = out.core_count;

    if (prev_cpu_ticks_.size() == current_ticks.size()) {
      const auto& p0 = prev_cpu_ticks_[0];
      const auto& c0 = current_ticks[0];
      const uint64_t diff_idle = c0.idle >= p0.idle ? c0.idle - p0.idle : 0;
      const uint64_t diff_total = c0.total >= p0.total ? c0.total - p0.total : 0;
      if (diff_total > 0) {
        double pct = (1.0 - static_cast<double>(diff_idle) / static_cast<double>(diff_total)) * 100.0;
        pct = std::clamp(pct, 0.0, 100.0);
        out.total_usage_pct = std::round(pct * 10.0) / 10.0;
      }

      for (size_t i = 1; i < current_ticks.size(); ++i) {
        const uint64_t di =
            current_ticks[i].idle >= prev_cpu_ticks_[i].idle ? current_ticks[i].idle - prev_cpu_ticks_[i].idle : 0;
        const uint64_t dt = current_ticks[i].total >= prev_cpu_ticks_[i].total
                                ? current_ticks[i].total - prev_cpu_ticks_[i].total
                                : 0;
        double cpct = 0.0;
        if (dt > 0) {
          cpct = (1.0 - static_cast<double>(di) / static_cast<double>(dt)) * 100.0;
          cpct = std::clamp(cpct, 0.0, 100.0);
        }
        out.core_usages_pct.push_back(std::round(cpct * 10.0) / 10.0);
      }
    } else if (current_ticks.size() > 1) {
      out.core_usages_pct.assign(current_ticks.size() - 1, 0.0);
    }

    prev_cpu_ticks_ = std::move(current_ticks);
  }

  if (out.model_name.empty()) {
    out.model_name = CPU_SPEC_NAME;
  }
}

void TelemetryCollector::read_memory(MemorySnapshot& out) {
  std::ifstream in((fs::path(env_.proc_dir) / "meminfo").string());
  if (!in.is_open()) return;

  uint64_t mem_free = 0;
  uint64_t reclaimable = 0;
  uint64_t swap_free = 0;
  bool has_available = false;

  std::string line;
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    std::string key;
    uint64_t val = 0;
    if (!(iss >> key >> val)) continue;
    if (key == "MemTotal:") out.total_kb = val;
    else if (key == "MemFree:") mem_free = val;
    else if (key == "MemAvailable:") {
      out.available_kb = val;
      has_available = true;
    } else if (key == "Buffers:") out.buffers_kb = val;
    else if (key == "Cached:") out.cached_kb = val;
    else if (key == "SReclaimable:") reclaimable = val;
    else if (key == "SwapTotal:") out.swap_total_kb = val;
    else if (key == "SwapFree:") swap_free = val;
  }

  if (!has_available) {
    out.available_kb = mem_free + out.buffers_kb + out.cached_kb + reclaimable;
  }
  out.used_kb = out.total_kb >= out.available_kb ? (out.total_kb - out.available_kb) : 0;
  out.swap_used_kb = out.swap_total_kb >= swap_free ? (out.swap_total_kb - swap_free) : 0;
}

void TelemetryCollector::read_processes(std::vector<ProcessInfo>& out, uint64_t total_mem_kb, int limit) {
  out.clear();
  uint64_t total_jiffies = 0;
  {
    std::ifstream stat_f((fs::path(env_.proc_dir) / "stat").string());
    std::string first;
    if (std::getline(stat_f, first) && first.rfind("cpu ", 0) == 0) {
      std::istringstream iss(first);
      std::string lbl;
      iss >> lbl;
      uint64_t v = 0;
      while (iss >> v) total_jiffies += v;
    }
  }

  const uint64_t delta_total_jiffies =
      (prev_total_jiffies_ > 0 && total_jiffies > prev_total_jiffies_)
          ? (total_jiffies - prev_total_jiffies_)
          : 0;
  const long page_size_kb = std::max(1L, ::sysconf(_SC_PAGESIZE) / 1024L);

  std::map<int, ProcTickSample> next_ticks;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(env_.proc_dir, ec)) {
    if (ec) break;
    const std::string fname = entry.path().filename().string();
    if (fname.empty() || !std::all_of(fname.begin(), fname.end(),
                                      [](unsigned char c) { return std::isdigit(c) != 0; })) {
      continue;
    }
    int pid = 0;
    try {
      pid = std::stoi(fname);
    } catch (...) {
      continue;
    }

    std::ifstream pstat((entry.path() / "stat").string());
    if (!pstat.is_open()) continue;
    std::string stat_line;
    if (!std::getline(pstat, stat_line)) continue;

    size_t lparen = stat_line.find('(');
    size_t rparen = stat_line.rfind(')');
    if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) continue;

    std::string comm = stat_line.substr(lparen + 1, rparen - lparen - 1);
    std::istringstream rest(stat_line.substr(rparen + 2));

    char state = '?';
    if (!(rest >> state)) continue;
    // Fields after state in /proc/<pid>/stat:
    // 4:ppid 5:pgrp 6:session 7:tty_nr 8:tpgid 9:flags 10:minflt 11:cminflt 12:majflt 13:cmajflt
    // 14:utime 15:stime 16:cutime 17:cstime 18:priority 19:nice 20:num_threads 21:itrealvalue 22:starttime 23:vsize 24:rss
    std::string tok;
    uint64_t utime = 0, stime = 0, rss_pages = 0;
    int threads = 1;
    for (int f = 4; f <= 24; ++f) {
      if (!(rest >> tok)) break;
      try {
        if (f == 14) utime = std::stoull(tok);
        else if (f == 15) stime = std::stoull(tok);
        else if (f == 20) threads = std::max(1, std::stoi(tok));
        else if (f == 24) rss_pages = std::stoull(tok);
      } catch (...) {
      }
    }

    const uint64_t p_ticks = utime + stime;
    next_ticks[pid] = ProcTickSample{p_ticks};

    double cpu_pct = 0.0;
    auto prev_it = prev_proc_ticks_.find(pid);
    if (prev_it != prev_proc_ticks_.end() && delta_total_jiffies > 0 && p_ticks >= prev_it->second.utime_stime) {
      const uint64_t dp = p_ticks - prev_it->second.utime_stime;
      cpu_pct = (static_cast<double>(dp) / static_cast<double>(delta_total_jiffies)) *
                100.0 * static_cast<double>(std::max(1, cpu_core_count_cache_));
      cpu_pct = std::min(cpu_pct, 100.0 * std::max(1, cpu_core_count_cache_));
    }

    const double mem_kb = static_cast<double>(rss_pages) * static_cast<double>(page_size_kb);
    const double mem_mb = mem_kb / 1024.0;
    const double mem_pct = total_mem_kb > 0 ? (mem_kb / static_cast<double>(total_mem_kb)) * 100.0 : 0.0;

    struct stat st{};
    std::string user = "unknown";
    if (::stat(entry.path().c_str(), &st) == 0) {
      user = resolve_username(static_cast<int>(st.st_uid));
    }

    // Prefer short executable name from cmdline if readable, keeping comm fallback
    std::string cmd = comm;
    {
      std::ifstream cmdf((entry.path() / "cmdline").string(), std::ios::binary);
      if (cmdf.is_open()) {
        std::string raw_cmd;
        std::getline(cmdf, raw_cmd);
        if (!raw_cmd.empty()) {
          for (char& ch : raw_cmd) {
            if (ch == '\0') ch = ' ';
          }
          std::string cleaned = trim_str(raw_cmd);
          if (!cleaned.empty()) {
            if (cleaned.size() > 64) cleaned = cleaned.substr(0, 61) + "...";
            cmd = cleaned;
          }
        }
      }
    }

    ProcessInfo info;
    info.pid = pid;
    info.user = user;
    info.state = state;
    info.cpu_pct = std::round(cpu_pct * 10.0) / 10.0;
    info.mem_pct = std::round(mem_pct * 10.0) / 10.0;
    info.mem_mb = std::round(mem_mb * 10.0) / 10.0;
    info.threads = threads;
    info.command = cmd;
    out.push_back(std::move(info));
  }

  prev_total_jiffies_ = total_jiffies;
  prev_proc_ticks_ = std::move(next_ticks);

  std::sort(out.begin(), out.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
    if (a.cpu_pct != b.cpu_pct) return a.cpu_pct > b.cpu_pct;
    if (a.mem_mb != b.mem_mb) return a.mem_mb > b.mem_mb;
    return a.pid < b.pid;
  });

  if (limit > 0 && static_cast<int>(out.size()) > limit) {
    out.resize(static_cast<size_t>(limit));
  }
}

TelemetrySnapshot TelemetryCollector::collect(int process_limit) {
  TelemetrySnapshot snap;
  snap.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  char host_buf[256] = {0};
  if (::gethostname(host_buf, sizeof(host_buf) - 1) == 0) {
    snap.hostname = host_buf;
  } else {
    snap.hostname = "localhost";
  }

  struct utsname uts{};
  if (::uname(&uts) == 0) {
    snap.kernel_release = uts.release;
  }

  snap.os_name = read_os_pretty_name(env_.os_release_path);
  snap.dmi_product_name = read_sysfs_trimmed(env_.dmi_product_path).value_or("");

  snap.paths = detect_system_paths(env_);
  snap.is_hardware_detected = !snap.paths.sense_dir.empty();

  // Read OS CPU & Load & Memory
  read_cpu_and_load(snap.cpu);
  read_memory(snap.memory);
  read_processes(snap.processes, snap.memory.total_kb, process_limit);

  // Real CPU Temperature (prefer acer WMI hwmon temp1_input, else coretemp/k10temp/acpitz)
  if (!snap.paths.hwmon_acer_dir.empty()) {
    auto t = read_millidegrees((fs::path(snap.paths.hwmon_acer_dir) / "temp1_input").string());
    if (t.has_value()) {
      snap.cpu.temp_c = t;
      snap.cpu.temp_source = "hwmon:acer/temp1_input";
    }
  }
  if (!snap.cpu.temp_c.has_value() && !snap.paths.hwmon_cpu_dir.empty()) {
    auto t = read_millidegrees((fs::path(snap.paths.hwmon_cpu_dir) / "temp1_input").string());
    if (t.has_value()) {
      snap.cpu.temp_c = t;
      auto chip = read_sysfs_trimmed((fs::path(snap.paths.hwmon_cpu_dir) / "name").string()).value_or("hwmon");
      snap.cpu.temp_source = "hwmon:" + chip + "/temp1_input";
    }
  }

  // Real CPU Power via Intel RAPL
  {
    auto rapl_raw = read_sysfs_trimmed(env_.rapl_energy_path);
    if (rapl_raw.has_value()) {
      try {
        const uint64_t current_uj = std::stoull(*rapl_raw);
        const auto now = std::chrono::steady_clock::now();
        if (prev_rapl_uj_.has_value()) {
          const double dt = std::chrono::duration<double>(now - prev_rapl_time_).count();
          if (dt > 0.05 && current_uj >= *prev_rapl_uj_) {
            const double watts = (static_cast<double>(current_uj - *prev_rapl_uj_) / 1000000.0) / dt;
            if (watts >= 0.0 && watts < 200.0) {
              snap.cpu.power_watts = std::round(watts * 10.0) / 10.0;
            }
          }
        }
        prev_rapl_uj_ = current_uj;
        prev_rapl_time_ = now;
      } catch (...) {
      }
    }
  }

  // Real GPU telemetry (acer hwmon temp2_input/temp3_input + nvidia-smi)
  snap.gpu.name = GPU_SPEC_NAME;
  if (!snap.paths.hwmon_acer_dir.empty()) {
    auto t2 = read_millidegrees((fs::path(snap.paths.hwmon_acer_dir) / "temp2_input").string());
    if (!t2.has_value()) {
      t2 = read_millidegrees((fs::path(snap.paths.hwmon_acer_dir) / "temp3_input").string());
    }
    if (t2.has_value()) {
      snap.gpu.temp_c = t2;
      snap.gpu.source = "hwmon:acer";
    }
  }

  if (!env_.disable_nvidia_smi && command_nvidia_smi_exists()) {
    FILE* pipe = ::popen(
        "nvidia-smi --query-gpu=name,temperature.gpu,utilization.gpu,power.draw,memory.used,memory.total,clocks.gr "
        "--format=csv,noheader,nounits 2>/dev/null",
        "r");
    if (pipe) {
      char buf[512] = {0};
      if (::fgets(buf, sizeof(buf), pipe) != nullptr) {
        std::string line = trim_str(buf);
        std::vector<std::string> fields;
        std::istringstream iss(line);
        std::string part;
        while (std::getline(iss, part, ',')) {
          fields.push_back(trim_str(part));
        }
        if (fields.size() >= 7) {
          snap.gpu.nvidia_smi_available = true;
          if (!fields[0].empty() && fields[0] != "[N/A]") snap.gpu.name = fields[0];
          auto try_double = [](const std::string& s) -> std::optional<double> {
            try {
              if (s.empty() || s.find("N/A") != std::string::npos) return std::nullopt;
              return std::stod(s);
            } catch (...) {
              return std::nullopt;
            }
          };
          auto try_int = [](const std::string& s) -> std::optional<int> {
            try {
              if (s.empty() || s.find("N/A") != std::string::npos) return std::nullopt;
              return std::stoi(s);
            } catch (...) {
              return std::nullopt;
            }
          };
          if (auto t = try_double(fields[1]); t.has_value() && *t > 0.0) snap.gpu.temp_c = t;
          if (auto u = try_double(fields[2]); u.has_value()) snap.gpu.usage_pct = u;
          if (auto w = try_double(fields[3]); w.has_value()) snap.gpu.power_watts = std::round(*w * 10.0) / 10.0;
          if (auto vu = try_int(fields[4]); vu.has_value()) snap.gpu.vram_used_mb = vu;
          if (auto vt = try_int(fields[5]); vt.has_value()) snap.gpu.vram_total_mb = vt;
          if (auto clk = try_int(fields[6]); clk.has_value()) snap.gpu.clock_mhz = clk;
          snap.gpu.source = "nvidia-smi";
        }
      }
      ::pclose(pipe);
    }
  }

  // Real Fan RPMs from acer hwmon or any exposed hwmon fan*_input
  if (!snap.paths.hwmon_acer_dir.empty()) {
    auto f1 = parse_int_opt(read_sysfs_trimmed((fs::path(snap.paths.hwmon_acer_dir) / "fan1_input").string()));
    auto f2 = parse_int_opt(read_sysfs_trimmed((fs::path(snap.paths.hwmon_acer_dir) / "fan2_input").string()));
    if (f1.has_value()) snap.fans.cpu_rpm = std::max(0, *f1);
    if (f2.has_value()) snap.fans.gpu_rpm = std::max(0, *f2);
    if (f1.has_value() || f2.has_value()) snap.fans.rpm_source = "hwmon:acer";
  } else if (dir_exists(env_.hwmon_dir)) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(env_.hwmon_dir, ec)) {
      if (ec) break;
      auto f1 = parse_int_opt(read_sysfs_trimmed((entry.path() / "fan1_input").string()));
      auto f2 = parse_int_opt(read_sysfs_trimmed((entry.path() / "fan2_input").string()));
      if (f1.has_value()) snap.fans.cpu_rpm = std::max(0, *f1);
      if (f2.has_value()) snap.fans.gpu_rpm = std::max(0, *f2);
      if (f1.has_value() || f2.has_value()) {
        auto chip = read_sysfs_trimmed((entry.path() / "name").string()).value_or(entry.path().filename().string());
        snap.fans.rpm_source = "hwmon:" + chip;
        break;
      }
    }
  }

  // Real fan target % & hardware toggles from Linuwu-Sense nitro_sense
  if (!snap.paths.fan_speed_path.empty()) {
    auto raw_fan = read_sysfs_trimmed(snap.paths.fan_speed_path);
    if (raw_fan.has_value()) {
      FanPair pair = parse_fan_speed_sysfs(*raw_fan);
      snap.fans.cpu_target_pct = pair.cpu;
      snap.fans.gpu_target_pct = pair.gpu;
      if (pair.cpu == 100 && pair.gpu == 100) {
        snap.fans.mode = FanMode::Max;
      } else if (pair.cpu == 0 && pair.gpu == 0) {
        snap.fans.mode = FanMode::Auto;
      } else {
        snap.fans.mode = FanMode::Custom;
      }
    }
  }

  auto read_bool_attr = [](const std::string& p) -> std::optional<bool> {
    auto r = read_sysfs_trimmed(p);
    if (!r.has_value()) return std::nullopt;
    if (*r == "1") return true;
    if (*r == "0") return false;
    return std::nullopt;
  };

  snap.settings.battery_limiter = read_bool_attr(snap.paths.battery_limiter_path);
  snap.settings.lcd_overdrive = read_bool_attr(snap.paths.lcd_override_path);
  snap.settings.keyboard_backlight_timeout = read_bool_attr(snap.paths.backlight_timeout_path);
  snap.settings.usb_power_off_charging = parse_int_opt(read_sysfs_trimmed(snap.paths.usb_charging_path));
  snap.power.battery_limiter_active = snap.settings.battery_limiter;

  // Real ACPI platform_profile & choices
  if (!snap.paths.platform_profile_path.empty()) {
    auto raw_prof = read_sysfs_trimmed(snap.paths.platform_profile_path);
    if (raw_prof.has_value() && !raw_prof->empty()) {
      snap.active_profile_raw = *raw_prof;
      snap.active_profile = map_platform_profile_to_id(*raw_prof);
    }
  }
  if (!snap.paths.platform_profile_choices_path.empty()) {
    auto raw_choices = read_sysfs_trimmed(snap.paths.platform_profile_choices_path);
    if (raw_choices.has_value()) {
      std::istringstream iss(*raw_choices);
      std::string choice;
      while (iss >> choice) {
        snap.platform_profile_choices.push_back(choice);
      }
    }
  }

  // Real Power Supply (AC / Battery)
  if (dir_exists(env_.power_supply_dir)) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(env_.power_supply_dir, ec)) {
      if (ec) break;
      const std::string name = entry.path().filename().string();
      const std::string type = to_lower_str(read_sysfs_trimmed((entry.path() / "type").string()).value_or(""));
      if (type == "mains" || name.rfind("AC", 0) == 0 || name.rfind("ADP", 0) == 0) {
        auto online = read_sysfs_trimmed((entry.path() / "online").string());
        if (online == "1") snap.power.ac_connected = true;
        else if (online == "0") snap.power.ac_connected = false;
      } else if (type == "battery" || name.rfind("BAT", 0) == 0) {
        auto cap = parse_int_opt(read_sysfs_trimmed((entry.path() / "capacity").string()));
        if (cap.has_value()) snap.power.battery_pct = std::clamp(*cap, 0, 100);
        auto st = read_sysfs_trimmed((entry.path() / "status").string());
        if (st.has_value()) {
          snap.power.battery_status = *st;
          snap.power.battery_charging = (to_lower_str(*st) == "charging");
        }
      }
    }
  }

  snap.has_real_sensors = snap.cpu.temp_c.has_value() || snap.fans.cpu_rpm.has_value() ||
                          snap.gpu.nvidia_smi_available || !snap.paths.platform_profile_path.empty();

  if (snap.is_hardware_detected) {
    snap.driver_status = "active";
  } else if (snap.has_real_sensors) {
    snap.driver_status = "partial_linux_sensors";
  } else {
    snap.driver_status = "no_acer_ec_hardware";
  }

  return snap;
}

}  // namespace nitrosense
