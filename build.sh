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

# librga.so — ищем в нескольких местах:
#   1. lib/ рядом со скриптом (положите туда с платы)
#   2. $SDK_PATH/external/camera_engine_rkaiq/rkisp_demo/demo/libs/arm64/ (есть в SDK, но только arm32!)
#   3. Собираем из исходников $SDK_PATH/external/linux-rga/ (если есть cmake + cross-compiler)
# Для arm64 (RV1126B) librga.so НЕТ в SDK — только arm32 в rkisp_demo.
# Берём с платы: scp root@<board>:/usr/lib/librga.so lib/
LIB_RGA_PATH="${LIB_RGA_PATH:-$(dirname "$0")/lib}"

# Автопоиск librga.so в SDK (на случай если появится arm64)
if [ ! -f "$LIB_RGA_PATH/librga.so" ]; then
    for cand in \
        "$SDK_PATH/external/camera_engine_rkaiq/rkisp_demo/demo/libs/arm64/librga.so" \
        "$SDK_PATH/external/linux-rga/build/librga.so"; do
        if [ -f "$cand" ]; then
            LIB_RGA_PATH="$(dirname "$cand")"
            echo "Found librga.so in SDK: $cand"
            break
        fi
    done
fi

if [ ! -f "$LIB_RGA_PATH/librga.so" ]; then
    echo "WARNING: librga.so not found."
    echo "  Get it from the board: scp root@<board-ip>:/usr/lib/librga.so $LIB_RGA_PATH/"
    echo "  Or build from source:  cd $SDK_PATH/external/linux-rga && mkdir build && cd build && cmake .. && make"
    echo "  (arm32 version is in SDK at external/camera_engine_rkaiq/rkisp_demo/demo/libs/arm32/ — NOT for arm64)"
    if [ "${BUILD_RGA_FROM_SOURCE:-0}" = "1" ]; then
        echo "BUILD_RGA_FROM_SOURCE=1 — building librga.so from $SDK_PATH/external/linux-rga/..."
        mkdir -p "$SDK_PATH/external/linux-rga/build"
        (cd "$SDK_PATH/external/linux-rga/build" && cmake -DCMAKE_C_COMPILER=zig -DCMAKE_CXX_COMPILER=zig++ .. && make -j4) || {
            echo "ERROR: failed to build librga from source"
            exit 1
        }
        LIB_RGA_PATH="$SDK_PATH/external/linux-rga/build"
    else
        echo "  Or set BUILD_RGA_FROM_SOURCE=1 to build from $SDK_PATH/external/linux-rga/"
        exit 1
    fi
fi
echo "LIB_RGA_PATH = $LIB_RGA_PATH"

mkdir -p build

CC="zig cc -target aarch64-linux-gnu"
CFLAGS="-O2 -I$ROCKIT_INC -I$ROCKIT_DEF -I$RGA_INC1 -I$RGA_INC2"
LDFLAGS="-L$ROCKIT_LIB -L$LIB_RGA_PATH -lrockit -lrga -lpthread -lm"

SRCDIR="$(dirname "$0")/app/vi_grab_frame"
PROGRAMS="vi_grab_frame vi_grab_avs vi_grab_avs_dma vi_grab_dual"

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
    # vi_grab_avs_dma требует dma_alloc.c (выделение DMA буферов через /dev/dma_heap/)
    extra_src=""
    if [ "$prog" = "vi_grab_avs_dma" ]; then
        extra_src="$SRCDIR/dma_alloc.c"
    fi
    $CC $CFLAGS -o "build/$prog" "$src" $extra_src $LDFLAGS
    echo "  -> build/$prog ($(stat -c%s build/$prog 2>/dev/null || wc -c < build/$prog) bytes)"
done

echo ""
echo "Done. Binaries in build/"
echo "Copy to board: scp build/* root@<board>:/tmp/"
echo ""
echo "NOTE: librga.so (arm64) is NOT in SDK — only arm32 in rkisp_demo."
echo "  Get from board: scp root@<board>:/usr/lib/librga.so $LIB_RGA_PATH/"
echo "  Or build from source: BUILD_RGA_FROM_SOURCE=1 $0"
