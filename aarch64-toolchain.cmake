# aarch64-toolchain.cmake — кросс-компиляция для RV1126B (aarch64-linux-gnu)
#
# Использование:
#   cmake -DSDK_PATH=/path/to/sdk -DCMAKE_TOOLCHAIN_FILE=../aarch64-toolchain.cmake ..
#
# Тулчейн: Arm GNU Toolchain 15.2.1 (2025) для Windows
#   D:\dev\aarch64-none-linux\bin\aarch64-none-linux-gnu-gcc.exe
#   D:\dev\aarch64-none-linux\bin\aarch64-none-linux-gnu-g++.exe
#   sysroot: D:\dev\aarch64-none-linux\aarch64-none-linux-gnu\libc
#
# Альтернатива (если нет Arm GNU): zig cc
#   set CMAKE_C_COMPILER=zig
#   (zig сам определяет target по -target)

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Путь к тулчейну Arm GNU (Windows)
set(AARCH64_TOOLCHAIN_PATH "D:/dev/aarch64-none-linux/bin"
    CACHE PATH "Path to aarch64-none-linux-gnu toolchain bin")

# Кросс-компиляторы
set(CMAKE_C_COMPILER   "${AARCH64_TOOLCHAIN_PATH}/aarch64-none-linux-gnu-gcc.exe")
set(CMAKE_CXX_COMPILER "${AARCH64_TOOLCHAIN_PATH}/aarch64-none-linux-gnu-g++.exe")

# Sysroot тулчейна (libc, заголовки POSIX)
set(CMAKE_SYSROOT "D:/dev/aarch64-none-linux/aarch64-none-linux-gnu/libc"
    CACHE PATH "Sysroot for aarch64 target")

# Дополнительно: где искать библиотеки/заголовки платы
# (SDK .so и заголовки rockit/rga/rkaiq — добавляются в CMakeLists.txt через SDK_PATH)
set(CMAKE_FIND_ROOT_PATH
    "${CMAKE_SYSROOT}"
    "${CMAKE_SYSROOT}/usr"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Флаги компилятора
set(CMAKE_C_FLAGS_INIT   "-fPIC")
set(CMAKE_CXX_FLAGS_INIT "-fPIC")
