#!/bin/bash
# build.sh — сборка vi_grab_frame / vi_grab_avs / vi_grab_dual для RV1126B (aarch64)
#
# Использование:
#   SDK_PATH=/path/to/sdk ./build.sh           # собрать всё
#   SDK_PATH=/path/to/sdk ./build.sh vi_grab_avs  # только одну программу
#   ./build.sh                                  # SDK_PATH по умолчанию = ../sdk
#
# Требования:
#   - zig (для кросс-компиляции aarch64-linux-gnu)
#   - Rockchip RV1126B SDK в $SDK_PATH (с external/rockit и external/linux-rga)
#
# Что ищется в SDK:
#   $SDK_PATH/external/rockit/mpi/sdk/include     — заголовки rockit (rk_mpi_*.h)
#   $SDK_PATH/external/rockit/lib/arm64/rv1126b   — rk_defines.h
#   $SDK_PATH/external/rockit/lib/arm64/rv1126b/linux — librockit.so
#   $SDK_PATH/external/linux-rga/im2d_api         — заголовки RGA (im2d_*.h)
#   $SDK_PATH/external/linux-rga/include          — rga.h, drmrga.h
#
# librga.so берётся с платы (/usr/lib/librga.so) — её нет в SDK, только заголовки.
# Скопируйте librga.so с платы в $LIB_RGA_PATH или передайте -L путь через LIB_RGA_PATH.

set -e

# SDK_PATH по умолчанию — соседняя директория ../sdk (как в K:\sdk рядом с K:\rv1126b_temp)
SDK_PATH="${SDK_PATH:-$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)/sdk}"
# Если ../sdk нет, попробуем ../rv1126b_sdk
if [ ! -d "$SDK_PATH/external/rockit" ]; then
    ALT="$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)/rv1126b_sdk"
    if [ -d "$ALT/external/rockit" ]; then
        SDK_PATH="$ALT"
    fi
fi

if [ ! -d "$SDK_PATH/external/rockit" ]; then
    echo "ERROR: SDK not found. Set SDK_PATH to your RV1126B SDK root."
    echo "  Expected: \$SDK_PATH/external/rockit/mpi/sdk/include/rk_mpi_sys.h"
    echo "  Try: SDK_PATH=/path/to/sdk $0"
    exit 1
fi

echo "SDK_PATH = $SDK_PATH"

ROCKIT_INC="$SDK_PATH/external/rockit/mpi/sdk/include"
ROCKIT_DEF="$SDK_PATH/external/rockit/lib/arm64/rv1126b"
ROCKIT_LIB="$SDK_PATH/external/rockit/lib/arm64/rv1126b/linux"
RGA_INC1="$SDK_PATH/external/linux-rga/im2d_api"
RGA_INC2="$SDK_PATH/external/linux-rga/include"

for d in "$ROCKIT_INC" "$ROCKIT_DEF" "$ROCKIT_LIB" "$RGA_INC1" "$RGA_INC2"; do
    if [ ! -d "$d" ]; then
        echo "ERROR: missing SDK dir: $d"
        exit 1
    fi
done

# librga.so — с платы. По умолчанию ищем в lib/ рядом со скриптом (положите туда).
LIB_RGA_PATH="${LIB_RGA_PATH:-$(dirname "$0")/lib}"
mkdir -p build

CC="zig cc -target aarch64-linux-gnu"
CFLAGS="-O2 -I$ROCKIT_INC -I$ROCKIT_DEF -I$RGA_INC1 -I$RGA_INC2"
LDFLAGS="-L$ROCKIT_LIB -L$LIB_RGA_PATH -lrockit -lrga -lpthread -lm"

SRCDIR="$(dirname "$0")/app/vi_grab_frame"
PROGRAMS="vi_grab_frame vi_grab_avs vi_grab_dual"

# Если аргумент передан — собираем только указанную программу
if [ $# -gt 0 ]; then
    PROGRAMS="$@"
fi

for prog in $PROGRAMS; do
    src="$SRCDIR/$prog.c"
    if [ ! -f "$src" ]; then
        echo "SKIP: $src not found"
        continue
    fi
    echo "=== Building $prog ==="
    $CC $CFLAGS -o "build/$prog" "$src" $LDFLAGS
    echo "  -> build/$prog ($(stat -c%s build/$prog 2>/dev/null || wc -c < build/$prog) bytes)"
done

echo ""
echo "Done. Binaries in build/"
echo "Copy to board: scp build/* root@<board>:/tmp/"
echo ""
echo "NOTE: librga.so is NOT in SDK. Get it from the board:"
echo "  scp root@<board>:/usr/lib/librga.so $LIB_RGA_PATH/"
