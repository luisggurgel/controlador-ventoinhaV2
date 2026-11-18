#!/usr/bin/env bash
# ==============================================================================
# NitroSense-BTOP (C++20 Terminal App) - Instalador para Fedora Linux
# Notebook Alvo: Acer Nitro V 15 (ANV15-52-52VN)
# CPU: Intel Core i5-13420H | GPU: NVIDIA GeForce RTX 4060 (8 GB GDDR6)
# Rotação Máxima Oficial: 8600 RPM (CPU & GPU Fans)
# Baseado no módulo Linuwu-Sense (https://github.com/0x7375646F/Linuwu-Sense)
# ==============================================================================

set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo -e "${CYAN}======================================================================${NC}"
echo -e "${CYAN}   NitroSense-BTOP (C++20 TUI) - Acer Nitro V15 (ANV15-52-52VN)       ${NC}"
echo -e "${CYAN}   Aplicação de Terminal estilo btop • RTX 4060 • Até 8600 RPM        ${NC}"
echo -e "${CYAN}======================================================================${NC}"

if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}Erro: Execute como root: sudo ./install.sh${NC}"
  exit 1
fi

echo -e "\n[1/5] Instalando compilador C++20 e dependências do sistema (Fedora / Linux)..."
if command -v dnf &>/dev/null; then
    dnf install -y git make gcc gcc-c++ dkms kernel-devel
elif command -v pacman &>/dev/null; then
    pacman -S --needed --noconfirm base-devel git dkms linux-headers
elif command -v apt-get &>/dev/null; then
    apt-get update
    apt-get install -y build-essential g++ git dkms "linux-headers-$(uname -r)"
else
    echo -e "${YELLOW}Aviso: Gerenciador não reconhecido. Instale git, make, g++, dkms e kernel-devel.${NC}"
fi

echo -e "\n[2/5] Verificando módulo de kernel Linuwu-Sense (DKMS)..."
if dkms status -m linuwu_sense 2>/dev/null | grep -q "$(uname -r).*installed" && [ "$1" != "--rebuild-driver" ]; then
    echo -e "${GREEN}Módulo linuwu_sense já instalado no DKMS para kernel $(uname -r)!${NC}"
elif [ ! -d "/lib/modules/$(uname -r)/build" ]; then
    echo -e "${YELLOW}Aviso: Headers do kernel ($(uname -r)) não encontrados em /lib/modules/$(uname -r)/build.${NC}"
    echo -e "${YELLOW}Se o kernel foi atualizado pelo DNF recentemente, reinicie o notebook no novo kernel.${NC}"
else
    TEMP_DIR=$(mktemp -d)
    git clone --depth 1 https://github.com/0x7375646F/Linuwu-Sense.git "$TEMP_DIR/Linuwu-Sense"
    cd "$TEMP_DIR/Linuwu-Sense"
    make install || make LLVM=1 install || echo -e "${YELLOW}Aviso: Falha ao compilar módulo linuwu_sense.${NC}"
    cd "$PROJECT_DIR"
    rm -rf "$TEMP_DIR"
fi

echo -e "\n[3/5] Configurando quirk DMI ANV15-52 (ec_mmio_profile=1) e permissões sysfs..."
rm -f /etc/modprobe.d/linuwu_sense.conf
echo "blacklist acer_wmi" > /etc/modprobe.d/blacklist-acer_wmi.conf
echo "linuwu_sense" > /etc/modules-load.d/linuwu_sense.conf

depmod -a 2>/dev/null || true
modprobe -r acer_wmi 2>/dev/null || true
modprobe -r linuwu_sense 2>/dev/null || true

if ! modprobe linuwu_sense 2>/dev/null; then
    echo -e "${YELLOW}Nota: Não foi possível carregar linuwu_sense agora (verifique Secure Boot / chave MOK).${NC}"
else
    echo -e "${GREEN}Módulo linuwu_sense carregado via DMI (quirk_acer_nitro_anv15_52)!${NC}"
