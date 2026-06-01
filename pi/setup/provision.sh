#!/usr/bin/env bash
# Pi Zero 2 W first-boot provisioning for the vroom remote-start project.
#
# Run as: sudo bash provision.sh
#
# Idempotent — re-running is safe. Each step skips itself if already done.

set -euo pipefail

PROJECT_DIR=/opt/vroom
REPO_URL="https://github.com/SixOfFive/2009_kia_spectra.git"
PI_USER="${PI_USER:-pi}"

if [[ $EUID -ne 0 ]]; then
    echo "This script must be run as root (sudo bash provision.sh)" >&2
    exit 1
fi

log() { echo -e "\n==> $*"; }

# ---------- 1. System packages ----------

log "Installing system packages..."
apt-get update
# Kiosk display stack: X11 + openbox + surf. Chromium is installed as
# a *fallback*, but the default kiosk launcher is surf — Chromium reliably
# OOM-killed itself on the Pi Zero 2 W (512 MB total) while loading the
# vroom dashboard. surf's WebKit2GTK runtime fits in ~60 MB RSS and
# handles the dashboard + Leaflet map fine. See docs/10-pi-setup.md
# "Why surf, not Chromium" for the full reasoning.
apt-get install -y \
    python3 python3-pip python3-flask python3-serial \
    git unclutter \
    xserver-xorg-core xserver-xorg-legacy xinit openbox x11-xserver-utils xdotool \
    surf \
    chromium-browser

# ---------- 2. Python packages ----------

log "Installing Python packages..."
# paho-mqtt is not in apt for all releases; pip --break-system-packages is the
# pragmatic choice on a single-purpose appliance.
pip3 install --break-system-packages paho-mqtt

# ---------- 3. Serial port for ESP32 UART ----------

log "Configuring serial port..."
# Disable serial login, enable serial hardware
raspi-config nonint do_serial_hw 0 || true
raspi-config nonint do_serial_cons 1 || true

# ---------- 4. Runtime directories ----------

log "Creating runtime directories..."
mkdir -p /var/lib/vroom /var/log/vroom
chown "$PI_USER:$PI_USER" /var/lib/vroom /var/log/vroom

# ---------- 5. Clone or update project ----------

if [[ ! -d "$PROJECT_DIR" ]]; then
    log "Cloning vroom project to $PROJECT_DIR..."
    git clone "$REPO_URL" "$PROJECT_DIR"
    chown -R "$PI_USER:$PI_USER" "$PROJECT_DIR"
else
    log "Project already at $PROJECT_DIR — pulling latest..."
    sudo -u "$PI_USER" git -C "$PROJECT_DIR" pull --ff-only || true
fi

# ---------- 6. Secrets check ----------

if [[ ! -f "$PROJECT_DIR/pi/app/secrets.py" ]]; then
    log "No secrets.py — copy and edit before starting the service:"
    echo "    sudo -u $PI_USER cp $PROJECT_DIR/pi/app/secrets.py.example $PROJECT_DIR/pi/app/secrets.py"
    echo "    sudoedit $PROJECT_DIR/pi/app/secrets.py"
fi

# ---------- 7. sudoers entry so daemon can shutdown without password ----------

log "Granting passwordless shutdown to $PI_USER..."
cat > /etc/sudoers.d/vroom <<EOF
$PI_USER ALL=(ALL) NOPASSWD: /sbin/shutdown
EOF
chmod 0440 /etc/sudoers.d/vroom

# ---------- 8. systemd service ----------

log "Installing systemd service..."
cp "$PROJECT_DIR/pi/systemd/vroom.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable vroom.service
# The SNMP responder runs as a thread inside vroom.service (so it
# shares the in-process state dict). No separate unit needed. To
# disable SNMP entirely, set SNMP_ENABLED = False in pi/app/config.py
# and restart vroom.service.

# ---------- 9. surf kiosk autostart (TTY1 login -> startx -> openbox -> surf) ----------

log "Configuring surf kiosk autostart..."

# Allow non-console users (we autostart X from TTY1's login shell) to start X.
cat > /etc/X11/Xwrapper.config <<'EOF'
allowed_users=anybody
needs_root_rights=yes
EOF

# .xinitrc — what `startx` runs. Disables screen blanking, hides the
# cursor when idle, launches openbox, then surf at the dashboard URL.
# surf flags: -F fullscreen, -K disable keyboard shortcuts (we don't
# want the kiosk user accidentally opening a URL bar), -T disable
# strict transport security (so we can hit a self-signed HTTPS dev URL
# during testing). For production over plain HTTP locally, -T is a
# no-op.
cat > "/home/$PI_USER/.xinitrc" <<'EOF'
#!/bin/sh
xset s noblank
xset s off
xset -dpms
unclutter -idle 0.5 -root &
openbox &
exec surf -FKT http://localhost:8000/
EOF
chmod +x "/home/$PI_USER/.xinitrc"
chown "$PI_USER:$PI_USER" "/home/$PI_USER/.xinitrc"

# .bash_profile — startx automatically when this user logs in on TTY1.
# SSH sessions don't have a controlling tty under /dev/tty*, so this
# block is a no-op there.
if ! grep -q 'auto-startx-tty1' "/home/$PI_USER/.bash_profile" 2>/dev/null; then
    cat >> "/home/$PI_USER/.bash_profile" <<'EOF'

# auto-startx-tty1: launch X on the TV when this user logs in on TTY1.
if [ -z "$DISPLAY" ] && [ "$(tty)" = "/dev/tty1" ]; then
    exec startx
fi
EOF
fi
chown "$PI_USER:$PI_USER" "/home/$PI_USER/.bash_profile"

# Enable TTY1 autologin (so the kiosk comes up without manual login).
# This is the systemd-getty drop-in pattern.
mkdir -p /etc/systemd/system/getty@tty1.service.d
cat > /etc/systemd/system/getty@tty1.service.d/autologin.conf <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $PI_USER --noclear %I \$TERM
EOF
systemctl daemon-reload

# ---------- Done ----------

cat <<EOF

------------------------------------------------------------
Provisioning complete.

Next steps:
  1. Edit secrets if you haven't:
       sudoedit $PROJECT_DIR/pi/app/secrets.py
  2. Start the daemon:
       sudo systemctl start vroom.service
  3. Tail logs:
       journalctl -u vroom.service -f
  4. Reboot for chromium kiosk to take effect:
       sudo reboot

Dashboard is at http://<pi-ip>:8000 from any device on the LAN.
------------------------------------------------------------
EOF
