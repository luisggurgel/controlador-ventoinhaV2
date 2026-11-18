#include "control.hpp"
#include "nitro_types.hpp"
#include "sensors.hpp"
#include "tui.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

void print_help() {
  std::cout << "nitrosense-btop - Monitor Térmico e Controle EC estilo BTOP em C++20\n"
            << "Hardware: Acer Nitro V 15 (ANV15-52-52VN | i5-13420H | RTX 4060 8GB | Max 8600 RPM)\n\n"
            << "Uso:\n"
            << "  nitrosense-btop                          Inicia interface TUI estilo BTOP no terminal\n"
            << "  nitrosense-btop --tui [MS]               Inicia TUI com intervalo de atualização MS\n"
            << "  nitrosense-btop --daemon [MS]            Executa daemon em segundo plano para curva dinâmica\n"
            << "  nitrosense-btop --json [--sample-ms N]   Imprime snapshot JSON real de telemetria\n"
            << "  nitrosense-btop --set-fan CPU GPU MODE   Aplica fan_speed (MODE: auto|custom|max)\n"
            << "  nitrosense-btop --set-profile PROFILE    Aplica perfil (eco|quiet|balanced|performance|turbo)\n"
            << "  nitrosense-btop --set-setting KEY VAL    Grava ajuste sysfs (batteryLimiter|lcdOverdrive|keyboardBacklightTimeout|usbPowerOffCharging)\n"
            << "  nitrosense-btop --version                Mostra versão\n"
            << "  nitrosense-btop --help                   Mostra esta ajuda\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace nitrosense;

  const EnvOverrides env = load_env_overrides();

  if (argc == 1) {
    return run_btop_tui(env, 1000);
  }

  const std::string arg1 = argv[1];
  if (arg1 == "--help" || arg1 == "-h") {
    print_help();
    return 0;
  }

  if (arg1 == "--version" || arg1 == "-v") {
    std::cout << "nitrosense-btop 2.0.0 (C++20 / Acer Nitro ANV15-52-52VN)\n";
    return 0;
  }

  if (arg1 == "--tui") {
    int refresh_ms = 1000;
    if (argc >= 3) {
      try {
        refresh_ms = std::clamp(std::stoi(argv[2]), 200, 10000);
      } catch (...) {
      }
    }
    return run_btop_tui(env, refresh_ms);
  }

  if (arg1 == "--daemon") {
    int interval_ms = 2000;
    if (argc >= 3) {
      try {
        interval_ms = std::clamp(std::stoi(argv[2]), 500, 10000);
      } catch (...) {
      }
    }
    return run_fan_curve_daemon(env, interval_ms);
  }

  if (arg1 == "--json") {
    int sample_ms = 150;
    for (int i = 2; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--sample-ms" && i + 1 < argc) {
        try {
          sample_ms = std::clamp(std::stoi(argv[i + 1]), 0, 5000);
        } catch (...) {
        }
      }
    }
    TelemetryCollector collector(env);
    collector.prime_cpu_sample();
    if (sample_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sample_ms));
    }
    TelemetrySnapshot snap = collector.collect(60);
    std::cout << snapshot_to_json(snap) << "\n";
    return 0;
  }

  if (arg1 == "--set-fan") {
    if (argc < 5) {
      std::cerr << "Uso: --set-fan <cpu_pct> <gpu_pct> <auto|custom|max>\n";
      return 1;
    }
    int cpu = std::stoi(argv[2]);
    int gpu = std::stoi(argv[3]);
    std::string mode_str = argv[4];
    FanMode mode = FanMode::Custom;
    if (mode_str == "auto") mode = FanMode::Auto;
    else if (mode_str == "max") mode = FanMode::Max;

    ControlResult res = apply_fan_speed(cpu, gpu, mode, env);
    std::cout << control_result_to_json(res) << "\n";
    return res.ok ? 0 : 2;
  }

  if (arg1 == "--set-profile") {
    if (argc < 3) {
      std::cerr << "Uso: --set-profile <eco|quiet|balanced|performance|turbo>\n";
      return 1;
    }
    ControlResult res = apply_thermal_profile(argv[2], env);
    std::cout << control_result_to_json(res) << "\n";
    return res.ok ? 0 : 2;
  }

  if (arg1 == "--set-setting") {
    if (argc < 4) {
      std::cerr << "Uso: --set-setting <key> <value>\n";
      return 1;
    }
    int val = std::stoi(argv[3]);
    ControlResult res = apply_hardware_setting(argv[2], val, env);
    std::cout << control_result_to_json(res) << "\n";
    return res.ok ? 0 : 2;
  }

  std::cerr << "Argumento inválido: " << arg1 << "\n";
  print_help();
  return 1;
}
