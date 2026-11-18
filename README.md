# NitroSense-BTOP (C++20) — Acer Nitro V 15 (ANV15-52-52VN)

Aplicação nativa de terminal estilo **btop++** escrita em **C++20** (sem dependências externas, sem Node.js e sem servidor web) para monitoramento térmico real e controle físico do notebook **Acer Nitro V 15 (`ANV15-52-52VN`)** no **Fedora Linux**.

## Especificações Oficiais Suportadas (`ANV15-52-52VN`)
- **CPU:** Intel Core i5-13420H (8C/12T até 4.60 GHz)
- **GPU:** NVIDIA GeForce RTX 4060 Laptop GPU (8 GB GDDR6)
- **Memória / Tela:** 16 GB DDR5 5200 MHz • 15.6" IPS FHD 165 Hz
- **Ventoinhas:** Controle duplo CPU (`fan1_input`) + GPU (`fan2_input`) até **8600 RPM**
- **Quirk DMI / EC:** `quirk_acer_nitro_anv15_52` (`nitro_v4=1`, `ec_mmio_profile=1`, janela `0xFE708000` + `0x607`)

## Telemetria 100% Real (Zero Simulação)
- Leituras diretas de `/proc/stat` (uso total e por núcleo), `/proc/cpuinfo` (MHz), `/proc/meminfo`, `/proc/loadavg`, `/proc/uptime`, `/proc/<pid>` (processos vivos com filtro e `SIGTERM`), `/sys/class/hwmon` (`acer`, `coretemp`, `nvme`, `acpitz`), `intel-rapl:0`, `/sys/class/power_supply`, `nvidia-smi`, `/sys/firmware/acpi/platform_profile` e `nitro_sense/*`.
- Sensores ausentes exibem `N/A` honestamente (nunca valores fabricados).

## Instalação no Fedora Linux (Branch `btop-cpp`)
```bash
git clone https://github.com/luisggurgel/linuwu-sense-anv15.git
cd linuwu-sense-anv15
git checkout btop-cpp
sudo ./install.sh
```

Execute no terminal:
```bash
nitrosense-btop
```

## Atalhos no Terminal (`nitrosense-btop`)
- `[1..5]` — Perfis ACPI: **Eco**, **Quiet**, **Balanced**, **Performance**, **Turbo (8600 RPM)**
- `[a]` — Ventoinhas em **Auto (`0,0`)**
- `[m]` — Ventoinhas no **Máximo (`100,100` / até 8600 RPM)**
- `[` / `]` — Ajustar ventoinha CPU (`-5%` / `+5%`) | `{` / `}` — GPU (`-5%` / `+5%`) | `[v]` Vincular/Desvincular
- `[c]` — Abrir **Editor Interativo de Curva Dinâmica** (`silent`, `balanced`, `gaming`, `aggressive` + pontos editáveis salvos em `curve.conf`)
- `[b]` / `[o]` / `[t]` / `[u]` — `battery_limiter` (80%), `lcd_override` (165Hz), `backlight_timeout`, `usb_charging`
- `↑` / `↓` / `j` / `k` — Selecionar processo | `/` — Filtrar processos | `[s]` / `[r]` — Ordenar / Inverter | `[K]` — Enviar `SIGTERM`
- `[+]` / `[-]` — Taxa de atualização (`250ms`..`5000ms`) | `[h]` / `[F1]` — Diagnóstico & Ajuda | `[q]` — Sair

## CLI e Daemon
```bash
nitrosense-btop --json                # Snapshot JSON único com leituras reais
nitrosense-btop --set-fan 75 75 custom
nitrosense-btop --set-profile turbo
nitrosense-btop --daemon              # Serviço em segundo plano para curva dinâmica (curve.conf)
```

## Compilar e Testar Localmente
```bash
make clean && make && make test
```
