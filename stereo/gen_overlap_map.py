#!/usr/bin/env python3
"""
Generate srcOverlapMap.bin for RV1126B stereo camera (2x GC2093, side-by-side).

The file format is a raw binary dump of struct RK_PS_SrcOverlapMap:

    struct RK_PS_SrcOverlapMap {
        char versionInfo[64];                           // 64 bytes
        enum RK_PS_SrcOverlapPosition srcOverlapPositon[8]; // 8 * 4 = 32 bytes
        unsigned char overlapMap[15 * 15 * 8];           // 1800 bytes
    };
    // Total: 64 + 32 + 1800 = 1896 bytes

enum RK_PS_SrcOverlapPosition:
    0 = ROTATE_0
    1 = ROTATE_90
    2 = ROTATE_180
    3 = ROTATE_270

The overlapMap is a 15x15 grid per camera (8 cameras max).
Each cell indicates which camera "owns" that region (0-based camera index, or 0xFF = none).

For 2 cameras side-by-side (cam0 left, cam1 right):
  - Camera 0: left half + overlap zone on the right
  - Camera 1: right half + overlap zone on the left
  - Overlap zone: middle columns where both cameras see the same area

Usage:
    python3 gen_overlap_map.py [--output srcOverlapMap.bin]

The generated file should be placed on the device at:
    /oem/usr/share/avs_calib/srcOverlapMap.bin
"""

import struct
import sys
import os

# Constants from panorama_stitchingApp.h
RK_PS_SRC_OVERLAP_POSITION_0 = 0
RK_PS_SRC_OVERLAP_POSITION_90 = 1
RK_PS_SRC_OVERLAP_POSITION_180 = 2
RK_PS_SRC_OVERLAP_POSITION_270 = 3

MAX_CAMS = 8
GRID_W = 15
GRID_H = 15
VERSION_INFO_SIZE = 64
OVERLAP_MAP_SIZE = GRID_W * GRID_H * MAX_CAMS  # 1800

def gen_overlap_map_2cam_side_by_side():
    """
    Generate overlap map for 2 cameras placed side-by-side horizontally.

    Layout (15x15 grid, cam0=left, cam1=right):

        Columns 0-6:   cam0 only (left non-overlap)
        Columns 7-8:   overlap zone (both cam0 and cam1)
        Columns 9-14:  cam1 only (right non-overlap)

    The overlapMap is indexed as overlapMap[y * GRID_W + x] per camera,
    but the actual layout in memory is overlapMap[cam * GRID_W * GRID_H + y * GRID_W + x].

    Each cell value: camera index (0 or 1) that "owns" this region,
    or 0xFF if this camera doesn't see this region at all.
    """
    # Initialize: all cameras see nothing (0xFF)
    overlap_map = bytearray([0xFF] * OVERLAP_MAP_SIZE)

    # Camera 0 (left): sees columns 0-8 (including overlap)
    for cam in [0]:
        for y in range(GRID_H):
            for x in range(9):  # columns 0-8
                idx = cam * GRID_W * GRID_H + y * GRID_W + x
                overlap_map[idx] = 0  # owned by cam0

    # Camera 1 (right): sees columns 6-14 (including overlap)
    for cam in [1]:
        for y in range(GRID_H):
            for x in range(6, GRID_W):  # columns 6-14
                idx = cam * GRID_W * GRID_H + y * GRID_W + x
                overlap_map[idx] = 1  # owned by cam1

    return overlap_map

def gen_src_overlap_map_bin(output_path, num_cams=2, rotations=None):
    """
    Generate the full srcOverlapMap.bin file.

    Args:
        output_path: path to write the .bin file
        num_cams: number of cameras (2 for stereo)
        rotations: list of rotation values per camera (default: all 0)
    """
    if rotations is None:
        rotations = [RK_PS_SRC_OVERLAP_POSITION_0] * MAX_CAMS

    # versionInfo: 64 bytes, null-padded string
    version = b"RV1126B_STEREO_2xGC2093_v1.0"
    version_info = version + b'\x00' * (VERSION_INFO_SIZE - len(version))

    # srcOverlapPositon: 8 * int32 (enum = 4 bytes on ARM)
    positions = struct.pack('<8I', *rotations)

    # overlapMap: 15*15*8 bytes
    overlap_map = gen_overlap_map_2cam_side_by_side()

    # Concatenate
    data = version_info + positions + bytes(overlap_map)

    assert len(data) == VERSION_INFO_SIZE + 32 + OVERLAP_MAP_SIZE, \
        f"Size mismatch: {len(data)} != {VERSION_INFO_SIZE + 32 + OVERLAP_MAP_SIZE}"

    with open(output_path, 'wb') as f:
        f.write(data)

    print(f"Generated: {output_path}")
    print(f"  Size: {len(data)} bytes")
    print(f"  Version: {version.decode()}")
    print(f"  Cameras: {num_cams}")
    print(f"  Rotations: {rotations[:num_cams]}")
    print(f"  Layout: 2 cameras side-by-side, overlap at columns 6-8")
    print()
    print("Place on device at: /oem/usr/share/avs_calib/srcOverlapMap.bin")

def main():
    output = "srcOverlapMap.bin"
    if len(sys.argv) > 1:
        if sys.argv[1] in ('-h', '--help'):
            print(__doc__)
            sys.exit(0)
        if '--output' in sys.argv:
            idx = sys.argv.index('--output')
            output = sys.argv[idx + 1]

    # Default: 2 cameras, no rotation
    gen_src_overlap_map_bin(output, num_cams=2,
                           rotations=[RK_PS_SRC_OVERLAP_POSITION_0] * MAX_CAMS)

if __name__ == '__main__':
    main()
