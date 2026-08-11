/* vpss_rotate_matrix.c — матрица тестов VPSS rotation
 * Проверяем все комбинации: размер × rotation × online/offline
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rk_mpi_sys.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_comm_vpss.h"
#include "rk_comm_mb.h"
#include "rk_common.h"

static int test_vpss_rot(int src_w, int src_h, int dst_w, int dst_h, int rotation) {
    VPSS_GRP grp = 0;
    VPSS_CHN chn = 0;
    int ret;

    const char* rot_str = rotation==0?"0":rotation==90?"90":rotation==180?"180":rotation==270?"270":"?";
    printf("\n--- %dx%d → rot%s → %dx%d ---\n", src_w, src_h, rot_str, dst_w, dst_h);

    /* Выделяем входной буфер */
    size_t src_size = src_w * src_h * 3 / 2;
    MB_BLK src_mb = NULL;
    ret = RK_MPI_SYS_MmzAlloc_Cached(&src_mb, NULL, NULL, src_size);
    if (ret || !src_mb) { printf("  MmzAlloc fail\n"); return -1; }

    uint8_t* src_ptr = RK_MPI_MB_Handle2VirAddr(src_mb);
    if (!src_ptr) { printf("  Handle2VirAddr fail\n"); RK_MPI_MB_ReleaseMB(src_mb); return -1; }

    /* Паттерн: left=0, right=255 */
    for (int y = 0; y < src_h; y++)
        for (int x = 0; x < src_w; x++)
            src_ptr[y * src_w + x] = (x < src_w/2) ? 0 : 255;
    memset(src_ptr + src_w * src_h, 128, src_w * src_h / 2);
    RK_MPI_SYS_MmzFlushCache(src_mb, RK_FALSE);

    /* VPSS */
    VPSS_GRP_ATTR_S grp_attr = {0};
    grp_attr.u32MaxW = src_w;
    grp_attr.u32MaxH = src_h;
    grp_attr.enPixelFormat = RK_FMT_YUV420SP;
    grp_attr.enCompressMode = COMPRESS_MODE_NONE;
    grp_attr.stFrameRate.s32SrcFrameRate = -1;
    grp_attr.stFrameRate.s32DstFrameRate = -1;

    ret = RK_MPI_VPSS_CreateGrp(grp, &grp_attr);
    if (ret) { printf("  CreateGrp fail: 0x%x\n", ret); RK_MPI_MB_ReleaseMB(src_mb); return -1; }

    ret = RK_MPI_VPSS_SetVProcDev(grp, VIDEO_PROC_DEV_RGA);
    if (ret) { printf("  SetVProcDev fail: 0x%x\n", ret); goto cleanup; }

    VPSS_CHN_ATTR_S chn_attr = {0};
    chn_attr.enChnMode = VPSS_CHN_MODE_USER;
    chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    chn_attr.stFrameRate.s32SrcFrameRate = -1;
    chn_attr.stFrameRate.s32DstFrameRate = -1;
    chn_attr.u32Width = dst_w;
    chn_attr.u32Height = dst_h;
    chn_attr.u32FrameBufCnt = 3;
    chn_attr.u32Depth = 2;

    ret = RK_MPI_VPSS_SetChnAttr(grp, chn, &chn_attr);
    if (ret) { printf("  SetChnAttr fail: 0x%x\n", ret); goto cleanup; }

    ret = RK_MPI_VPSS_EnableChn(grp, chn);
    if (ret) { printf("  EnableChn fail: 0x%x\n", ret); goto cleanup; }

    ROTATION_E rot_e = rotation==90?ROTATION_90:rotation==180?ROTATION_180:rotation==270?ROTATION_270:ROTATION_0;
    ret = RK_MPI_VPSS_SetGrpRotation(grp, rot_e);
    if (ret) { printf("  SetGrpRotation fail: 0x%x\n", ret); goto cleanup; }

    ret = RK_MPI_VPSS_StartGrp(grp);
    if (ret) { printf("  StartGrp fail: 0x%x\n", ret); goto cleanup; }

    /* Push frame */
    VIDEO_FRAME_INFO_S vf = {0};
    vf.stVFrame.pMbBlk        = src_mb;
    vf.stVFrame.u32Width      = src_w;
    vf.stVFrame.u32Height     = src_h;
    vf.stVFrame.u32VirWidth   = src_w;
    vf.stVFrame.u32VirHeight  = src_h;
    vf.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    vf.stVFrame.u64PTS        = 0;

    ret = RK_MPI_VPSS_SendFrame(grp, 0, &vf, 2000);
    if (ret) { printf("  SendFrame fail: 0x%x\n", ret); goto cleanup; }

    /* Get frame */
    VIDEO_FRAME_INFO_S dst_vf = {0};
    ret = RK_MPI_VPSS_GetChnFrame(grp, chn, &dst_vf, 3000);
    if (ret) {
        printf("  GetChnFrame TIMEOUT: 0x%x ❌\n", ret);
        goto cleanup;
    }

    if (dst_vf.stVFrame.pMbBlk) {
        uint8_t* dst_ptr = RK_MPI_MB_Handle2VirAddr(dst_vf.stVFrame.pMbBlk);
        int dw = dst_vf.stVFrame.u32Width;
        int dh = dst_vf.stVFrame.u32Height;
        int dvw = dst_vf.stVFrame.u32VirWidth;

        if (dst_ptr) {
            long top_sum=0, bot_sum=0, left_sum=0, right_sum=0;
            int hh = dh/2, hw = dw/2;
            for (int y = 0; y < hh; y++)
                for (int x = 0; x < dw; x++) top_sum += dst_ptr[y*dvw+x];
            for (int y = hh; y < dh; y++)
                for (int x = 0; x < dw; x++) bot_sum += dst_ptr[y*dvw+x];
            for (int y = 0; y < dh; y++) {
                for (int x = 0; x < hw; x++) left_sum += dst_ptr[y*dvw+x];
                for (int x = hw; x < dw; x++) right_sum += dst_ptr[y*dvw+x];
            }
            long ta=top_sum/(hh*dw), ba=bot_sum/((dh-hh)*dw);
            long la=left_sum/(hw*dh), ra=right_sum/((dw-hw)*dh);

            const char* result = "???";
            if (rotation == 0 && la<30 && ra>200) result = "✅ NO ROT (pass-through)";
            else if (rotation == 90 && ta<30 && ba>200) result = "✅ ROT90 WORKS!";
            else if (rotation == 270 && ta>200 && ba<30) result = "✅ ROT270 WORKS!";
            else if (rotation == 180 && la>200 && ra<30) result = "✅ ROT180 WORKS!";
            else if (la<30 && ra>200) result = "❌ NO ROTATION (ignored)";
            else result = "??? unclear";

            printf("  DST %dx%d: top=%ld bot=%ld left=%ld right=%ld → %s\n",
                   dw, dh, ta, ba, la, ra, result);
        }
        RK_MPI_VPSS_ReleaseChnFrame(grp, chn, &dst_vf);
    }

