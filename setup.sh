#!/bin/bash
# Summerboard Debian setup — bare DE-less server → Xorg + Mesa glamor + autostart.
# Reference install: HP Stream x360 (Debian 13), modesetting/glamor/DRI3, matchbox-keyboard.
#
# Usage (as the desktop user, with sudo):
#   git clone https://github.com/teutekeune/summerboard.git
#   cd summerboard && ./setup.sh
#   sudo reboot

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="${SUDO_USER:-${USER:-$(id -un)}}"
if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
  USER_NAME="$SUDO_USER"
fi
USER_HOME="$(getent passwd "$USER_NAME" | cut -d: -f6)"
if [ -z "$USER_HOME" ] || [ ! -d "$USER_HOME" ]; then
  echo "setup: cannot resolve home for user '$USER_NAME'" >&2
  exit 1
fi

ASSETS_DIR="$USER_HOME/summerboard"
BIN_DIR=/usr/local/bin
SHARE_DIR=/usr/local/share/summerboard
XORG_D=/etc/X11/xorg.conf.d
GETTY_D=/etc/systemd/system/getty@tty1.service.d

log()  { printf '==> %s\n' "$*"; }
warn() { printf 'setup: warning: %s\n' "$*" >&2; }
die()  { printf 'setup: %s\n' "$*" >&2; exit 1; }

need_sudo() {
  if [ "$(id -u)" -eq 0 ]; then
    SUDO=
  elif command -v sudo >/dev/null 2>&1; then
    SUDO=sudo
    $SUDO -v || die "sudo authentication failed"
  else
    die "run as root, or install sudo and re-run as the desktop user"
  fi
}

run() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    $SUDO "$@"
  fi
}

as_user() {
  if [ "$(id -u)" -eq 0 ] && [ "$USER_NAME" != root ]; then
    runuser -u "$USER_NAME" -- "$@"
  else
    "$@"
  fi
}

# ---------- packages ----------
install_packages() {
  log "Installing Debian packages (Xorg, Mesa, libinput, matchbox-keyboard, build deps)"
  export DEBIAN_FRONTEND=noninteractive
  run apt-get update -y
  # modesetting_drv ships inside xserver-xorg-core (no separate package on Debian).
  run apt-get install -y --no-install-recommends \
    build-essential \
    pkg-config \
    xserver-xorg \
    xserver-xorg-core \
    xserver-xorg-legacy \
    xserver-xorg-input-libinput \
    xserver-xorg-input-evdev \
    xinit \
    x11-xserver-utils \
    xinput \
    libx11-dev \
    libxext-dev \
    libxfixes-dev \
    libegl1-mesa-dev \
    libgl-dev \
    libgles-dev \
    libfreetype-dev \
    libdrm2 \
    mesa-utils \
    fonts-dejavu \
    fonts-liberation \
    matchbox-keyboard \
    alsa-utils \
    pciutils \
    udev
}

