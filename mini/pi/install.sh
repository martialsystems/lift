#!/usr/bin/env bash
# Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#
# One command on a Raspberry Pi 4/5 (64-bit Raspberry Pi OS, Bookworm or later):
#   git clone -b mini-pi <repo> lift && cd lift && ./mini/pi/install.sh
# Installs build deps, builds LIFT Mini, installs it to /usr/local/bin and
# starts it full screen at login. Re-run to update. --no-service skips autostart.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
SERVICE=1; [[ "${1:-}" == "--no-service" ]] && SERVICE=0

echo "== packages"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build git pkg-config \
  libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev

echo "== build (first build takes ~15-25 min on a Pi 5, longer on a Pi 4)"
JOBS=$(nproc); MEM_MB=$(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo)
(( MEM_MB < 3000 )) && JOBS=2   # 1-2 GB boards: fewer jobs or the linker runs out of memory
cmake -S . -B build-mini -G Ninja -DCMAKE_BUILD_TYPE=Release -DLIFT_APP_ONLY=ON -DLIFT_BUILD_MINI=ON
cmake --build build-mini --target LIFT_Mini -j "$JOBS"
sudo install -m 755 build-mini/LIFT_Mini_artefacts/Release/lift-mini /usr/local/bin/lift-mini

echo "== config"
sudo mkdir -p /etc/lift-mini
[[ -f /etc/lift-mini/controls.json ]] || sudo install -m 644 mini/pi/controls.example.json /etc/lift-mini/controls.json.example
sudo usermod -aG audio,gpio "$USER" || true
# real-time audio for the audio group (JUCE asks for RT priority on its audio thread)
echo '@audio - rtprio 70
@audio - memlock unlimited' | sudo tee /etc/security/limits.d/lift-mini.conf >/dev/null

if (( SERVICE )); then
  echo "== autostart"
  mkdir -p ~/.config/systemd/user
  install -m 644 mini/pi/lift-mini.service ~/.config/systemd/user/lift-mini.service
  systemctl --user daemon-reload
  systemctl --user enable lift-mini.service
  sudo raspi-config nonint do_boot_behaviour B4 2>/dev/null || true   # desktop autologin
  echo "Autostart on. Reboot (needed once for the audio/gpio groups), or: systemctl --user restart lift-mini"
fi
echo "Done. Run by hand: lift-mini            (window)   lift-mini --kiosk   (full screen)"
