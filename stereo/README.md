# Stereo Camera Configuration for RV1126B (2× GC2093)

This directory contains files for enabling stereo camera features on RV1126B
with dual GC2093 sensors.

## Files

| File | Purpose |
|------|---------|
| `camgroup_gc2093_dual.json` | Group IQ file — enables synchronized AWB between cameras |
| `srcOverlapMap.bin` | Overlap map — tells ISP which regions overlap between cameras |
| `gen_overlap_map.py` | Python script to regenerate `srcOverlapMap.bin` |
| `rkipc-stereo-overlay.ini` | INI overlay with stereo-specific settings |
| `patch_rkipc_camgroup.patch` | Patch for rkipc to pass group_iq_file + overlap_map_file |

## What These Files Do

### camgroup_gc2093_dual.json

Group IQ file for `rk_aiq_camgroup`. Contains a single field:

```json
{
    "group_awb": 1
}
```

When `group_awb = 1`, the AWB algorithm runs in **group mode**: it computes
a single white balance gain for all cameras in the group, ensuring both
cameras produce the same colors. This is critical for stereo matching.

Parsed by `CamCalibDbCreateCalibDbCamgroup()` in
`external/camera_engine_rkaiq/rkaiq/iq_parser_v2/RkAiqCalibDbV2.c:391`.

Structure (`RkAiqCalibDbTypesV2.h:228`):
```c
typedef struct CamCalibDbCamgroup_s {
    int group_awb;
} CamCalibDbCamgroup_t;
```

### srcOverlapMap.bin

Binary file (1896 bytes) — raw dump of `struct RK_PS_SrcOverlapMap`:

```c
struct RK_PS_SrcOverlapMap {
    char versionInfo[64];                               // 64 bytes
    enum RK_PS_SrcOverlapPosition srcOverlapPositon[8]; // 32 bytes
    unsigned char overlapMap[15 * 15 * 8];              // 1800 bytes
};
// Total: 1896 bytes
```

Layout for 2 cameras side-by-side:
- Camera 0 (left):  grid columns 0-8
- Camera 1 (right): grid columns 6-14
- Overlap zone:     columns 6-8 (both cameras see this region)

Read by `rk_aiq_uapi2_camgroup_getOverlapMap_from_file()` in
`external/camera_engine_rkaiq/rkaiq/uAPI2_c/rk_aiq_user_api2_camgroup.c:280`.

The overlap map is used to:
1. Set `module_rotation` for each camera (`srcOverlapPositon[i]`)
2. Inform AWB about which regions overlap (for gain computation)

### patch_rkipc_camgroup.patch

rkipc currently does NOT pass `group_iq_file` or `overlap_map_file` to
`rk_aiq_uapi2_camgroup_create()`. This patch adds ini parameters:

```ini
[avs]
group_iq_file = camgroup_gc2093_dual.json
overlap_map_file = srcOverlapMap.bin
```

And reads them in `isp_camera_group_init()` (`isp.c:187`).

## How to Test

### Step 1: Copy files to device

```bash
# IQ files (from SDK)
scp external/camera_engine_rkaiq/rkaiq/iqfiles/isp20/gc2093_BFC105-DUAL-L_IR.xml \
    root@10.0.55.160:/oem/usr/share/iqfiles/

# Stereo files (from this directory)
scp stereo/camgroup_gc2093_dual.json \
    root@10.0.55.160:/oem/usr/share/iqfiles/
scp stereo/srcOverlapMap.bin \
    root@10.0.55.160:/oem/usr/share/avs_calib/
```

### Step 2: Patch rkipc

```bash
cd sdk
patch -p1 < stereo/patch_rkipc_camgroup.patch
# Rebuild rkipc
```

### Step 3: Update ini file

Merge `rkipc-stereo-overlay.ini` into your `rkipc-dual-800w.ini`:

```bash
# On device, backup original
cp /etc/rkipc.ini /etc/rkipc.ini.bak

# Edit: set group_mode=1, group_ldch=1, sync=1, projection_mode=1
# Add: group_iq_file, overlap_map_file
vi /etc/rkipc.ini
```

Key changes:
```ini
[isp]
group_mode = 1          ; was 0
group_ldch = 1          ; was 0

[avs]
enable_avs = 1          ; was 0
sync = 1                ; was 0
projection_mode = 1     ; new (RECTILINEAR)
group_iq_file = camgroup_gc2093_dual.json       ; new
overlap_map_file = srcOverlapMap.bin            ; new
```

### Step 4: Restart rkipc and verify

```bash
# Kill old rkipc
killall rkipc

# Start with new config
rkipc -c /etc/rkipc.ini &

# Check logs for group mode
dmesg | grep -i "camgroup\|group_awb\|overlap"
# Expected:
#   camgroup_cfg.sns_num is 2
#   camgroup group_iq_file = camgroup_gc2093_dual.json
#   rk_aiq_uapi2_camgroup_create over
#   rk_aiq_uapi2_camgroup_prepare over
#   rk_aiq_uapi2_camgroup_start over
```

### Step 5: Verify stereo sync

```bash
# Check that both cameras have same AE (exposure)
# Look at rkaiq logs:
cat /proc/rkaiq/log | grep -i "ae\|exposure"

# Both cameras should report the same exposure time and gain.
# If they differ, group mode is not working.

# Check AWB:
cat /proc/rkaiq/log | grep -i "awb\|wb_gain"
# Both cameras should report the same WB gains.
```

## What to Expect

| Feature | Without group_mode | With group_mode=1 |
|---------|-------------------|-------------------|
| AE (exposure) | Independent per camera | Synchronized |
| AWB (white balance) | Independent per camera | Synchronized |
| LDCH (distortion) | Per-camera level | Group-calibrated |
| Frame sync | No | Yes (bSyncPipe=1) |
| Colors | May differ between cameras | Matched |
| Brightness | May differ between cameras | Matched |

## Regenerating srcOverlapMap.bin

If your camera layout differs (e.g., cameras rotated, different overlap zone):

```bash
python3 stereo/gen_overlap_map.py --output srcOverlapMap.bin
```

Edit `gen_overlap_map.py` to change:
- `rotations` — camera rotation (0, 90, 180, 270 degrees)
- Overlap zone columns in `gen_overlap_map_2cam_side_by_side()`

## Limitations

1. **No disparity/depth map** — AVS stitches but does not compute depth.
   For depth, use NPU with a stereo matching RKNN model.

2. **No epipolar rectification** — LDCH corrects lens distortion, but does
   NOT align epipolar lines. For precise stereo matching, you need external
   rectification (OpenCV `stereoRectify`).

3. **rkipc patch required** — without the patch, `group_iq_file` and
   `overlap_map_file` are ignored.

4. **Calibration file needed** — `calib_file.xml` must be generated by
   external calibration tool (Hugin, OpenCV). The SDK includes examples
   in `external/avs/avs_calib/` but they are for 3/6 cameras, not 2.
