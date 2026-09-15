#!/usr/bin/env bash
# Deploy the vroom OBD-II log page and its puller to the projects VM.
#
#   obd-dashboard/deploy/deploy.sh [ssh-target]        (default: node@192.168.15.3)
#
# Idempotent. /var/www/html/vroom/data is never deleted or overwritten: it holds the only
# copy of rows the board has since dropped. The target account needs passwordless sudo
# for the systemd and Apache steps.
set -euo pipefail

HOST="${1:-node@192.168.15.3}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

echo "== folders"
ssh "$HOST" 'sudo install -d -o node -g node -m 755 /var/www/html/vroom /var/www/html/vroom/data /opt/vroom-obd'

echo "== site -> /var/www/html/vroom/"
# index.html names each asset with a hash of its content, so a browser holding an old copy
# fetches the new one after a deploy. No --delete: data/ lives beside the site files.
SITE_STAGE="$(mktemp -d)"
trap 'rm -rf "$SITE_STAGE"' EXIT
cp -p "$HERE"/site/* "$SITE_STAGE/"
for f in style.css chart.js app.js; do
  h="$(md5sum "$HERE/site/$f" | cut -c1-10)"
  sed -i "s#\"$f\"#\"$f?v=$h\"#" "$SITE_STAGE/index.html"
done
rsync -rtp --chmod=D755,F644 "$SITE_STAGE/" "$HOST:/var/www/html/vroom/"

echo "== puller -> /opt/vroom-obd/"
rsync -tp --chmod=F755 "$HERE/puller/vroom_obd_pull.py" "$HOST:/opt/vroom-obd/"

echo "== systemd timer + Apache conf"
STAGE="$(ssh "$HOST" mktemp -d)"
scp -q "$HERE/deploy/vroom-obd-pull.service" "$HERE/deploy/vroom-obd-pull.timer" \
    "$HERE/deploy/apache-vroom.conf" "$HOST:$STAGE/"
ssh "$HOST" "set -e
  sudo install -m 644 '$STAGE/vroom-obd-pull.service' '$STAGE/vroom-obd-pull.timer' /etc/systemd/system/
  sudo install -m 644 '$STAGE/apache-vroom.conf' /etc/apache2/conf-available/vroom.conf
  rm -rf '$STAGE'
  sudo systemctl daemon-reload
  sudo systemctl enable --now vroom-obd-pull.timer
  sudo a2enconf -q vroom
  sudo apache2ctl configtest
  sudo systemctl reload apache2"

echo "== first check"
ssh "$HOST" 'sudo systemctl start vroom-obd-pull.service; journalctl -u vroom-obd-pull.service -n 5 --no-pager -o cat'
echo "== http://192.168.15.3/vroom/"
