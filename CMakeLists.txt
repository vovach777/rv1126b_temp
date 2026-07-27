# CMakeLists.txt — сборка vi_grab_frame / vi_grab_avs / vi_grab_dual для RV1126B
#
# Использование:
#   mkdir build && cd build
#   cmake -DSDK_PATH=/path/to/sdk ..
#   make
#
# Или с кросс-компиляцией (aarch64):
#   mkdir build && cd build
#   cmake -DSDK_PATH=/path/to/sdk -DCMAKE_TOOLCHAIN_FILE=../aarch64-toolchain.cmake ..
#   make
#
# SDK_PATH (REQUIRED) — корень RV1126B SDK с external/rockit и external/linux-rga.
#
# Что ищется:
#   $SDK_PATH/external/rockit/mpi/sdk/include     — rk_mpi_*.h
#   $SDK_PATH/external/rockit/lib/arm64/rv1126b   — rk_defines.h
#   $SDK_PATH/external/rockit/lib/arm64/rv1126b/linux — librockit.so
#   $SDK_PATH/external/linux-rga/im2d_api         — im2d_*.h
#   $SDK_PATH/external/linux-rga/include          — rga.h
#
# librga.so берётся с платы (см. Findlibrga.cmake в camera_engine_rkaiq).
# Положите librga.so в lib/ рядом с CMakeLists.txt или укажите -DLIB_RGA_PATH=/path.

cmake_minimum_required(VERSION 3.10)
project(rv1126b_vi_grab C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- SDK_PATH (обязательный) ---
if(NOT DEFINED SDK_PATH)
    # Попробовать соседнюю ../sdk
    get_filename_component(_DEFAULT_SDK "${CMAKE_SOURCE_DIR}/../sdk" ABSOLUTE)
    if(EXISTS "${_DEFAULT_SDK}/external/rockit")
        set(SDK_PATH "${_DEFAULT_SDK}")
    else()
        message(FATAL_ERROR
            "SDK_PATH not set. Point to RV1126B SDK root:\n"
            "  cmake -DSDK_PATH=/path/to/sdk ..\n"
            "Expected: \$SDK_PATH/external/rockit/mpi/sdk/include/rk_mpi_sys.h")
    endif()
endif()
message(STATUS "SDK_PATH = ${SDK_PATH}")

# --- Пути к заголовкам ---
set(ROCKIT_INC   "${SDK_PATH}/external/rockit/mpi/sdk/include")
set(ROCKIT_DEF   "${SDK_PATH}/external/rockit/lib/arm64/rv1126b")
set(ROCKIT_LIB   "${SDK_PATH}/external/rockit/lib/arm64/rv1126b/linux")
set(RGA_INC_IM2D "${SDK_PATH}/external/linux-rga/im2d_api")
set(RGA_INC_RGA  "${SDK_PATH}/external/linux-rga/include")

foreach(d ROCKIT_INC ROCKIT_DEF ROCKIT_LIB RGA_INC_IM2D RGA_INC_RGA)
    if(NOT IS_DIRECTORY "${${d}}")
        message(FATAL_ERROR "Missing SDK dir: ${${d}} (var=${d})")
    endif()
endforeach()

# --- librga.so (arm64) — ищем в нескольких местах ---
# 1. lib/ рядом с CMakeLists.txt
# 2. $SDK_PATH/external/rknpu2/examples/3rdparty/rga/libs/Linux/gcc-aarch64/ ← ЕСТЬ в SDK!
# 3. $SDK_PATH/external/camera_engine_rkaiq/rkisp_demo/demo/libs/arm64/ (только arm32 в SDK)
# 4. Собираем из $SDK_PATH/external/linux-rga/ (если BUILD_RGA_FROM_SOURCE=ON)
if(NOT DEFINED LIB_RGA_PATH)
    get_filename_component(LIB_RGA_PATH "${CMAKE_SOURCE_DIR}/lib" ABSOLUTE)
    if(NOT EXISTS "${LIB_RGA_PATH}/librga.so")
        # Попробуем в SDK — rknpu2 (там есть arm64!)
        set(_RGA_CAND "${SDK_PATH}/external/rknpu2/examples/3rdparty/rga/libs/Linux/gcc-aarch64")
        if(EXISTS "${_RGA_CAND}/librga.so")
            set(LIB_RGA_PATH "${_RGA_CAND}")
            message(STATUS "Found librga.so in SDK (rknpu2): ${LIB_RGA_PATH}")
        else()
            # fallback: rkisp_demo (только arm32 в SDK)
            set(_RGA_CAND "${SDK_PATH}/external/camera_engine_rkaiq/rkisp_demo/demo/libs/arm64")
            if(EXISTS "${_RGA_CAND}/librga.so")
                set(LIB_RGA_PATH "${_RGA_CAND}")
                message(STATUS "Found librga.so in SDK (rkisp_demo): ${LIB_RGA_PATH}")
            endif()
        endif()
    endif()
endif()
if(NOT EXISTS "${LIB_RGA_PATH}/librga.so")
    if(BUILD_RGA_FROM_SOURCE)
        message(STATUS "Building librga.so from ${SDK_PATH}/external/linux-rga/...")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SDK_PATH}/external/linux-rga/build
        )
        execute_process(
            COMMAND ${CMAKE_COMMAND} ${SDK_PATH}/external/linux-rga
                    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            WORKING_DIRECTORY ${SDK_PATH}/external/linux-rga/build
        )
        execute_process(COMMAND ${CMAKE_COMMAND} --build . -- -j4
            WORKING_DIRECTORY ${SDK_PATH}/external/linux-rga/build)
        set(LIB_RGA_PATH "${SDK_PATH}/external/linux-rga/build")
    else()
        message(WARNING
            "librga.so (arm64) not found in ${LIB_RGA_PATH}.\n"
            "Get from board: scp root@<board>:/usr/lib/librga.so ${LIB_RGA_PATH}/\n"
            "Or: cmake -DLIB_RGA_PATH=/other/path ..\n"
            "Or: cmake -DBUILD_RGA_FROM_SOURCE=ON .. (builds from external/linux-rga/)")
    endif()
endif()

# --- Includes ---
include_directories(
    ${ROCKIT_INC}
    ${ROCKIT_DEF}
    ${RGA_INC_IM2D}
    ${RGA_INC_RGA}
)

# --- Libraries ---
link_directories(${ROCKIT_LIB} ${LIB_RGA_PATH})

# --- Программы ---
set(PROGRAMS vi_grab_frame vi_grab_avs vi_grab_dual)
foreach(prog ${PROGRAMS})
    set(src "${CMAKE_SOURCE_DIR}/app/vi_grab_frame/${prog}.c")
    if(EXISTS "${src}")
        add_executable(${prog} ${src})
        target_link_libraries(${prog} rockit rga pthread m)
        message(STATUS "Configured ${prog}")
    else()
        message(STATUS "Skip ${prog} (no source)")
    endif()
endforeach()

# --- Установка (опционально) ---
install(TARGETS ${PROGRAMS} DESTINATION bin OPTIONAL)
