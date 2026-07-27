#!/bin/bash
# build.sh — сборка vi_grab_frame / vi_grab_avs / vi_grab_dual / stereo_demo для RV1126B (aarch64)
#
# Использование:
#   SDK_PATH=/path/to/sdk ./build.sh               # собрать всё
#   SDK_PATH=/path/to/sdk ./build.sh stereo_demo   # только одну программу
#   ./build.sh                                      # SDK_PATH по умолчанию = ../sdk
#
# Требования:
#   - zig (для кросс-компиляции aarch64-linux-gnu)
#   - Rockchip RV1126B SDK в $SDK_PATH (с external/rockit, external/linux-rga, external/camera_engine_rkaiq)
#
# Что ищется в SDK:
#   $SDK_PATH/external/rockit/mpi/sdk/include       — заголовки rockit (rk_mpi_*.h)
#   $SDK_PATH/external/rockit/lib/arm64/rv1126b     — rk_defines.h
#   $SDK_PATH/external/rockit/lib/arm64/rv1126b/linux — librockit.so
#   $SDK_PATH/external/linux-rga/im2d_api           — заголовки RGA (im2d_*.h)
#   $SDK_PATH/external/linux-rga/include            — rga.h, drmrga.h
#   $SDK_PATH/external/camera_engine_rkaiq/rkaiq/include — заголовки rkaiq (3A + camgroup)
#
# librga.so берётся с платы (/usr/lib/librga.so) — её нет в SDK, только заголовки.
# librkaiq.so берётся с платы (/usr/lib/librkaiq.so) — она собирается из SDK, но не входит в репозиторий.
# Скопируйте librga.so и librkaiq.so с платы в $LIB_RGA_PATH или передайте -L путь через LIB_RGA_PATH.

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
RKAIQ_INC="$SDK_PATH/external/camera_engine_rkaiq/rkaiq/include"

for d in "$ROCKIT_INC" "$ROCKIT_DEF" "$ROCKIT_LIB" "$RGA_INC1" "$RGA_INC2"; do
    if [ ! -d "$d" ]; then
        echo "ERROR: missing SDK dir: $d"
        exit 1
    fi
done

# rkaiq headers (optional — only needed for stereo_demo)
if [ ! -d "$RKAIQ_INC" ]; then
    echo "WARNING: rkaiq headers not found at $RKAIQ_INC"
    echo "  stereo_demo requires rkaiq (3A + camgroup) headers."
    echo "  Other programs (vi_grab_frame, vi_grab_avs, etc.) will still build."
    RKAIQ_INC=""
fi

# librga.so (arm64) — ищем в нескольких местах:
#   1. lib/ рядом со скриптом (можно положить с платы или из SDK)
#   2. $SDK_PATH/external/rknpu2/examples/3rdparty/rga/libs/Linux/gcc-aarch64/ ← ЕСТЬ в SDK!
#   3. $SDK_PATH/external/camera_engine_rkaiq/rkisp_demo/demo/libs/arm64/ (только arm32 в SDK)
#   4. Собираем из исходников $SDK_PATH/external/linux-rga/ (если есть cmake + cross-compiler)
# Для arm64 (RV1126B) librga.so ЕСТЬ в SDK — в rknpu2 (статическая librga.a тоже есть).
LIB_RGA_PATH="${LIB_RGA_PATH:-$(dirname "$0")/lib}"

# Автопоиск librga.so в SDK (arm64)
if [ ! -f "$LIB_RGA_PATH/librga.so" ]; then
    for cand in \
        "$SDK_PATH/external/rknpu2/examples/3rdparty/rga/libs/Linux/gcc-aarch64/librga.so" \
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

# rkaiq flags (for stereo_demo only)
if [ -n "$RKAIQ_INC" ]; then
    RKAIQ_CFLAGS="-I$RKAIQ_INC -I$RKAIQ_INC/uAPI2 -I$RKAIQ_INC/common -I$RKAIQ_INC/algos -I$RKAIQ_INC/xcore -I$RKAIQ_INC/iq_parser -I$RKAIQ_INC/iq_parser_v2 -I$RKAIQ_INC/isp"
    RKAIQ_LDFLAGS="-lrkaiq"
else
    RKAIQ_CFLAGS=""
    RKAIQ_LDFLAGS=""
fi

SRCDIR="$(dirname "$0")/app/vi_grab_frame"
PROGRAMS="vi_grab_frame vi_grab_avs vi_grab_avs_dma vi_grab_dual stereo_demo"

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

    # stereo_demo требует rkaiq (camgroup + 3A)
    if [ "$prog" = "stereo_demo" ] && [ -z "$RKAIQ_INC" ]; then
        echo "SKIP: $prog requires rkaiq headers (not found in SDK)"
        continue
    fi

    echo "=== Building $prog ==="
    # vi_grab_avs_dma требует dma_alloc.c (выделение DMA буферов через /dev/dma_heap/)
    extra_src=""
    extra_cflags=""
    extra_ldflags=""
    if [ "$prog" = "vi_grab_avs_dma" ]; then
        extra_src="$SRCDIR/dma_alloc.c"
    fi
    if [ "$prog" = "stereo_demo" ]; then
        extra_cflags="$RKAIQ_CFLAGS"
        extra_ldflags="$RKAIQ_LDFLAGS"
    fi
    $CC $CFLAGS $extra_cflags -o "build/$prog" "$src" $extra_src $LDFLAGS $extra_ldflags
    echo "  -> build/$prog ($(stat -c%s build/$prog 2>/dev/null || wc -c < build/$prog) bytes)"
done

echo ""
echo "Done. Binaries in build/"
echo "Copy to board: scp build/* root@<board>:/tmp/"
echo ""
echo "NOTE: librga.so (arm64) is in SDK at external/rknpu2/examples/3rdparty/rga/libs/Linux/gcc-aarch64/"
echo "  Also available: librga.a (static), and from board: scp root@<board>:/usr/lib/librga.so $LIB_RGA_PATH/"
echo ""
echo "NOTE: stereo_demo requires librkaiq.so (3A + camgroup)."
echo "  Get from board: scp root@<board>:/usr/lib/librkaiq.so $LIB_RGA_PATH/"
echo "  Or build from SDK: cd $SDK_PATH/external/camera_engine_rkaiq && mkdir build && cd build && cmake .. && make"