cleanup:
    RK_MPI_VPSS_StopGrp(grp);
    RK_MPI_VPSS_DisableChn(grp, chn);
    RK_MPI_VPSS_DestroyGrp(grp);
    RK_MPI_MB_ReleaseMB(src_mb);
    return ret;
}

int main(void) {
    printf("=== VPSS Rotation Matrix Test ===\n");
    int ret = RK_MPI_SYS_Init();
    if (ret) { printf("SYS_Init fail\n"); return 1; }

    struct { int sw,sh,dw,dh,rot; const char* desc; } tests[] = {
        /* Без ротации — baseline */
        {1920,1080, 1920,1080, 0,   "1920x1080 passthrough"},
        {512, 512,  512, 512,  0,   "512x512 passthrough"},

        /* Rotation 90° с тем же размером выхода */
        {512, 512,  512, 512,  90,  "512x512 rot90 → 512x512"},
        {640, 480,  640, 480,  90,  "640x480 rot90 → 640x480"},
        {1280,720,  1280,720,  90,  "1280x720 rot90 → 1280x720"},
        {1920,1080, 1920,1080, 90,  "1920x1080 rot90 → 1920x1080"},

        /* Rotation 90° со swap размером выхода */
        {512, 512,  512, 512,  90,  "512x512 rot90 → 512x512 (square swap=same)"},
        {640, 480,  480, 640,  90,  "640x480 rot90 → 480x640 (swapped)"},
        {1280,720,  720, 1280, 90,  "1280x720 rot90 → 720x1280 (swapped)"},
        {1920,1080, 1080,1920, 90,  "1920x1080 rot90 → 1080x1920 (swapped)"},

        /* Rotation 90° со scale */
        {1920,1080, 800, 1280, 90,  "1920x1080 rot90 → 800x1280 (scale+rot)"},
        {1920,1080, 720, 1280, 90,  "1920x1080 rot90 → 720x1280 (NN size)"},
        {1280,720,  800, 1280, 90,  "1280x720 rot90 → 800x1280"},

        /* Rotation 270° */
        {1920,1080, 1920,1080, 270, "1920x1080 rot270 → 1920x1080"},
        {1920,1080, 800, 1280, 270, "1920x1080 rot270 → 800x1280"},

        /* Rotation 180° */
        {1920,1080, 1920,1080, 180, "1920x1080 rot180 → 1920x1080"},
    };

    int ok=0, fail=0;
    for (int i = 0; i < (int)(sizeof(tests)/sizeof(tests[0])); i++) {
        int r = test_vpss_rot(tests[i].sw, tests[i].sh, tests[i].dw, tests[i].dh, tests[i].rot);
        if (r == 0) ok++; else fail++;
    }

    printf("\n=== ИТОГ: %d OK, %d FAIL ===\n", ok, fail);
    RK_MPI_SYS_Exit();
    return 0;
}
