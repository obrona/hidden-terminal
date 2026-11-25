#!/usr/bin/env bash
# Clean uninstall of everything scripts/install.sh set up.
#
#   sudo ./scripts/uninstall.sh            # system files, builds, niri config, Rust
#   sudo ./scripts/uninstall.sh --packages # ...and also the apt packages it installed
#
# Log out of the Niri session (use GNOME) before running.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "Run with sudo: sudo $0 [--packages]" >&2; exit 1; fi
U="${SUDO_USER:?run via sudo from your normal account, not as root}"
H="$(getent passwd "$U" | cut -d: -f6)"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
STATE="$H/.local/state/hidden-terminal"

echo "==> Removing system files"
rm -fv /usr/local/bin/hidden-terminal \
       /usr/local/share/applications/dev.hidden.Terminal.desktop \
       /usr/local/bin/niri /usr/local/bin/niri-session /usr/local/bin/xwayland-satellite \
       /usr/share/wayland-sessions/niri.desktop \
       /usr/share/xdg-desktop-portal/niri-portals.conf \
       /usr/local/lib/systemd/user/niri.service \
       /usr/local/lib/systemd/user/niri-shutdown.target

echo "==> Removing builds and niri data for $U"
rm -rf "$H/.local/share/niri" "$H/src/niri" "$H/src/xwayland-satellite" "$HERE/build"
rmdir "$H/src" 2>/dev/null || true

CFG="$H/.config/niri/config.kdl"
if [[ -f "$STATE/created-niri-config" ]]; then
  echo "==> Removing $H/.config/niri (written by install.sh)"
  rm -rf "$H/.config/niri"
elif [[ -f "$CFG" ]]; then
  echo "==> Removing Hidden Terminal include from your own $CFG"
  sed -i '\|^include ".*/niri/hidden-terminal\.kdl"$|d' "$CFG"
fi

if [[ -f "$STATE/installed-rust" && -x "$H/.cargo/bin/rustup" ]]; then
  echo "==> Removing Rust (rustup), which install.sh added"
  sudo -u "$U" "$H/.cargo/bin/rustup" self uninstall -y   # also removes ~/.cargo, ~/.rustup
  sed -i '\|^\. "\$HOME/\.cargo/env"$|d' "$H/.bashrc" "$H/.profile" 2>/dev/null || true
fi

if [[ "${1:-}" == "--packages" ]]; then
  echo "==> Removing apt packages installed for this project"
  PKGS=()
  if [[ -f "$STATE/apt-packages.txt" ]]; then
    mapfile -t PKGS < "$STATE/apt-packages.txt"
  fi
  # Packages installed by hand on 2026-11-25, before install.sh tracked them.
  mapfile -t -O "${#PKGS[@]}" PKGS < <(awk '/^Start-Date: 2026-11-25  12:01:15/{f=1} f&&/^Install:/{print; exit}' \
      /var/log/apt/history.log 2>/dev/null | sed 's/^Install: //; s/), /)\n/g' | sed 's/ (.*//; s/:.*//')
  mapfile -t PKGS < <(printf '%s\n' "${PKGS[@]}" | grep -v '^$' | sort -u)
  if [[ ${#PKGS[@]} -gt 0 ]]; then
    apt-get remove -y "${PKGS[@]}"
  else
    echo "No recorded packages; skipping."
  fi
fi

rm -rf "$STATE"
echo "Done. The project folder $HERE was left in place; delete it yourself if wanted."
