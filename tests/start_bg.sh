#!/bin/sh
# Start weston + rkaiq_3A_server (restore desktop)
# Usage: startbg

echo "=== Starting weston + rkaiq_3A_server ==="

# Start rkaiq_3A_server if not running (needed for ISP pipeline)
if ! pgrep -x rkaiq_3A_server >/dev/null; then
    /usr/bin/rkaiq_3A_server >/dev/null 2>&1 &
    echo "  rkaiq_3A_server started"
else
    echo "  rkaiq_3A_server: already running"
fi

sleep 1

# Start weston via init script (loads env from /etc/profile.d/weston.sh)
if ! pgrep -x weston >/dev/null; then
    sh /etc/init.d/S49weston start
else
    echo "  weston: already running"
fi

sleep 2
echo "=== Running services ==="
ps aux | grep -iE 'weston|rkaiq' | grep -v grep | head -5
echo "=== Done ==="