fi

cat << 'EOF' > /etc/tmpfiles.d/linuwu_sense.conf
f /sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi/nitro_sense/fan_speed 0666 root root
f /sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi/nitro_sense/battery_limiter 0666 root root
f /sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi/nitro_sense/battery_calibration 0666 root root
f /sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi/nitro_sense/usb_charging 0666 root root
f /sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi/nitro_sense/backlight_timeout 0666 root root
f /sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi/nitro_sense/lcd_override 0666 root root
f /sys/firmware/acpi/platform_profile 0666 root root
EOF

systemd-tmpfiles --create /etc/tmpfiles.d/linuwu_sense.conf 2>/dev/null || true
chmod 0666 /sys/firmware/acpi/platform_profile 2>/dev/null || true
chmod 0666 /sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi/nitro_sense/* 2>/dev/null || true
chmod 0666 /sys/devices/platform/acer-wmi/nitro_sense/* 2>/dev/null || true

cat << 'EOF' > /etc/sudoers.d/nitrosense-linux
ALL ALL=(root) NOPASSWD: /usr/bin/tee /sys/module/linuwu_sense/drivers/platform\:acer-wmi/acer-wmi/nitro_sense/*
ALL ALL=(root) NOPASSWD: /usr/bin/tee /sys/devices/platform/acer-wmi/nitro_sense/*
ALL ALL=(root) NOPASSWD: /usr/bin/tee /sys/firmware/acpi/platform_profile
ALL ALL=(root) NOPASSWD: /usr/bin/chmod 0666 /sys/firmware/acpi/platform_profile
EOF
chmod 0440 /etc/sudoers.d/nitrosense-linux

echo -e "\n[4/5] Compilando e testando nitrosense-btop (C++20 puro)..."
cd "$PROJECT_DIR"
make clean && make && make test
make install

mkdir -p /etc/nitrosense-btop
chmod 0777 /etc/nitrosense-btop

if [ -n "$SUDO_USER" ]; then
    chown -R "$SUDO_USER:$SUDO_USER" "$PROJECT_DIR"
fi

echo -e "\n[5/5] Configurando atalho de terminal (.desktop) e daemon opcional de curva..."
# Remove serviço web antigo caso exista de versões anteriores
systemctl disable --now nitrosense-linux.service 2>/dev/null || true
rm -f /etc/systemd/system/nitrosense-linux.service

cat << 'EOF' > /etc/systemd/system/nitrosense-fan-daemon.service
[Unit]
Description=NitroSense-BTOP Fan Curve Daemon (Acer ANV15-52-52VN)
After=multi-user.target linuwu_sense.service

[Service]
Type=simple
ExecStart=/usr/local/bin/nitrosense-btop --daemon 2000
Restart=on-failure
RestartSec=3
User=root

[Install]
WantedBy=multi-user.target
EOF

cat << 'EOF' > /usr/share/applications/nitrosense-btop.desktop
[Desktop Entry]
Name=NitroSense BTOP
Comment=Monitor Térmico e Controle EC estilo BTOP (Acer Nitro V15 ANV15-52)
Exec=/usr/local/bin/nitrosense-btop
Terminal=true
Type=Application
Categories=System;Monitor;Utility;
Icon=utilities-system-monitor
EOF

if command -v restorecon &>/dev/null; then
    restorecon -v /usr/local/bin/nitrosense-btop >/dev/null 2>&1 || true
fi

systemctl daemon-reload
systemctl enable --now nitrosense-fan-daemon.service

echo -e "\n${GREEN}======================================================================${NC}"
echo -e "${GREEN}   Instalação do nitrosense-btop concluída no Fedora Linux!           ${NC}"
echo -e "${GREEN}======================================================================${NC}"
echo -e "Execute no seu terminal: ${CYAN}nitrosense-btop${NC}"
echo -e "Ajuda e flags CLI:       ${CYAN}nitrosense-btop --help${NC}"
