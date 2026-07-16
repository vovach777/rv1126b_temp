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

message(STATUS "Target: ${TARGET_ARCH} / ${TARGET_CHIP}")
message(STATUS "Rockit root: ${ROCKIT_ROOT}")
message(STATUS "Include: ${ROCKIT_INCLUDE_DIR}")
message(STATUS "Lib dir: ${ROCKIT_LIB_DIR}")

# === Cross-compile (если нужен) ===
# Раскомментировать для cross-compile:
# set(CMAKE_C_COMPILER aarch64-rockchip1240-linux-gnu-gcc)
# set(CMAKE_CXX_COMPILER aarch64-rockchip1240-linux-gnu-g++)
# set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)

# === Флаги ===
add_definitions(-DOS_LINUX -Wno-unused-variable -Wno-unused-but-set-variable)

# === Исполняемые файлы ===

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
