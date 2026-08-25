/* vpss_rotate_real.c — пропускаем кадр через VPSS с rotation
 * Offline режим: сами пушим кадр в VPSS, сами забираем с выхода.
 * Паттерн: левая половина чёрная, правая белая.
 * После rot90: верх чёрный, низ белый.
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

#define SRC_W 1920
#define SRC_H 1080
#define DST_W 1920
#define DST_H 1080

int main(void) {
    printf("=== VPSS Real Rotation Test (offline) ===\n");
    printf("SRC %dx%d → rot90 → DST %dx%d\n\n", SRC_W, SRC_H, DST_W, DST_H);

    int ret = RK_MPI_SYS_Init();
    if (ret) { printf("SYS_Init fail: %d\n", ret); return 1; }

    /* 1. Выделяем MMZ буфер для входного кадра */
    size_t src_size = SRC_W * SRC_H * 3 / 2;
    MB_BLK src_mb = NULL;
    ret = RK_MPI_SYS_MmzAlloc_Cached(&src_mb, NULL, NULL, src_size);
    if (ret || !src_mb) { printf("MmzAlloc fail: 0x%x\n", ret); return 1; }
    printf("MMZ buffer allocated (size=%zu)\n", src_size);

    /* 2. Заполняем паттерном */
    uint8_t* src_ptr = RK_MPI_MB_Handle2VirAddr(src_mb);
    if (!src_ptr) { printf("Handle2VirAddr fail\n"); goto cleanup; }

    /* Паттерн: левая половина Y=0, правая Y=255 */
    for (int y = 0; y < SRC_H; y++)
        for (int x = 0; x < SRC_W; x++)
            src_ptr[y * SRC_W + x] = (x < SRC_W/2) ? 0 : 255;
    memset(src_ptr + SRC_W * SRC_H, 128, SRC_W * SRC_H / 2);
    RK_MPI_SYS_MmzFlushCache(src_mb, RK_FALSE);  /* flush to physical */
    printf("SRC pattern: left=0, right=255 (1920x1080)\n");

    /* 3. VPSS Group */
    VPSS_GRP grp = 0;
    VPSS_CHN chn = 0;

    VPSS_GRP_ATTR_S grp_attr = {0};
    grp_attr.u32MaxW = SRC_W;
    grp_attr.u32MaxH = SRC_H;
    grp_attr.enPixelFormat = RK_FMT_YUV420SP;
    grp_attr.enCompressMode = COMPRESS_MODE_NONE;
    grp_attr.stFrameRate.s32SrcFrameRate = -1;
    grp_attr.stFrameRate.s32DstFrameRate = -1;

    ret = RK_MPI_VPSS_CreateGrp(grp, &grp_attr);
    if (ret) { printf("CreateGrp fail: 0x%x\n", ret); goto cleanup; }

    ret = RK_MPI_VPSS_SetVProcDev(grp, VIDEO_PROC_DEV_RGA);
    if (ret) { printf("SetVProcDev fail: 0x%x\n", ret); goto vpss_cleanup; }

    VPSS_CHN_ATTR_S chn_attr = {0};
    chn_attr.enChnMode = VPSS_CHN_MODE_USER;
    chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    chn_attr.stFrameRate.s32SrcFrameRate = -1;
    chn_attr.stFrameRate.s32DstFrameRate = -1;
    chn_attr.u32Width = DST_W;
    chn_attr.u32Height = DST_H;
    chn_attr.u32FrameBufCnt = 3;
    chn_attr.u32Depth = 2;

    ret = RK_MPI_VPSS_SetChnAttr(grp, chn, &chn_attr);
    if (ret) { printf("SetChnAttr fail: 0x%x\n", ret); goto vpss_cleanup; }

    ret = RK_MPI_VPSS_EnableChn(grp, chn);
    if (ret) { printf("EnableChn fail: 0x%x\n", ret); goto vpss_cleanup; }

    /* Set rotation 90° — выход того же размера что вход */
    ret = RK_MPI_VPSS_SetGrpRotation(grp, ROTATION_90);
    printf("SetGrpRotation(90) = 0x%x\n", ret);

    ret = RK_MPI_VPSS_StartGrp(grp);
    if (ret) { printf("StartGrp fail: 0x%x\n", ret); goto vpss_cleanup; }
    printf("VPSS started\n");

    /* 4. Отправляем кадр */
    VIDEO_FRAME_INFO_S vf = {0};
    vf.stVFrame.pMbBlk        = src_mb;
    vf.stVFrame.u32Width      = SRC_W;
    vf.stVFrame.u32Height     = SRC_H;
    vf.stVFrame.u32VirWidth   = SRC_W;
    vf.stVFrame.u32VirHeight  = SRC_H;
    vf.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    vf.stVFrame.u64PTS        = 0;

    ret = RK_MPI_VPSS_SendFrame(grp, 0, &vf, 2000);
    printf("VPSS_SendFrame = 0x%x\n", ret);
    if (ret) {
        printf("SendFrame failed!\n");
        goto vpss_cleanup;
    }

    /* 5. Получаем результат */
    VIDEO_FRAME_INFO_S dst_vf = {0};
    ret = RK_MPI_VPSS_GetChnFrame(grp, chn, &dst_vf, 3000);
    printf("VPSS_GetChnFrame = 0x%x\n", ret);

    if (ret == 0 && dst_vf.stVFrame.pMbBlk) {
        uint8_t* dst_ptr = RK_MPI_MB_Handle2VirAddr(dst_vf.stVFrame.pMbBlk);
        int dw = dst_vf.stVFrame.u32Width;
        int dh = dst_vf.stVFrame.u32Height;
        int dvw = dst_vf.stVFrame.u32VirWidth;
        printf("DST frame: %dx%d (vir %dx%d)\n", dw, dh, dvw, dst_vf.stVFrame.u32VirHeight);

        if (dst_ptr) {
            /* Анализ */
            printf("\n=== Углы DST ===\n");
            printf("dst[0,0]=%d (TL)\n", dst_ptr[0]);
            printf("dst[0,%d]=%d (TR)\n", dw-1, dst_ptr[dvw-1]);
            printf("dst[%d,0]=%d (BL)\n", dh-1, dst_ptr[(dh-1)*dvw]);
            printf("dst[%d,%d]=%d (BR)\n", dh-1, dw-1, dst_ptr[(dh-1)*dvw + dw-1]);

            long top_sum=0, bot_sum=0, left_sum=0, right_sum=0;
            int half_h = dh/2, half_w = dw/2;
            for (int y = 0; y < half_h; y++)
                for (int x = 0; x < dw; x++)
                    top_sum += dst_ptr[y * dvw + x];
            for (int y = half_h; y < dh; y++)
                for (int x = 0; x < dw; x++)
                    bot_sum += dst_ptr[y * dvw + x];
            for (int y = 0; y < dh; y++) {
                for (int x = 0; x < half_w; x++)
                    left_sum += dst_ptr[y * dvw + x];
                for (int x = half_w; x < dw; x++)
                    right_sum += dst_ptr[y * dvw + x];
            }
            long ta = top_sum/(half_h*dw), ba = bot_sum/((dh-half_h)*dw);
            long la = left_sum/(half_w*dh), ra = right_sum/((dw-half_w)*dh);
            printf("\ntop_avg=%ld bot_avg=%ld  left_avg=%ld right_avg=%ld\n", ta, ba, la, ra);

            printf("\n=== ИТОГ ===\n");
            if (ta < 30 && ba > 200)
                printf("✅ ROTATION 90° РАБОТАЕТ! (top=black, bot=white)\n");
            else if (ta > 200 && ba < 30)
                printf("✅ ROTATION 270° (top=white, bot=black)\n");
            else if (la < 30 && ra > 200)
                printf("❌ НЕТ РОТАЦИИ — кадр как есть (left=black, right=white)\n");
            else
                printf("??? Неожиданный паттерн (ta=%ld ba=%ld la=%ld ra=%ld)\n", ta, ba, la, ra);

            /* Сохраним */
            FILE* f = fopen("/tmp/iva_test/vpss_rot_out.yuv", "wb");
            if (f) {
                fwrite(dst_ptr, 1, dw * dh * 3 / 2, f);
                fclose(f);
                printf("Saved /tmp/iva_test/vpss_rot_out.yuv (%dx%d)\n", dw, dh);
            }
        }
        RK_MPI_VPSS_ReleaseChnFrame(grp, chn, &dst_vf);
    } else {
        printf("GetChnFrame failed!\n");
    }

vpss_cleanup:
    RK_MPI_VPSS_StopGrp(grp);
    RK_MPI_VPSS_DisableChn(grp, chn);
    RK_MPI_VPSS_DestroyGrp(grp);
cleanup:
    if (src_mb) RK_MPI_MB_ReleaseMB(src_mb);
    RK_MPI_SYS_Exit();
    return 0;
}
