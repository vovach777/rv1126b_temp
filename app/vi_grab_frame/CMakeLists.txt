cmake_minimum_required(VERSION 2.8.8)

project(vi_grab_frame C)

# === Платформа ===
# По умолчанию: RV1126B arm64
# Переопределить: cmake -DTARGET_ARCH=arm ..
set(TARGET_ARCH "arm64" CACHE STRING "Target architecture: arm64 or arm")
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
# set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
# set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
# set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)

# === Флаги ===
add_definitions(-DOS_LINUX -Wno-unused-variable -Wno-unused-but-set-variable)

# === Исполняемый файл ===
add_executable(vi_grab_frame vi_grab_frame.c)

target_include_directories(vi_grab_frame PRIVATE
    ${ROCKIT_INCLUDE_DIR}
)

# === Линковка с librockit.so ===
# librockit.so зависит от: libasound.so.2, librockchip_mpp.so.1, libdrm.so.2
# На плате они уже есть. Для cross-compile указать путь через -DROCKIT_LIB_DIR
target_link_directories(vi_grab_frame PRIVATE
    ${ROCKIT_LIB_DIR}
)

target_link_libraries(vi_grab_frame
    rockit      # librockit.so
    m
)

# === Установка ===
install(TARGETS vi_grab_frame RUNTIME DESTINATION "bin")
