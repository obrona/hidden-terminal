#!/usr/bin/env bash
# Full install of Hidden Terminal + the niri session, from nothing:
# apt dependencies, Rust, building the terminal, niri and xwayland-satellite,
# installing them system-wide and writing ~/.config/niri/config.kdl.
#
#   sudo ./scripts/install.sh
#
# Safe to re-run: finished steps are skipped or redone quickly.
# Undo everything with: sudo ./scripts/uninstall.sh [--packages]
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "Run with sudo: sudo $0" >&2; exit 1; fi
U="${SUDO_USER:?run via sudo from your normal account, not as root}"
H="$(getent passwd "$U" | cut -d: -f6)"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

NIRI_REPO="https://github.com/niri-wm/niri"
NIRI_REV="398026a8"            # first commit with block-out-from kind="hide"
XWS_REPO="https://github.com/Supreeeme/xwayland-satellite"
XWS_REV="v0.8.3"
NIRI_SRC="$H/src/niri"
XWS_SRC="$H/src/xwayland-satellite"

# Records what this script added, so uninstall.sh removes only that.
STATE="$H/.local/state/hidden-terminal"

# Run a command as the normal user, with Rust on PATH.
as_user() { sudo -u "$U" -H env PATH="$H/.cargo/bin:$PATH" bash -c "$1"; }

sudo -u "$U" mkdir -p "$STATE"

# ---------------------------------------------------------------- packages
PKGS=(build-essential meson ninja-build pkg-config git curl libgtk-4-dev
  libvte-2.91-gtk4-dev libpcre2-dev clang libclang-dev libudev-dev libgbm-dev
  libxkbcommon-dev libegl1-mesa-dev libwayland-dev libinput-dev libdbus-1-dev
  libsystemd-dev libseat-dev libpipewire-0.3-dev libpango1.0-dev
  libdisplay-info-dev libxcb-cursor-dev fuzzel swaylock)

MISSING=()
for p in "${PKGS[@]}"; do
  dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "ok installed" || MISSING+=("$p")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
  echo "==> Installing apt packages: ${MISSING[*]}"
  apt-get update
  apt-get install -y "${MISSING[@]}"
  # Remember them (appending, in case of several runs) for uninstall --packages.
  printf '%s\n' "${MISSING[@]}" | sudo -u "$U" tee -a "$STATE/apt-packages.txt" >/dev/null
else
  echo "==> apt packages already installed"
fi

# ---------------------------------------------------------------- Rust
if ! as_user "command -v cargo" >/dev/null; then
  echo "==> Installing Rust (rustup) for $U"
  as_user "curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y"
  sudo -u "$U" touch "$STATE/installed-rust"
else
  echo "==> Rust already installed"
fi

# ---------------------------------------------------------------- terminal
echo "==> Building hidden-terminal"
if [[ ! -d "$HERE/build" ]]; then as_user "meson setup '$HERE/build' '$HERE'"; fi
as_user "ninja -C '$HERE/build'"
meson install -C "$HERE/build" --no-rebuild

# ---------------------------------------------------------------- niri
fetch() {  # fetch <repo> <dir> <rev>
  if [[ ! -d "$2/.git" ]]; then
    as_user "mkdir -p '$(dirname "$2")' && git clone '$1' '$2'"
  fi
  as_user "cd '$2' && (git cat-file -e '$3^{commit}' 2>/dev/null || git fetch --tags origin) && git checkout -q '$3'"
}

echo "==> Building niri $NIRI_REV (takes several minutes the first time)"
fetch "$NIRI_REPO" "$NIRI_SRC" "$NIRI_REV"
as_user "cd '$NIRI_SRC' && cargo build --release"

echo "==> Installing niri"
install -Dm755 "$NIRI_SRC/target/release/niri"            /usr/local/bin/niri
install -Dm755 "$NIRI_SRC/resources/niri-session"         /usr/local/bin/niri-session
install -Dm644 "$NIRI_SRC/resources/niri.desktop"         /usr/share/wayland-sessions/niri.desktop
install -Dm644 "$NIRI_SRC/resources/niri-portals.conf"    /usr/share/xdg-desktop-portal/niri-portals.conf
install -Dm644 "$NIRI_SRC/resources/niri.service"         /usr/local/lib/systemd/user/niri.service
install -Dm644 "$NIRI_SRC/resources/niri-shutdown.target" /usr/local/lib/systemd/user/niri-shutdown.target

# ---------------------------------------------------------------- xwayland-satellite
echo "==> Building xwayland-satellite $XWS_REV (X11 app support in niri)"
fetch "$XWS_REPO" "$XWS_SRC" "$XWS_REV"
as_user "cd '$XWS_SRC' && cargo build --release"
install -Dm755 "$XWS_SRC/target/release/xwayland-satellite" /usr/local/bin/xwayland-satellite

# ---------------------------------------------------------------- niri config
CFG="$H/.config/niri/config.kdl"
INCLUDE="include \"$HERE/niri/hidden-terminal.kdl\""
if [[ ! -f "$CFG" ]]; then
  echo "==> Writing $CFG (niri default config, waybar off, Hidden Terminal rules)"
  sudo -u "$U" mkdir -p "$(dirname "$CFG")"
  sed 's|^\(\s*\)spawn-at-startup "waybar"|\1// spawn-at-startup "waybar"|' \
      "$NIRI_SRC/resources/default-config.kdl" | sudo -u "$U" tee "$CFG" >/dev/null
  printf '\n%s\n' "$INCLUDE" | sudo -u "$U" tee -a "$CFG" >/dev/null
  sudo -u "$U" touch "$STATE/created-niri-config"
elif ! grep -qF "$INCLUDE" "$CFG"; then
  echo "==> Adding Hidden Terminal include to existing $CFG"
  printf '\n%s\n' "$INCLUDE" | sudo -u "$U" tee -a "$CFG" >/dev/null
else
  echo "==> $CFG already includes Hidden Terminal rules"
fi
as_user "niri validate -c '$CFG'"

echo
echo "Done. Log out, click the gear icon on the login screen, choose 'Niri',"
echo "then press Super+T to open Hidden Terminal."