# ---------- GPU probe ----------
# Convert lspci slot "00:02.0" → Xorg BusID "PCI:0:2:0"
pci_to_busid() {
  local slot="$1"
  local bus dev fun
  bus=$(printf '%s' "$slot" | cut -d: -f1)
  dev=$(printf '%s' "$slot" | cut -d: -f2 | cut -d. -f1)
  fun=$(printf '%s' "$slot" | cut -d. -f2)
  # strip leading zeros / convert hex
  bus=$((16#$bus))
  dev=$((16#$dev))
  fun=$((16#$fun))
  printf 'PCI:%d:%d:%d' "$bus" "$dev" "$fun"
}

probe_gpu() {
  log "Probing GPU (BusID + /dev/dri)"
  BUSID=
  KMSDEV=

  if [ -e /dev/dri/card0 ]; then
    KMSDEV=/dev/dri/card0
  elif [ -e /dev/dri/card1 ]; then
    KMSDEV=/dev/dri/card1
  else
    warn "no /dev/dri/card* yet — modesetting config will still be written; check after reboot"
    KMSDEV=/dev/dri/card0
  fi

  # Prefer the DRM device behind KMSDEV
  local pci_path=
  if [ -e "$KMSDEV" ]; then
    local sys
    sys=$(readlink -f "/sys/class/drm/$(basename "$KMSDEV")/device" 2>/dev/null || true)
    if [ -n "$sys" ]; then
      pci_path=$(basename "$(dirname "$sys")" 2>/dev/null || true)
      # sys may already be …/0000:00:02.0
      case "$sys" in
        */0000:*|*/0001:*|*/0002:*) pci_path=$(basename "$sys") ;;
      esac
    fi
  fi

  local slot=
  if [ -n "$pci_path" ]; then
    slot=${pci_path#*:}   # drop domain → 00:02.0
  fi
  if [ -z "$slot" ] && command -v lspci >/dev/null 2>&1; then
    slot=$(lspci 2>/dev/null | awk '/VGA compatible controller|Display controller|3D controller/{print $1; exit}')
  fi
  if [ -n "$slot" ]; then
    BUSID=$(pci_to_busid "$slot")
  else
    warn "could not probe PCI BusID — defaulting to PCI:0:2:0 (Intel iGPU common)"
    BUSID=PCI:0:2:0
  fi

  log "  BusID=$BUSID  kmsdev=$KMSDEV"
}

probe_touchscreen() {
  # Default matches the HP Stream SYNA7813 reference; catchall InputClass covers others.
  TOUCH_PRODUCT=SYNA7813
  local udev_name=
  for ue in /sys/class/input/event*/device/uevent; do
    [ -f "$ue" ] || continue
    if grep -q 'ID_INPUT_TOUCHSCREEN=1\|TOUCHSCREEN' "$ue" 2>/dev/null \
       || grep -qi 'Touchscreen\|Touch Screen' "$ue" 2>/dev/null; then
      udev_name=$(grep '^NAME=' "$ue" 2>/dev/null | head -1 | cut -d= -f2- | tr -d '"')
      [ -n "$udev_name" ] && break
    fi
  done
  if [ -z "$udev_name" ] && [ -r /proc/bus/input/devices ]; then
    udev_name=$(awk '
      /^N: Name=/ {
        name=$0; sub(/^N: Name="/,"",name); sub(/"$/,"",name)
      }
      /Touchscreen|Touch Screen|SYNAPTICS|Goodix|ELAN|SYNA|Atmel|fts/ {
        if (name != "") { print name; exit }
      }
    ' /proc/bus/input/devices 2>/dev/null || true)
  fi
  if [ -n "$udev_name" ]; then
    TOUCH_PRODUCT=$(printf '%s' "$udev_name" | awk '{print $1}' | cut -d: -f1)
    log "  Touchscreen product match: $TOUCH_PRODUCT ($udev_name)"
  else
    log "  Using default MatchProduct=$TOUCH_PRODUCT (catchall InputClass still applies)"
  fi
}

configure_xorg() {
  log "Writing Xorg configs → $XORG_D"
  run mkdir -p "$XORG_D"

  local conf
  conf=$(sed -e "s|@BUSID@|$BUSID|g" -e "s|@KMSDEV@|$KMSDEV|g" \
    "$ROOT/debian/xorg.conf.d/20-modesetting.conf.in")
  printf '%s\n' "$conf" | run tee "$XORG_D/20-modesetting.conf" >/dev/null

  # Touch: rewrite MatchProduct on the product-specific section
  sed "s/MatchProduct \"SYNA7813\"/MatchProduct \"$TOUCH_PRODUCT\"/" \
    "$ROOT/debian/xorg.conf.d/40-libinput-touchscreen.conf" \
    | run tee "$XORG_D/40-libinput-touchscreen.conf" >/dev/null

  # Allow startx from the autologin user without a display manager
  if [ -f /etc/X11/Xwrapper.config ]; then
    run sed -i \
      -e 's/^allowed_users=.*/allowed_users=anybody/' \
      -e 's/^needs_root_rights=.*/needs_root_rights=yes/' \
      /etc/X11/Xwrapper.config
    grep -q '^allowed_users=' /etc/X11/Xwrapper.config || \
      printf 'allowed_users=anybody\n' | run tee -a /etc/X11/Xwrapper.config >/dev/null
    grep -q '^needs_root_rights=' /etc/X11/Xwrapper.config || \
      printf 'needs_root_rights=yes\n' | run tee -a /etc/X11/Xwrapper.config >/dev/null
  else
    printf 'allowed_users=anybody\nneeds_root_rights=yes\n' \
      | run tee /etc/X11/Xwrapper.config >/dev/null
  fi
}

# ---------- build & install ----------
build_summerboard() {
  log "Compiling summerboardv28_gl.c against Debian Mesa"
  local src="$ROOT/summerboardv28_gl.c"
  [ -f "$src" ] || die "missing $src"
  [ -f "$ROOT/summerboard_gl.h" ] || die "missing summerboard_gl.h"
  [ -f "$ROOT/stb_image.h" ] || die "missing stb_image.h (needed to build)"
  [ -f "$ROOT/nanosvg.h" ] || die "missing nanosvg.h"
  [ -f "$ROOT/nanosvgrast.h" ] || die "missing nanosvgrast.h"

  local out="$ROOT/summerboard"
  cc "$src" -o "$out" \
    -O2 -I/usr/include/freetype2 \
    -lX11 -lXext -lXfixes -lEGL -lGL -lfreetype -lm \
    || die "compile failed"

  run install -m 755 "$out" "$BIN_DIR/summerboard"
  run install -m 755 "$ROOT/rotate" "$BIN_DIR/rotate"
  log "  installed $BIN_DIR/summerboard and $BIN_DIR/rotate"
}

install_assets() {
  log "Installing assets → $ASSETS_DIR (and $SHARE_DIR)"
  run mkdir -p "$ASSETS_DIR" "$SHARE_DIR"
  run chown "$USER_NAME:$USER_NAME" "$ASSETS_DIR"

  # Runtime tree expected by SUMMERBOARD_DIR
  as_user cp -a "$ROOT/assets/." "$ASSETS_DIR/"
  # Ensure WhiteSur lands even if assets/. copy quirks
  if [ -d "$ROOT/assets/WhiteSur" ] && [ ! -d "$ASSETS_DIR/WhiteSur" ]; then
    as_user cp -a "$ROOT/assets/WhiteSur" "$ASSETS_DIR/"
  fi

  # System-wide mirror (optional fallback)
  run cp -a "$ROOT/assets/SFPRODISPLAYREGULAR.OTF" "$SHARE_DIR/" 2>/dev/null || true
  run cp -a "$ROOT/assets/summerboardbg.png" "$SHARE_DIR/" 2>/dev/null || true
  if [ -d "$ROOT/assets/WhiteSur" ]; then
    run cp -a "$ROOT/assets/WhiteSur" "$SHARE_DIR/" 2>/dev/null || true
  fi
  run chown -R "$USER_NAME:$USER_NAME" "$ASSETS_DIR"
}

install_matchbox_layouts() {
  log "Wiring matchbox-keyboard layouts (number row)"
  local mb=/usr/share/matchbox-keyboard
  if [ -d "$mb" ]; then
    if [ -f "$mb/keyboard.xml" ] && [ ! -f "$mb/keyboard.xml.bak-pre-numbers" ]; then
      run cp -a "$mb/keyboard.xml" "$mb/keyboard.xml.bak-pre-numbers"
    fi
    run install -m 644 "$ROOT/debian/matchbox-keyboard/numbers-fragment.xml" "$mb/numbers-fragment.xml"
    run install -m 644 "$ROOT/debian/matchbox-keyboard/keyboard.xml" "$mb/keyboard.xml"
  else
    warn "matchbox-keyboard share dir missing — package may not have installed"
  fi

  # User override + SUMMERBOARD_DIR copy (MB_KBD_CONFIG prefers these)
  as_user mkdir -p "$USER_HOME/.matchbox"
  as_user cp -a "$ASSETS_DIR/keyboard.xml" "$USER_HOME/.matchbox/keyboard.xml"
}

install_asoundrc() {
  if [ ! -f "$USER_HOME/.asoundrc" ]; then
    log "Installing ~/.asoundrc (Settings sound boost)"
    as_user cp -a "$ROOT/debian/asoundrc" "$USER_HOME/.asoundrc"
  else
    if grep -q 'summerboard-softvol' "$USER_HOME/.asoundrc" 2>/dev/null; then
      log "~/.asoundrc already has summerboard softvol"
    else
      warn "~/.asoundrc exists — leaving it unchanged (see debian/asoundrc)"
    fi
  fi
}

configure_autostart() {
  log "Configuring getty autologin + startx + .xinitrc (no display manager)"

  run mkdir -p "$GETTY_D"
  printf '%s\n' \
    '[Service]' \
    'ExecStart=' \
    "ExecStart=-/sbin/agetty --autologin $USER_NAME --noclear %I \$TERM" \
    | run tee "$GETTY_D/autologin.conf" >/dev/null
  run systemctl daemon-reload

  # .xinitrc
  as_user cp -a "$ROOT/debian/xinitrc" "$USER_HOME/.xinitrc"
  as_user chmod 755 "$USER_HOME/.xinitrc"

  # Ensure login shells on tty1 exec startx (idempotent marker)
  local marker='# SUMMERBOARD_STARTX'
  local snippet
  snippet=$(cat <<EOF

$marker
if [ -z "\$DISPLAY" ] && [ "\${XDG_VTNR:-}" = "1" ]; then
  exec startx
fi
EOF
)
  for f in "$USER_HOME/.bash_profile" "$USER_HOME/.profile"; do
    if [ -f "$f" ] && grep -q "$marker" "$f" 2>/dev/null; then
      log "  startx hook already in $f"
    else
      # Create .bash_profile if missing so bash login shells see it
      if [ ! -f "$f" ]; then
        if [ "$(basename "$f")" = ".bash_profile" ]; then
          printf '%s\n' '# ~/.bash_profile' 'if [ -f "$HOME/.profile" ]; then . "$HOME/.profile"; fi' \
            | as_user tee "$f" >/dev/null
        else
          as_user touch "$f"
        fi
      fi
      printf '%s\n' "$snippet" | as_user tee -a "$f" >/dev/null
      log "  appended startx hook → $f"
    fi
  done
}

configure_groups() {
  log "Adding $USER_NAME to video, input, audio, render (if present)"
  for g in video input audio render; do
    if getent group "$g" >/dev/null 2>&1; then
      run usermod -aG "$g" "$USER_NAME" || warn "usermod -aG $g failed"
    fi
  done
}

print_summary() {
  cat <<EOF

----------------------------------------------------------------------
Summerboard Debian setup finished.

  User:     $USER_NAME
  Binary:   $BIN_DIR/summerboard
  Helper:   $BIN_DIR/rotate
  Assets:   $ASSETS_DIR   (SUMMERBOARD_DIR)
  Present:  SUMMERBOARD_PRESENT=egl
  GPU:      modesetting + glamor  BusID=$BUSID  kmsdev=$KMSDEV
  Autostart: getty@tty1 autologin → startx → .xinitrc → summerboard

Reboot to enter the Summerboard session on the panel:

  sudo reboot

To rebuild later from this tree:

  cc summerboardv28_gl.c -o summerboard \\
    -I/usr/include/freetype2 \\
    -lX11 -lXext -lXfixes -lEGL -lGL -lfreetype -lm
  sudo install -m 755 summerboard /usr/local/bin/summerboard
----------------------------------------------------------------------
EOF
}

main() {
  log "Summerboard Debian setup (user=$USER_NAME home=$USER_HOME)"
  need_sudo
  install_packages
  probe_gpu
  probe_touchscreen
  configure_xorg
  build_summerboard
  install_assets
  install_matchbox_layouts
  install_asoundrc
  configure_groups
  configure_autostart
  print_summary
}

main "$@"
