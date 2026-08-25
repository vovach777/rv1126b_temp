#!/bin/sh
# Stop weston + conflicting apps before vi_grab_avs_dma VO demo
# VO and weston share VOP0-win2-0 plane — weston must be stopped to avoid conflicts
# Usage: stopbg

echo "=== Stopping weston + conflicting apps ==="

# Stop rkipc (if running)
killall rkipc 2>/dev/null && echo "  rkipc stopped" || echo "  rkipc: not running"

# Stop weston-player if launched manually
killall weston-player 2>/dev/null && echo "  weston-player stopped" || echo "  weston-player: not running"

# Stop our app if still running
killall vi_grab_avs_dma 2>/dev/null && echo "  vi_grab_avs_dma stopped" || echo "  vi_grab_avs_dma: not running"

# Stop weston (releases VOP0-win2-0 plane for VO)
if pgrep -x weston >/dev/null; then
    sh /etc/init.d/S49weston stop
else
    echo "  weston: not running"
fi

sleep 2
echo "=== Remaining processes ==="
ps aux | grep -iE 'weston|rkaiq|rkipc|vi_grab' | grep -v grep | head -10
echo "=== RGA error check ==="
dmesg | grep -iE 'rga.*invalid' | tail -3
echo "=== Done. Display free for VO ==="
echo "=== To restore desktop: startbg ==="
