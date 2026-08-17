cmake_minimum_required(VERSION 2.8.8)

project(vi_grab_frame C)

# === Платформа ===
# RV1126B: AArch64 (64-bit Linux), toolchain: aarch64-rockchip1240-linux-gnu
# В SDK есть обе версии librockit.so (arm/arm64), но плата работает в 64-bit.
set(TARGET_ARCH "arm64" CACHE STRING "Target architecture: arm64 (default) or arm (32-bit fallback)")
set(TARGET_CHIP "rv1126b" CACHE STRING "Target chip: rv1126b, rk3588, etc.")

if(TARGET_ARCH STREQUAL "arm64")
    set(LIB_ARCH_DIR "arm64")
else()
    set(LIB_ARCH_DIR "arm")
endif()

# === Пути к rockit ===
# Переопределить: cmake -DROCKIT_ROOT=/path/to/rockit ..
set(ROCKIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../external/rockit" CACHE PATH "Rockit root")

set(ROCKIT_INCLUDE_DIR "${ROCKIT_ROOT}/mpi/include")
set(ROCKIT_LIB_DIR "${ROCKIT_ROOT}/lib/${LIB_ARCH_DIR}/${TARGET_CHIP}/linux")

# === Пути к rkaiq (3A + camgroup) ===
# Заголовки в SDK; библиотека librkaiq.so — в sysroot при cross-compile
# или на устройстве /usr/lib/librkaiq.so
# Переопределить: cmake -DRKAIQ_ROOT=/path/to/rkaiq/include ..
set(RKAIQ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../external/camera_engine_rkaiq/rkaiq/include"
    CACHE PATH "rkaiq include root")
set(RKAIQ_INCLUDE_DIRS
    "${RKAIQ_ROOT}"
    "${RKAIQ_ROOT}/uAPI2"
    "${RKAIQ_ROOT}/common"
    "${RKAIQ_ROOT}/algos"
    "${RKAIQ_ROOT}/xcore"
    "${RKAIQ_ROOT}/iq_parser"
    "${RKAIQ_ROOT}/iq_parser_v2"
    "${RKAIQ_ROOT}/isp"
)
# Библиотека: искать в ROCKIT_LIB_DIR или в системном пути sysroot
set(RKAIQ_LIB_DIR "${ROCKIT_LIB_DIR}" CACHE PATH "rkaiq library dir (or sysroot /usr/lib)")

message(STATUS "Target: ${TARGET_ARCH} / ${TARGET_CHIP}")
message(STATUS "Rockit root: ${ROCKIT_ROOT}")
message(STATUS "Rockit include: ${ROCKIT_INCLUDE_DIR}")
message(STATUS "Rockit lib dir: ${ROCKIT_LIB_DIR}")
message(STATUS "rkaiq include: ${RKAIQ_ROOT}")

# === Cross-compile (если нужен) ===
# Раскомментировать для cross-compile:
# set(CMAKE_C_COMPILER aarch64-rockchip1240-linux-gnu-gcc)
# set(CMAKE_CXX_COMPILER aarch64-rockchip1240-linux-gnu-g++)
# set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)

# === Флаги ===
add_definitions(-DOS_LINUX -Wno-unused-variable -Wno-unused-but-set-variable)

# Sysroot lib path — librockit.so тянет libasound, libdrm, librockchip_mpp
# (на плате они есть, при кросс-сборке — в sysroot)
set(SYSROOT_LIB_DIR "K:/cross_sysroot_aarch64/usr/lib" CACHE PATH "Sysroot lib dir")

# === Исполняемые файлы ===
link_directories(${SYSROOT_LIB_DIR})

# vi_grab_frame — один сенсор
add_executable(vi_grab_frame vi_grab_frame.c)
target_include_directories(vi_grab_frame PRIVATE ${ROCKIT_INCLUDE_DIR})
target_link_directories(vi_grab_frame PRIVATE ${ROCKIT_LIB_DIR})
target_link_libraries(vi_grab_frame rockit m)
install(TARGETS vi_grab_frame RUNTIME DESTINATION "bin")

# vi_grab_dual — два сенсора одновременно (синхронизация через pthread_barrier)
add_executable(vi_grab_dual vi_grab_dual.c)
target_include_directories(vi_grab_dual PRIVATE ${ROCKIT_INCLUDE_DIR})
target_link_directories(vi_grab_dual PRIVATE ${ROCKIT_LIB_DIR})
target_link_libraries(vi_grab_dual rockit m pthread)
install(TARGETS vi_grab_dual RUNTIME DESTINATION "bin")

# vpss_fanout_test — VPSS fan-out: 2 channel, читаем только CHN0, CHN1 не трогаем
add_executable(vpss_fanout_test vpss_fanout_test.c)
target_include_directories(vpss_fanout_test PRIVATE
    ${ROCKIT_INCLUDE_DIR} ${RGA_INCLUDE_DIRS})
target_link_directories(vpss_fanout_test PRIVATE
    ${ROCKIT_LIB_DIR} ${LIB_RGA_PATH})
target_link_libraries(vpss_fanout_test rockit rga m)
install(TARGETS vpss_fanout_test RUNTIME DESTINATION "bin")

# vi_grab_avs — аппаратное сшивание двух камер через AVS (мега-кадр)
add_executable(vi_grab_avs vi_grab_avs.c)
target_include_directories(vi_grab_avs PRIVATE ${ROCKIT_INCLUDE_DIR})
target_link_directories(vi_grab_avs PRIVATE ${ROCKIT_LIB_DIR})
target_link_libraries(vi_grab_avs rockit m pthread)
install(TARGETS vi_grab_avs RUNTIME DESTINATION "bin")

# vi_grab_avs_dma — AVS → RGA crop+rotate → DMA буфер (zero-copy, VO/rknn ready)
add_executable(vi_grab_avs_dma vi_grab_avs_dma.c dma_alloc.c)
target_include_directories(vi_grab_avs_dma PRIVATE
    ${ROCKIT_INCLUDE_DIR} ${RGA_INCLUDE_DIRS})
target_link_directories(vi_grab_avs_dma PRIVATE ${ROCKIT_LIB_DIR} ${LIB_RGA_PATH})
target_link_libraries(vi_grab_avs_dma rockit rga m pthread)
install(TARGETS vi_grab_avs_dma RUNTIME DESTINATION "bin")

# stereo_demo — полная стерео-программа: camgroup (3A sync) + AVS + VPSS (crop fan-out)
# Все настройки в коде/CLI — НЕ нужно патчить rkipc или системные ini-файлы.
# Пайплайн: 2× GC2093 → camgroup → AVS (NOBLEND_HOR, bSyncPipe) → VPSS → 3 канала
#   CHN0 = cam0 (crop left), CHN1 = cam1 (crop right), CHN2 = full stitch
#   + EnableBackupFrame для snapshot через GetGrpFrame
add_executable(stereo_demo stereo_demo.c)
target_include_directories(stereo_demo PRIVATE
    ${ROCKIT_INCLUDE_DIR} ${RKAIQ_INCLUDE_DIRS})
target_link_directories(stereo_demo PRIVATE ${ROCKIT_LIB_DIR} ${RKAIQ_LIB_DIR})
target_link_libraries(stereo_demo rockit rkaiq m pthread)
install(TARGETS stereo_demo RUNTIME DESTINATION "bin")

# avs_mega_fanout_test — AVS sync → мега-кадр → RGA split для NN + display
# Пайплайн: VI×2 → AVS (bSyncPipe) → мега 1920×2160
#   → RGA crop RGB half → DMA (NN RGB thread, ~10fps)
#   → RGA crop IR half  → DMA (NN IR thread, ~10fps)
#   → RGA crop display_ch half + rot90 → VO (30fps, switch every 2s)
add_executable(avs_mega_fanout_test avs_mega_fanout_test.c dma_alloc.c)
target_include_directories(avs_mega_fanout_test PRIVATE
    ${ROCKIT_INCLUDE_DIR} ${RGA_INCLUDE_DIRS})
target_link_directories(avs_mega_fanout_test PRIVATE ${ROCKIT_LIB_DIR} ${LIB_RGA_PATH})
target_link_libraries(avs_mega_fanout_test rockit rga m pthread)
install(TARGETS avs_mega_fanout_test RUNTIME DESTINATION "bin")
