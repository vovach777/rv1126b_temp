/* vi_vo_bind_test.c — тест VI ext chn5 → SYS_Bind → VO (zero-app-frame)
 *
 * Архитектура (по образцу rkipc_pipe_vi_vo_init):
 *
 *   GC2093 → VI pipe 0
 *             ├─ ext chn 4 (depth=0) → SYS_Bind → IVS MD
 *             └─ ext chn 5 (depth=0) → SYS_Bind → VO (GRAPHIC, RGB888, ROT90)
 *
 *   Никакого GetChnFrame / RGA / MMZ / SendFrame в приложении.
 *   Rockit сам гонит кадры VI→VO. VO делает rotate90.
 *
 *   Зелёный экран НЕ реализован в этом тесте — только проверка bind path.
 *
 * Запуск:
 *   systemctl stop s30gui
 *   /tmp/vi_vo_bind_test
 *   sleep 5 && modetest -M rockchip -w 59:zpos:0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/prctl.h>

#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_ivs.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_comm_vo.h"
#include "rk_comm_ivs.h"

/* ---- Размеры ---- */
#define CAM_W       1920
#define CAM_H       1080
#define IVS_W       1920
#define IVS_H       1080

/* ---- VI ---- */
#define VI_PIPE         0
#define VI_DEV          0
#define VI_CHN_IVS      4
#define VI_CHN_DISP     5

/* ---- VO ---- */
#define VO_LAYER    0
#define VO_DEV      0
#define VO_CHN      0

/* ---- IVS ---- */
#define IVS_CHN     0

static volatile int g_exit = 0;
static void sig_handler(int s) { (void)s; g_exit = 1; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static volatile int g_md_frame_count = 0;
static volatile int g_md_event_count = 0;

/* ========================================================================
 * VI init — ext channels only, no physical chn, no VPSS
 * ====================================================================== */
static int vi_init(void)
{
    int ret = 0;

    /* Dev + Pipe */
    VI_DEV_ATTR_S stDevAttr;
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    ret = RK_MPI_VI_GetDevAttr(VI_DEV, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(VI_DEV, &stDevAttr);
        if (ret) { fprintf(stderr, "VI_SetDevAttr: %#x\n", ret); return -1; }
    }
    ret = RK_MPI_VI_GetDevIsEnable(VI_DEV);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(VI_DEV);
        if (ret) { fprintf(stderr, "VI_EnableDev: %#x\n", ret); return -1; }
        VI_DEV_BIND_PIPE_S stBindPipe;
        memset(&stBindPipe, 0, sizeof(stBindPipe));
        stBindPipe.u32Num = VI_PIPE;
        stBindPipe.PipeId[0] = VI_PIPE;
        ret = RK_MPI_VI_SetDevBindPipe(VI_DEV, &stBindPipe);
        if (ret) { fprintf(stderr, "VI_SetDevBindPipe: %#x\n", ret); return -1; }
    }

    /* VI_EXT_CHN_MODE + mirrorCmsc=0 */
    VI_PARAM_MOD_S stModParam;
    memset(&stModParam, 0, sizeof(stModParam));
    stModParam.enViModType = VI_EXT_CHN_MODE;
    stModParam.stExtChnParam.mirrorCmsc = 0;
    ret = RK_MPI_VI_SetModParam(&stModParam);
    if (ret) fprintf(stderr, "VI_SetModParam: %#x (non-fatal)\n", ret);

    /* Ext chn 4 — IVS MD (depth=0, bound to IVS) */
    VI_CHN_ATTR_S chnAttr;
    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.stIspOpt.u32BufCount = 2;
    chnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chnAttr.stIspOpt.stMaxSize.u32Width = IVS_W;
    chnAttr.stIspOpt.stMaxSize.u32Height = IVS_H;
    chnAttr.stSize.u32Width = IVS_W;
    chnAttr.stSize.u32Height = IVS_H;
    chnAttr.enPixelFormat = RK_FMT_YUV420SP;
    chnAttr.enCompressMode = COMPRESS_MODE_NONE;
    chnAttr.u32Depth = 0;
    ret = RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN_IVS, &chnAttr);
    if (ret) { fprintf(stderr, "VI_SetChnAttr[IVS]: %#x\n", ret); return -1; }
    ret = RK_MPI_VI_EnableChn(VI_PIPE, VI_CHN_IVS);
    if (ret) { fprintf(stderr, "VI_EnableChn[IVS]: %#x\n", ret); return -1; }

    /* Ext chn 5 — display (depth=0, bound to VO via SYS_Bind) */
    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.stIspOpt.u32BufCount = 3;
    chnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chnAttr.stSize.u32Width = CAM_W;
    chnAttr.stSize.u32Height = CAM_H;
    chnAttr.enPixelFormat = RK_FMT_YUV420SP;
    chnAttr.u32Depth = 0;  /* bound to VO, app не читает */
    ret = RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN_DISP, &chnAttr);
    if (ret) { fprintf(stderr, "VI_SetChnAttr[DISP]: %#x\n", ret); return -1; }
    ret = RK_MPI_VI_EnableChn(VI_PIPE, VI_CHN_DISP);
    if (ret) { fprintf(stderr, "VI_EnableChn[DISP]: %#x\n", ret); return -1; }

    printf("VI: pipe %d, ext chn %d (IVS %dx%d) + ext chn %d (DISP %dx%d, bound)\n",
           VI_PIPE, VI_CHN_IVS, IVS_W, IVS_H, VI_CHN_DISP, CAM_W, CAM_H);
    return 0;
}

