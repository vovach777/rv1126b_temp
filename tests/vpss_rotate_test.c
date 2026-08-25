/* vpss_rotate_test.c — тест VPSS SetGrpRotation с разными размерами
 * Может rotation работает только для маленьких кадров?
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rk_mpi_sys.h"
#include "rk_mpi_vpss.h"
#include "rk_comm_vpss.h"
#include "rk_common.h"

static int test_vpss_rotate(int max_w, int max_h, int out_w, int out_h, int vproc_dev, int rotation) {
    VPSS_GRP grp = 0;
    VPSS_CHN chn = 0;
    int ret;

    const char* rot_str = "NONE";
    if (rotation == 90) rot_str = "90";
    if (rotation == 180) rot_str = "180";
    if (rotation == 270) rot_str = "270";

    printf("\n--- VPSS rotate: max=%dx%d out=%dx%d dev=%d rot=%s ---\n",
           max_w, max_h, out_w, out_h, vproc_dev, rot_str);

    VPSS_GRP_ATTR_S grp_attr = {0};
    grp_attr.u32MaxW = max_w;
    grp_attr.u32MaxH = max_h;
    grp_attr.enPixelFormat = RK_FMT_YUV420SP;
    grp_attr.enCompressMode = COMPRESS_MODE_NONE;
    grp_attr.stFrameRate.s32SrcFrameRate = -1;
    grp_attr.stFrameRate.s32DstFrameRate = -1;

    ret = RK_MPI_VPSS_CreateGrp(grp, &grp_attr);
    if (ret) { printf("  CreateGrp FAIL: 0x%x\n", ret); return -1; }

    ret = RK_MPI_VPSS_SetVProcDev(grp, vproc_dev);
    if (ret) { printf("  SetVProcDev FAIL: 0x%x\n", ret); RK_MPI_VPSS_DestroyGrp(grp); return -1; }

    VPSS_CHN_ATTR_S chn_attr = {0};
    chn_attr.enChnMode = VPSS_CHN_MODE_USER;
    chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    chn_attr.stFrameRate.s32SrcFrameRate = -1;
    chn_attr.stFrameRate.s32DstFrameRate = -1;
    chn_attr.u32Width = out_w;
    chn_attr.u32Height = out_h;
    chn_attr.u32FrameBufCnt = 3;
    chn_attr.u32Depth = 0;

    ret = RK_MPI_VPSS_SetChnAttr(grp, chn, &chn_attr);
    if (ret) { printf("  SetChnAttr FAIL: 0x%x\n", ret); RK_MPI_VPSS_DestroyGrp(grp); return -1; }

    ret = RK_MPI_VPSS_EnableChn(grp, chn);
    if (ret) { printf("  EnableChn FAIL: 0x%x\n", ret); RK_MPI_VPSS_DestroyGrp(grp); return -1; }

    /* Set rotation */
    ROTATION_E rot_enum;
    if (rotation == 90) rot_enum = ROTATION_90;
    else if (rotation == 180) rot_enum = ROTATION_180;
    else if (rotation == 270) rot_enum = ROTATION_270;
    else rot_enum = ROTATION_0;

    if (rotation != 0) {
        ret = RK_MPI_VPSS_SetGrpRotation(grp, rot_enum);
        if (ret) {
            printf("  SetGrpRotation(%s) FAIL: 0x%x ❌\n", rot_str, ret);
            RK_MPI_VPSS_DisableChn(grp, chn);
            RK_MPI_VPSS_StopGrp(grp);
            RK_MPI_VPSS_DestroyGrp(grp);
            return -1;
        }
        printf("  SetGrpRotation(%s) returned OK (0x%x) — но крутит ли?\n", rot_str, ret);
    }

    ret = RK_MPI_VPSS_StartGrp(grp);
    if (ret) { printf("  StartGrp FAIL: 0x%x\n", ret); RK_MPI_VPSS_DisableChn(grp, chn); RK_MPI_VPSS_DestroyGrp(grp); return -1; }
    printf("  StartGrp OK\n");

    /* Проверим GetGrpRotation — реально ли установилось */
    ROTATION_E got_rot = ROTATION_0;
    ret = RK_MPI_VPSS_GetGrpRotation(grp, &got_rot);
    if (ret == 0) {
        const char* gr = got_rot==ROTATION_0?"0":got_rot==ROTATION_90?"90":got_rot==ROTATION_180?"180":got_rot==ROTATION_270?"270":"?";
        printf("  GetGrpRotation = %s (запросили %s)\n", gr, rot_str);
    } else {
        printf("  GetGrpRotation FAIL: 0x%x\n", ret);
    }

    printf("  ✅ Setup OK (rotation=%s)\n", rot_str);

    RK_MPI_VPSS_StopGrp(grp);
    RK_MPI_VPSS_DisableChn(grp, chn);
    RK_MPI_VPSS_DestroyGrp(grp);
    return 0;
}

