/* vpss_size_test.c — тест VPSS с разными размерами кадра
 * Проверяем: какой max размер VPSS реально переваривает на RV1126B
 * И какой VProcDev работает (RGA vs VPSS)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rk_mpi_sys.h"
#include "rk_mpi_vpss.h"
#include "rk_comm_vpss.h"
#include "rk_common.h"

static const char* vproc_str(int dev) {
    switch (dev) {
        case 0: return "GPU";
        case 1: return "RGA";
        case 2: return "ISP";
        case 3: return "VPSS";
        default: return "UNKNOWN";
    }
}

static int test_vpss(int max_w, int max_h, int out_w, int out_h, int vproc_dev) {
    VPSS_GRP grp = 0;
    VPSS_CHN chn = 0;
    int ret;

    printf("\n--- VPSS test: max=%dx%d out=%dx%d VProcDev=%s(%d) ---\n",
           max_w, max_h, out_w, out_h, vproc_str(vproc_dev), vproc_dev);

    /* Create Group */
    VPSS_GRP_ATTR_S grp_attr = {0};
    grp_attr.u32MaxW = max_w;
    grp_attr.u32MaxH = max_h;
    grp_attr.enPixelFormat = RK_FMT_YUV420SP;
    grp_attr.enCompressMode = COMPRESS_MODE_NONE;
    grp_attr.stFrameRate.s32SrcFrameRate = -1;
    grp_attr.stFrameRate.s32DstFrameRate = -1;

    ret = RK_MPI_VPSS_CreateGrp(grp, &grp_attr);
    if (ret) {
        printf("  CreateGrp FAILED: 0x%x\n", ret);
        return -1;
    }
    printf("  CreateGrp OK (max %dx%d)\n", max_w, max_h);

    /* Set VProcDev */
    ret = RK_MPI_VPSS_SetVProcDev(grp, vproc_dev);
    if (ret) {
        printf("  SetVProcDev(%s) FAILED: 0x%x\n", vproc_str(vproc_dev), ret);
        RK_MPI_VPSS_DestroyGrp(grp);
        return -1;
    }
    printf("  SetVProcDev(%s) OK\n", vproc_str(vproc_dev));

    /* Set Channel */
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
    if (ret) {
        printf("  SetChnAttr(%dx%d) FAILED: 0x%x\n", out_w, out_h, ret);
        RK_MPI_VPSS_DestroyGrp(grp);
        return -1;
    }
    printf("  SetChnAttr(%dx%d) OK\n", out_w, out_h);

    ret = RK_MPI_VPSS_EnableChn(grp, chn);
    if (ret) {
        printf("  EnableChn FAILED: 0x%x\n", ret);
        RK_MPI_VPSS_DestroyGrp(grp);
        return -1;
    }
    printf("  EnableChn OK\n");

    ret = RK_MPI_VPSS_StartGrp(grp);
    if (ret) {
        printf("  StartGrp FAILED: 0x%x\n", ret);
        RK_MPI_VPSS_DisableChn(grp, chn);
        RK_MPI_VPSS_DestroyGrp(grp);
        return -1;
    }
    printf("  StartGrp OK ✅\n");

    /* Cleanup */
    RK_MPI_VPSS_StopGrp(grp);
    RK_MPI_VPSS_DisableChn(grp, chn);
    RK_MPI_VPSS_DestroyGrp(grp);
    return 0;
}

int main(void) {
    printf("=== VPSS Size Test (RV1126B) ===\n");

    int ret = RK_MPI_SYS_Init();
    if (ret) {
        printf("SYS_Init failed: %d\n", ret);
        return 1;
    }

    /* Тесты с разными размерами и VProcDev */
    struct { int mw, mh, ow, oh, dev; const char* desc; } tests[] = {
        /* Маленькие размеры */
        {640,  480,  640,  480,  1, "640x480 RGA"},
        {640,  480,  640,  480,  3, "640x480 VPSS"},
        /* 720p */
        {1280, 720, 1280, 720,  1, "1280x720 RGA"},
        {1280, 720, 1280, 720,  3, "1280x720 VPSS"},
        {1280, 720, 800,  1280, 1, "1280x720→800x1280 RGA"},
        {1280, 720, 800,  1280, 3, "1280x720→800x1280 VPSS"},
        /* 1080p */
        {1920, 1080, 1920, 1080, 1, "1920x1080 RGA"},
        {1920, 1080, 1920, 1080, 3, "1920x1080 VPSS"},
        {1920, 1080, 800,  1280, 1, "1920x1080→800x1280 RGA"},
        {1920, 1080, 800,  1280, 3, "1920x1080→800x1280 VPSS"},
        {1920, 1080, 720,  1280, 1, "1920x1080→720x1280 RGA"},
        {1920, 1080, 1080, 1920, 1, "1920x1080→1080x1920 RGA (rotate size)"},
        /* Max камера */
        {2688, 1520, 1920, 1080, 1, "2688x1520→1920x1080 RGA"},
        {2688, 1520, 1920, 1080, 3, "2688x1520→1920x1080 VPSS"},
        /* Большое */
        {4096, 2160, 1920, 1080, 1, "4096x2160→1920x1080 RGA"},
    };

    int ok = 0, fail = 0;
    for (int i = 0; i < (int)(sizeof(tests)/sizeof(tests[0])); i++) {
        int r = test_vpss(tests[i].mw, tests[i].mh, tests[i].ow, tests[i].oh, tests[i].dev);
        if (r == 0) ok++; else fail++;
    }

    printf("\n=== ИТОГ: %d OK, %d FAIL ===\n", ok, fail);
    RK_MPI_SYS_Exit();
    return 0;
}
