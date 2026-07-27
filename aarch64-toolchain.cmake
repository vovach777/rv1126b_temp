# aarch64-toolchain.cmake — кросс-компиляция для RV1126B (aarch64-linux-gnu)
#
# Использование:
#   cmake -DSDK_PATH=/path/to/sdk -DCMAKE_TOOLCHAIN_FILE=../aarch64-toolchain.cmake ..
#
# Вариант 1: через zig cc (если установлен zig)
#   set CMAKE_C_COMPILER=zig
#   (zig сам определяет target по -target)
#
# Вариант 2: через aarch64-linux-gnu-gcc (классический кросс-компилятор)
#   Установите: sudo apt install gcc-aarch64-linux-gnu

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Попробуем zig cc, потом aarch64-linux-gnu-gcc
find_program(ZIG zig)
if(ZIG)
    set(CMAKE_C_COMPILER   ${ZIG})
    set(CMAKE_C_COMPILER_ARG1 "-target aarch64-linux-gnu")
    message(STATUS "Using zig cc for aarch64 cross-compile")
else()
    find_program(AARCH64_GCC aarch64-linux-gnu-gcc)
    if(AARCH64_GCC)
        set(CMAKE_C_COMPILER ${AARCH64_GCC})
        message(STATUS "Using ${AARCH64_GCC} for aarch64 cross-compile")
    else()
        message(FATAL_ERROR
            "No aarch64 cross-compiler found. Install one of:\n"
            "  - zig (recommended, auto-detects target)\n"
            "  - gcc-aarch64-linux-gnu (apt install)")
    endif()
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