static void vi_deinit(void)
{
    RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN_DISP);
    RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN_IVS);
    RK_MPI_VI_DisableDev(VI_DEV);
}

/* ========================================================================
 * VO init — по образцу rkipc_pipe_vi_vo_init
 * GRAPHIC mode, RGB888, ROTATION_90, DispBufLen=2
 * ====================================================================== */
static int vo_init(void)
{
    VO_PUB_ATTR_S VoPubAttr;
    VO_VIDEO_LAYER_ATTR_S stLayerAttr;
    VO_CSC_S VideoCSC;
    VO_CHN_ATTR_S VoChnAttr;
    memset(&VoPubAttr, 0, sizeof(VoPubAttr));
    memset(&stLayerAttr, 0, sizeof(stLayerAttr));
    memset(&VideoCSC, 0, sizeof(VideoCSC));
    memset(&VoChnAttr, 0, sizeof(VoChnAttr));

    VoPubAttr.enIntfType = VO_INTF_MIPI;
    VoPubAttr.enIntfSync = VO_OUTPUT_DEFAULT;

    int ret = RK_MPI_VO_SetPubAttr(VO_DEV, &VoPubAttr);
    if (ret) { fprintf(stderr, "VO_SetPubAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_Enable(VO_DEV);
    if (ret) { fprintf(stderr, "VO_Enable: %#x\n", ret); return -1; }

    /* DispBufLen = 2 (как в rkipc) */
    ret = RK_MPI_VO_SetLayerDispBufLen(VO_LAYER, 2);
    if (ret) fprintf(stderr, "VO_SetLayerDispBufLen: %#x (non-fatal)\n", ret);

    /* Получаем реальные размеры дисплея */
    ret = RK_MPI_VO_GetPubAttr(VO_DEV, &VoPubAttr);
    if (ret) { fprintf(stderr, "VO_GetPubAttr: %#x\n", ret); return -1; }
    int dispW = VoPubAttr.stSyncInfo.u16Hact;
    int dispH = VoPubAttr.stSyncInfo.u16Vact;
    if (!dispW) { dispW = 720; dispH = 1280; }

    stLayerAttr.stDispRect.s32X = 0;
    stLayerAttr.stDispRect.s32Y = 0;
    stLayerAttr.stDispRect.u32Width = dispW;
    stLayerAttr.stDispRect.u32Height = dispH;
    stLayerAttr.stImageSize.u32Width = dispW;
    stLayerAttr.stImageSize.u32Height = dispH;
    stLayerAttr.u32DispFrmRt = 30;
    stLayerAttr.enPixFormat = RK_FMT_RGB888;  /* rkipc использует RGB888 */
    VideoCSC.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
    VideoCSC.u32Contrast = 50;
    VideoCSC.u32Hue = 50;
    VideoCSC.u32Luma = 50;
    VideoCSC.u32Satuature = 50;

    /* GRAPHIC mode (не VIDEO!) — как в rkipc */
    ret = RK_MPI_VO_BindLayer(VO_LAYER, VO_DEV, VO_LAYER_MODE_GRAPHIC);
    if (ret) { fprintf(stderr, "VO_BindLayer: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetLayerAttr(VO_LAYER, &stLayerAttr);
    if (ret) { fprintf(stderr, "VO_SetLayerAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetLayerSpliceMode(VO_LAYER, VO_SPLICE_MODE_RGA);
    if (ret) { fprintf(stderr, "VO_SetLayerSpliceMode: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableLayer(VO_LAYER);
    if (ret) { fprintf(stderr, "VO_EnableLayer: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetLayerCSC(VO_LAYER, &VideoCSC);
    if (ret) { fprintf(stderr, "VO_SetLayerCSC: %#x\n", ret); return -1; }

    /* VO channel с ROTATION_90 (VO делает rotate, не RGA) */
    VoChnAttr.bDeflicker = RK_FALSE;
    VoChnAttr.u32Priority = 1;
    VoChnAttr.stRect.s32X = 0;
    VoChnAttr.stRect.s32Y = 0;
    VoChnAttr.stRect.u32Width = dispW;
    VoChnAttr.stRect.u32Height = dispH;
    VoChnAttr.enRotation = ROTATION_90;  /* MIPI → rotate90 на VO */
    ret = RK_MPI_VO_SetChnAttr(VO_LAYER, VO_CHN, &VoChnAttr);
    if (ret) { fprintf(stderr, "VO_SetChnAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableChn(VO_LAYER, VO_CHN);
    if (ret) { fprintf(stderr, "VO_EnableChn: %#x\n", ret); return -1; }

    /* SYS_Bind VI chn5 → VO chn 0 (как rkipc, строки 2046-2055) */
    MPP_CHN_S vi_chn, vo_chn;
    vi_chn.enModId = RK_ID_VI;
    vi_chn.s32DevId = 0;
    vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO;
    vo_chn.s32DevId = VO_LAYER;
    vo_chn.s32ChnId = VO_CHN;
    ret = RK_MPI_SYS_Bind(&vi_chn, &vo_chn);
    if (ret) { fprintf(stderr, "SYS_Bind VI→VO: %#x\n", ret); return -1; }

    printf("VO: %dx%d, layer=%d (GRAPHIC+RGA, RGB888, ROTATION_90, bound to VI chn%d)\n",
           dispW, dispH, VO_LAYER, VI_CHN_DISP);
    return 0;
}

static void vo_deinit(void)
{
    /* Unbind first (как rkipc_pipe_vi_vo_deinit) */
    MPP_CHN_S vi_chn, vo_chn;
    vi_chn.enModId = RK_ID_VI;
    vi_chn.s32DevId = 0;
    vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO;
    vo_chn.s32DevId = VO_LAYER;
    vo_chn.s32ChnId = VO_CHN;
    RK_MPI_SYS_UnBind(&vi_chn, &vo_chn);

    RK_MPI_VO_DisableChn(VO_LAYER, VO_CHN);
    RK_MPI_VO_DisableLayer(VO_LAYER);
    RK_MPI_VO_Disable(VO_DEV);
    RK_MPI_VO_UnBindLayer(VO_LAYER, VO_DEV);
}

/* ========================================================================
 * IVS MD init — bind VI ext chn4 → IVS
 * ====================================================================== */
static int ivs_init(void)
{
    IVS_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.enMode = IVS_MODE_MD_OD;
    attr.u32PicWidth = IVS_W;
    attr.u32PicHeight = IVS_H;
    attr.u32MaxWidth = IVS_W;
    attr.u32MaxHeight = IVS_H;
    attr.enPixelFormat = RK_FMT_YUV420SP;
    attr.s32Gop = 30;
    attr.bSmearEnable = RK_FALSE;
    attr.bWeightpEnable = RK_TRUE;
    attr.bMDEnable = RK_TRUE;
    attr.s32MDInterval = 1;
    attr.bMDNightMode = RK_TRUE;
    attr.u32MDSensibility = 3;
    attr.bODEnable = RK_FALSE;
    attr.s32ODInterval = 1;
    attr.s32ODPercent = 6;

    int ret = RK_MPI_IVS_CreateChn(IVS_CHN, &attr);
    if (ret) { fprintf(stderr, "IVS_CreateChn: %#x\n", ret); return -1; }

    IVS_MD_ATTR_S stMdAttr;
    memset(&stMdAttr, 0, sizeof(stMdAttr));
    ret = RK_MPI_IVS_GetMdAttr(IVS_CHN, &stMdAttr);
    if (ret) { fprintf(stderr, "IVS_GetMdAttr: %#x\n", ret); return -1; }
    stMdAttr.s32ThreshSad = 80;
    stMdAttr.s32ThreshMove = 2;
    stMdAttr.s32SwitchSad = 0;
    stMdAttr.bFlycatkinFlt = RK_TRUE;
    stMdAttr.s32ThresDustMove = 3;
    stMdAttr.s32ThresDustBlk = 3;
    stMdAttr.s32ThresDustChng = 50;
    ret = RK_MPI_IVS_SetMdAttr(IVS_CHN, &stMdAttr);
    if (ret) { fprintf(stderr, "IVS_SetMdAttr: %#x\n", ret); return -1; }

    MPP_CHN_S vi_chn, ivs_chn;
    vi_chn.enModId = RK_ID_VI;
    vi_chn.s32DevId = 0;
    vi_chn.s32ChnId = VI_CHN_IVS;
    ivs_chn.enModId = RK_ID_IVS;
    ivs_chn.s32DevId = 0;
    ivs_chn.s32ChnId = IVS_CHN;
    ret = RK_MPI_SYS_Bind(&vi_chn, &ivs_chn);
    if (ret) { fprintf(stderr, "SYS_Bind VI→IVS: %#x\n", ret); return -1; }

    printf("IVS: MD on %dx%d, sensibility=3, threshSad=80, threshMove=2\n", IVS_W, IVS_H);
    return 0;
}

static void ivs_deinit(void)
{
    MPP_CHN_S vi_chn, ivs_chn;
    vi_chn.enModId = RK_ID_VI;
    vi_chn.s32DevId = 0;
    vi_chn.s32ChnId = VI_CHN_IVS;
    ivs_chn.enModId = RK_ID_IVS;
    ivs_chn.s32DevId = 0;
    ivs_chn.s32ChnId = IVS_CHN;
    RK_MPI_SYS_UnBind(&vi_chn, &ivs_chn);
    RK_MPI_IVS_DestroyChn(IVS_CHN);
}

/* ========================================================================
 * IVS results thread — мониторинг MD
 * ====================================================================== */
static void *ivs_results_thread(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "IvsMdResults", 0, 0, 0);

    int md_area_threshold = IVS_W * IVS_H * 1 / 100;
    double last_report = now_ms();
    int frame_count = 0;

    while (!g_exit) {
        IVS_RESULT_INFO_S stResults;
        memset(&stResults, 0, sizeof(stResults));
        int ret = RK_MPI_IVS_GetResults(IVS_CHN, &stResults, 1000);
        if (ret >= 0) {
            frame_count++;
            g_md_frame_count++;

            if (stResults.s32ResultNum > 0 &&
                stResults.pstResults->stMdInfo.u32Square > md_area_threshold) {
                g_md_event_count++;
            }

            RK_MPI_IVS_ReleaseResults(IVS_CHN, &stResults);
        } else {
            usleep(50000);
        }

        double now = now_ms();
        if (now - last_report > 5000) {
            printf("[IVS] %d frames, %d motion events\n",
                   frame_count, g_md_event_count);
            fflush(stdout);
            frame_count = 0;
            g_md_event_count = 0;
            last_report = now;
        }
    }
    return NULL;
}

/* ========================================================================
 * Main
 * ====================================================================== */
int main(int argc, char *argv[])
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("\n=== vi_vo_bind_test (VI→VO SYS_Bind, zero-app-frame) ===\n");
    printf("    VI pipe %d (GC2093 %dx%d)\n", VI_PIPE, CAM_W, CAM_H);
    printf("      → ext chn %d → SYS_Bind → IVS MD (%dx%d)\n",
           VI_CHN_IVS, IVS_W, IVS_H);
    printf("      → ext chn %d → SYS_Bind → VO (GRAPHIC, RGB888, ROT90)\n",
           VI_CHN_DISP);
    printf("    No GetChnFrame, no RGA, no MMZ, no SendFrame in app\n");
    printf("    Rockit pumps frames internally\n\n");
    fflush(stdout);

    int ret = RK_MPI_SYS_Init();
    if (ret) { fprintf(stderr, "SYS_Init: %#x\n", ret); return 1; }

    if (vi_init() < 0) goto cleanup_sys;
    if (vo_init() < 0) goto cleanup_vi;
    if (ivs_init() < 0) goto cleanup_vo;

    pthread_t ivs_tid;
    pthread_create(&ivs_tid, NULL, ivs_results_thread, NULL);

    printf("\n=== RUNNING (Ctrl-C to stop) ===\n");
    printf("    Video should be on display (OVERLAY z=0)\n");
    printf("    Run: modetest -M rockchip -w 59:zpos:0\n\n");
    fflush(stdout);

    double t0 = now_ms();
    while (!g_exit) {
        double elapsed = now_ms() - t0;
        if (elapsed > 120000) break;
        sleep(3);
        printf("[%.0fs] running... IVS frames=%d events=%d\n",
               elapsed / 1000.0, g_md_frame_count, g_md_event_count);
        fflush(stdout);
    }

    g_exit = 1;
    pthread_join(ivs_tid, NULL);

cleanup_ivs:
    ivs_deinit();
cleanup_vo:
    vo_deinit();
cleanup_vi:
    vi_deinit();
cleanup_sys:
    RK_MPI_SYS_Exit();
    printf("cleanup done\n");
    return 0;
}