int main(void) {
    printf("=== VPSS Rotation Size Test (RV1126B) ===\n");
    int ret = RK_MPI_SYS_Init();
    if (ret) { printf("SYS_Init failed: %d\n", ret); return 1; }

    /* Тестируем rotation с разными размерами */
    struct { int mw, mh, ow, oh, dev, rot; const char* desc; } tests[] = {
        /* Маленькие квадраты */
        {256,  256,  256,  256,  1, 90,  "256x256 rot90 RGA"},
        {256,  256,  256,  256,  3, 90,  "256x256 rot90 VPSS"},
        {512,  512,  512,  512,  1, 90,  "512x512 rot90 RGA"},
        {512,  512,  512,  512,  3, 90,  "512x512 rot90 VPSS"},
        {640,  640,  640,  640,  1, 90,  "640x640 rot90 RGA"},
        {640,  640,  640,  640,  3, 90,  "640x640 rot90 VPSS"},
        /* 720p */
        {1280, 720, 1280, 720,  1, 90,  "1280x720 rot90 RGA"},
        {1280, 720, 1280, 720,  3, 90,  "1280x720 rot90 VPSS"},
        {1280, 720, 720,  1280, 1, 90,  "1280x720→720x1280 rot90 RGA"},
        {1280, 720, 720,  1280, 3, 90,  "1280x720→720x1280 rot90 VPSS"},
        /* 1080p */
        {1920, 1080, 1920, 1080, 1, 90,  "1920x1080 rot90 RGA"},
        {1920, 1080, 1920, 1080, 3, 90,  "1920x1080 rot90 VPSS"},
        {1920, 1080, 1080, 1920, 1, 90,  "1920x1080→1080x1920 rot90 RGA"},
        {1920, 1080, 1080, 1920, 3, 90,  "1920x1080→1080x1920 rot90 VPSS"},
        /* 270° */
        {1920, 1080, 1080, 1920, 1, 270, "1920x1080→1080x1920 rot270 RGA"},
        {1920, 1080, 1080, 1920, 3, 270, "1920x1080→1080x1920 rot270 VPSS"},
        /* 180° */
        {1920, 1080, 1920, 1080, 1, 180, "1920x1080 rot180 RGA"},
        {1920, 1080, 1920, 1080, 3, 180, "1920x1080 rot180 VPSS"},
        /* SetChnRotation вместо SetGrpRotation */
        /* Очень маленькие */
        {64,   64,   64,   64,   1, 90,  "64x64 rot90 RGA"},
        {128,  128,  128,  128,  1, 90,  "128x128 rot90 RGA"},
    };

    int ok = 0, fail = 0;
    for (int i = 0; i < (int)(sizeof(tests)/sizeof(tests[0])); i++) {
        int r = test_vpss_rotate(tests[i].mw, tests[i].mh, tests[i].ow, tests[i].oh, tests[i].dev, tests[i].rot);
        if (r == 0) ok++; else fail++;
    }

    printf("\n=== ИТОГ: %d OK, %d FAIL ===\n", ok, fail);
    RK_MPI_SYS_Exit();
    return 0;
}
