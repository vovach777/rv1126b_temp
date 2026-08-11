#!/bin/sh
export QT_QPA_PLATFORM="linuxfb:rotation=180:size=800x1280:mmsize=94x151"
export QT_QPA_FB_DRM=1
export QT_QPA_GENERIC_PLUGINS="evdevtouch:/dev/input/event1"
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS="rotate=180"
export LD_LIBRARY_PATH=/tmp
export QT_LOGGING_RULES="qt.qpa.input=true"
/tmp/s30gui_test
