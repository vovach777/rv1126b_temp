#!/bin/sh
# Disable weston-player autostart launchers (cause RGA conflicts with AVS stitcher)

echo "=== Disabling weston-player launchers ==="

# Backup originals (if not already backed up)
[ ! -f /etc/xdg/weston/weston.ini.d/01-launcher.ini.bak ] && cp /etc/xdg/weston/weston.ini.d/01-launcher.ini /etc/xdg/weston/weston.ini.d/01-launcher.ini.bak
[ ! -f /etc/xdg/weston/weston.ini.d/03-desktop-launcher.ini.bak ] && cp /etc/xdg/weston/weston.ini.d/03-desktop-launcher.ini /etc/xdg/weston/weston.ini.d/03-desktop-launcher.ini.bak

# Restore from backup first
cp /etc/xdg/weston/weston.ini.d/01-launcher.ini.bak /etc/xdg/weston/weston.ini.d/01-launcher.ini
cp /etc/xdg/weston/weston.ini.d/03-desktop-launcher.ini.bak /etc/xdg/weston/weston.ini.d/03-desktop-launcher.ini

# Use awk to comment out [launcher] blocks containing weston-player or test_gst
for f in /etc/xdg/weston/weston.ini.d/01-launcher.ini /etc/xdg/weston/weston.ini.d/03-desktop-launcher.ini; do
  awk '
  /^\[launcher\]/ { in_block=1; block=""; }
  in_block { block = block $0 "\n"; next }
  { print }
  END {
    # process remaining block at EOF
  }
  ' "$f" > "$f.tmp" 2>/dev/null
  rm -f "$f.tmp"
done

# Simpler: just rename the files so weston doesn't read them
# But keep weston-terminal launcher. Create clean versions.

cat > /etc/xdg/weston/weston.ini.d/01-launcher.ini << 'EOF'
[launcher]
icon=/usr/share/weston/icon_terminal.png
path=/usr/bin/weston-terminal

[launcher]
icon=/usr/share/weston/sign_close.png
path=/usr/bin/pkill -USR2 -x weston
displayname=Kill Focused Window

[launcher]
icon=/usr/share/weston/icon_flower.png
path=/usr/bin/pkill -USR2 weston
displayname=Restart Weston
EOF

cat > /etc/xdg/weston/weston.ini.d/03-desktop-launcher.ini << 'EOF'
# Launchers in default group
# weston-player launcher removed (causes RGA conflicts with AVS stitcher)

[desktop-launcher]
icon=/usr/share/weston/icon_editor.png
path=/usr/bin/weston-editor
displayname=Editor

[desktop-launcher]
icon=/usr/share/icons/icon_simple-egl.png
path=/usr/bin/weston-simple-egl
displayname=EGL Test

[desktop-launcher]
icon=/usr/share/icons/icon_glmark2.png
path=/usr/bin/glmark2-es2-wayland --visual-config='a=0:buf=24' --annotate
displayname=Glmark2
EOF

echo "=== 01-launcher.ini after ==="
cat /etc/xdg/weston/weston.ini.d/01-launcher.ini
echo "=== 03-desktop-launcher.ini after ==="
cat /etc/xdg/weston/weston.ini.d/03-desktop-launcher.ini
echo "=== Done. weston-player launchers removed ==="
