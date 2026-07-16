cmake_minimum_required( VERSION 2.8.8 )
project (rockit)

if (NOT DEFINED ARCH64)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(ARCH arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(ARCH arm)
    endif()
endif()

set (CHIP_TYPE rv1103b)

if("${RK_MEDIA_CHIP}" STREQUAL "rv1106")
set (CHIP_TYPE rv1106)
endif()

if("${RK_MEDIA_CHIP}" STREQUAL "rv1126b")
set (CHIP_TYPE rv1126b)
endif()

if("${RK_MEDIA_CHIP}" STREQUAL "rk3576")
set (CHIP_TYPE rk3588)
endif()

if(RK3308)
set (CHIP_TYPE rk3308)
endif()

if(RK3506)
set (CHIP_TYPE rk3506)
endif()

if(RV1126B)
set (CHIP_TYPE rv1126b)
endif()

message(STATUS "RK_MEDIA_CHIP:${RK_MEDIA_CHIP}, CHIP_TYPE:${CHIP_TYPE}")

option(USE_ROCKIT_TGI  "enable rockit tgi" OFF)
if (${USE_ROCKIT_TGI})
    message("Build rockit tgi")
    add_subdirectory(tgi)
endif()

option(USE_ROCKIT_MPI  "enable rockit mpi" ON)
if (${USE_ROCKIT_MPI})
    message("Build rockit mpi")
    add_subdirectory(mpi)
endif()
