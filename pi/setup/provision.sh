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
apt-get install -y \
    python3 python3-pip python3-flask python3-serial \
    git chromium-browser unclutter xserver-xorg xinit \
    matchbox-window-manager

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

# ---------- 9. Chromium kiosk autostart ----------

log "Configuring chromium kiosk autostart..."
AUTOSTART_DIR="/home/$PI_USER/.config/lxsession/LXDE-pi"
mkdir -p "$AUTOSTART_DIR"
cat > "$AUTOSTART_DIR/autostart" <<EOF
@xset s noblank
@xset s off
@xset -dpms
@unclutter -idle 0.5 -root
@chromium-browser --kiosk --noerrdialogs --disable-infobars \
    --check-for-update-interval=31536000 \
    http://localhost:8000
EOF
chown -R "$PI_USER:$PI_USER" "/home/$PI_USER/.config"

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
