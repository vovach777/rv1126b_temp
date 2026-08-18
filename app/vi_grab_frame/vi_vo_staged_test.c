/* vi_vo_staged_test.c — staged ch6 diagnostic
 *
 * Цель: локализовать момент, когда ch6 (rkvpss_scale4) ломает IVS (rkvpss_scale2).
 *
 * Стадии:
 *   1. Start ch4→IVS + ch5→VO. Wait 5s. Verify IVS counts frames.
 *   2. RK_MPI_VI_SetChnAttr(ch6). Wait 3s. Check IVS.
 *   3. RK_MPI_VI_EnableChn(ch6), NO bind. Wait 3s. Check IVS.
 *   4. SYS_Bind(ch6→VO chn3). Wait 3s. Check IVS.
 *   5. SYS_UnBind + DisableChn(ch6). Wait 5s. Check if IVS recovers.
 *   6. Graceful exit with return-code logging on every teardown API.
 *
 * Параллельно: GetChnFrame(ch4) поток — проверяет, выдаёт ли scale2 кадры.
 *
 * Запуск:
 *   systemctl stop s30gui
 *   /tmp/vi_vo_staged_test
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
#include "rk_mpi_mb.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_comm_vo.h"
#include "rk_comm_ivs.h"

/* ---- Размеры ---- */
#define CAM_W       1920
#define CAM_H       1080
#define DISP_W      720
#define DISP_H      1280

/* ---- VI ---- */
#define VI_DEV_CAM0     0
#define VI_PIPE_CAM0    0
#define VI_DEV_CAM1     1
#define VI_PIPE_CAM1    1
#define VI_CHN_IVS      4
#define VI_CHN_DISP     5
#define VI_CHN_MINIPIP  3   /* ext chn for mini PiP (cam0) — using ch3/scale1 */

/* ---- VO ---- */
#define VO_LAYER    0
#define VO_DEV      0
#define VO_CHN_CAM0     0
#define VO_CHN_CAM1     1
#define VO_CHN_CAM2     3

/* ---- IVS ---- */
#define IVS_CHN     0

static volatile int g_exit = 0;
static void sig_handler(int s) { (void)s; g_exit = 1; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ---- Counters ---- */
static volatile int g_ivs_frames = 0;       /* from IVS_GetResults */
static volatile int g_ivs_events = 0;
static volatile int g_getframe_count = 0;   /* from GetChnFrame(ch4) */
static volatile int g_getframe_timeout = 0;
static volatile int g_ch6_count = 0;        /* from GetChnFrame(ch6) */
static volatile int g_ch6_timeout = 0;
static volatile int g_ch6_active = 0;       /* set when ch6 enabled */

/* ========================================================================
 * IVS results thread — counts frames from IVS
 * ====================================================================== */
static void *ivs_thread(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "ivs_monitor");
    while (!g_exit) {
        IVS_RESULT_INFO_S stResults;
        memset(&stResults, 0, sizeof(stResults));
        int ret = RK_MPI_IVS_GetResults(IVS_CHN, &stResults, 1000);
        if (ret >= 0) {
            __sync_fetch_and_add(&g_ivs_frames, 1);
            if (stResults.s32ResultNum > 0)
                __sync_fetch_and_add(&g_ivs_events, 1);
            RK_MPI_IVS_ReleaseResults(IVS_CHN, &stResults);
        }
    }
    return NULL;
}

/* ========================================================================
 * GetChnFrame(ch4) thread — checks if scale2 itself produces frames
 * ====================================================================== */
static void *getframe_thread(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "getframe_ch4");
    while (!g_exit) {
        VIDEO_FRAME_INFO_S frame;
        memset(&frame, 0, sizeof(frame));
        int ret = RK_MPI_VI_GetChnFrame(VI_PIPE_CAM0, VI_CHN_IVS, &frame, 1000);
        if (ret == RK_SUCCESS) {
            __sync_fetch_and_add(&g_getframe_count, 1);
            RK_MPI_VI_ReleaseChnFrame(VI_PIPE_CAM0, VI_CHN_IVS, &frame);
        } else {
            __sync_fetch_and_add(&g_getframe_timeout, 1);
        }
    }
    return NULL;
}

/* ========================================================================
 * GetChnFrame(ch6) thread — checks if scale4 (offline) produces frames
 * ====================================================================== */
static void *getframe_ch6_thread(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "getframe_ch6");
    while (!g_exit) {
        if (!g_ch6_active) {
            usleep(100000);
            continue;
        }
        VIDEO_FRAME_INFO_S frame;
        memset(&frame, 0, sizeof(frame));
        int ret = RK_MPI_VI_GetChnFrame(VI_PIPE_CAM0, VI_CHN_MINIPIP, &frame, 1000);
        if (ret == RK_SUCCESS) {
            __sync_fetch_and_add(&g_ch6_count, 1);
            RK_MPI_VI_ReleaseChnFrame(VI_PIPE_CAM0, VI_CHN_MINIPIP, &frame);
        } else {
            __sync_fetch_and_add(&g_ch6_timeout, 1);
        }
    }
    return NULL;
}

/* ========================================================================
 * Snapshot — print current counters
 * ====================================================================== */
static void print_stats(const char *label)
{
    int ivs_f = g_ivs_frames, ivs_e = g_ivs_events;
    int gf = g_getframe_count, gf_to = g_getframe_timeout;
    int ch6 = g_ch6_count, ch6_to = g_ch6_timeout;
    printf("[STATS] %-30s  IVS: %d fr, %d ev | ch4: %d ok, %d to | ch6: %d ok, %d to\n",
           label, ivs_f, ivs_e, gf, gf_to, ch6, ch6_to);
    fflush(stdout);
}

static void reset_stats(void)
{
    g_ivs_frames = 0;
    g_ivs_events = 0;
    g_getframe_count = 0;
    g_getframe_timeout = 0;
    g_ch6_count = 0;
    g_ch6_timeout = 0;
}

/* ========================================================================
 * VI init — cam0 (pipe0) + cam1 (pipe1), ch4 + ch5 only (no ch6)
 * ====================================================================== */
static int vi_init(void)
{
    /* VI module param — all online (mirrorCmsc=0, extChn[]=0) */
    VI_PARAM_MOD_S stModParam;
    memset(&stModParam, 0, sizeof(stModParam));
    stModParam.enViModType = VI_EXT_CHN_MODE;
    stModParam.stExtChnParam.mirrorCmsc = 0;
    stModParam.stExtChnParam.extChn[0] = 0;  /* ch2 online */
    stModParam.stExtChnParam.extChn[1] = 0;  /* ch3 MINIPIP online */
    stModParam.stExtChnParam.extChn[2] = 0;  /* ch4 IVS online */
    stModParam.stExtChnParam.extChn[3] = 0;  /* ch5 DISP online */
    stModParam.stExtChnParam.extChn[4] = 0;  /* ch6 online (unused) */
    stModParam.stExtChnParam.extChn[5] = 0;  /* ch7 online (unused) */
    int ret = RK_MPI_VI_SetModParam(&stModParam);
    if (ret) fprintf(stderr, "VI_SetModParam: %#x (non-fatal)\n", ret);

    /* ---- Cam 0: dev 0, pipe 0 ---- */
    VI_DEV_ATTR_S stDevAttr;
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    ret = RK_MPI_VI_GetDevAttr(VI_DEV_CAM0, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(VI_DEV_CAM0, &stDevAttr);
        if (ret) { fprintf(stderr, "VI_SetDevAttr[0]: %#x\n", ret); return -1; }
    }
    ret = RK_MPI_VI_GetDevIsEnable(VI_DEV_CAM0);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(VI_DEV_CAM0);
        if (ret) { fprintf(stderr, "VI_EnableDev[0]: %#x\n", ret); return -1; }
        VI_DEV_BIND_PIPE_S stBindPipe;
        memset(&stBindPipe, 0, sizeof(stBindPipe));
        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = VI_PIPE_CAM0;
        ret = RK_MPI_VI_SetDevBindPipe(VI_DEV_CAM0, &stBindPipe);
        if (ret) { fprintf(stderr, "VI_SetDevBindPipe[0]: %#x\n", ret); return -1; }
    }

    /* ch4 (IVS) — u32Depth=1 for GetChnFrame access */
    VI_CHN_ATTR_S chnAttr;
    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.stIspOpt.u32BufCount = 3;       /* extra buffers for GetChnFrame */
    chnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chnAttr.stIspOpt.stMaxSize.u32Width = CAM_W;
    chnAttr.stIspOpt.stMaxSize.u32Height = CAM_H;
    chnAttr.stSize.u32Width = CAM_W;
    chnAttr.stSize.u32Height = CAM_H;
    chnAttr.enPixelFormat = RK_FMT_YUV420SP;
    chnAttr.enCompressMode = COMPRESS_MODE_NONE;
    chnAttr.u32Depth = 1;                   /* allow GetChnFrame + bind */
    ret = RK_MPI_VI_SetChnAttr(VI_PIPE_CAM0, VI_CHN_IVS, &chnAttr);
    if (ret) { fprintf(stderr, "VI_SetChnAttr[IVS]: %#x\n", ret); return -1; }
    ret = RK_MPI_VI_EnableChn(VI_PIPE_CAM0, VI_CHN_IVS);
    if (ret) { fprintf(stderr, "VI_EnableChn[IVS]: %#x\n", ret); return -1; }

    /* ch5 (DISP) */
    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.stIspOpt.u32BufCount = 3;
    chnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chnAttr.stSize.u32Width = CAM_W;
    chnAttr.stSize.u32Height = CAM_H;
    chnAttr.enPixelFormat = RK_FMT_YUV420SP;
    chnAttr.u32Depth = 0;
    ret = RK_MPI_VI_SetChnAttr(VI_PIPE_CAM0, VI_CHN_DISP, &chnAttr);
    if (ret) { fprintf(stderr, "VI_SetChnAttr[DISP]: %#x\n", ret); return -1; }
    ret = RK_MPI_VI_EnableChn(VI_PIPE_CAM0, VI_CHN_DISP);
    if (ret) { fprintf(stderr, "VI_EnableChn[DISP]: %#x\n", ret); return -1; }

    /* ---- Cam 1: dev 1, pipe 1 ---- */
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    ret = RK_MPI_VI_GetDevAttr(VI_DEV_CAM1, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(VI_DEV_CAM1, &stDevAttr);
        if (ret) { fprintf(stderr, "VI_SetDevAttr[1]: %#x\n", ret); return -1; }
    }
    ret = RK_MPI_VI_GetDevIsEnable(VI_DEV_CAM1);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(VI_DEV_CAM1);
        if (ret) { fprintf(stderr, "VI_EnableDev[1]: %#x\n", ret); return -1; }
        VI_DEV_BIND_PIPE_S stBindPipe;
        memset(&stBindPipe, 0, sizeof(stBindPipe));
        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = VI_PIPE_CAM1;
        ret = RK_MPI_VI_SetDevBindPipe(VI_DEV_CAM1, &stBindPipe);
        if (ret) { fprintf(stderr, "VI_SetDevBindPipe[1]: %#x\n", ret); return -1; }
    }

    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.stIspOpt.u32BufCount = 3;
    chnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chnAttr.stSize.u32Width = CAM_W;
    chnAttr.stSize.u32Height = CAM_H;
    chnAttr.enPixelFormat = RK_FMT_YUV420SP;
    chnAttr.u32Depth = 0;
    ret = RK_MPI_VI_SetChnAttr(VI_PIPE_CAM1, VI_CHN_DISP, &chnAttr);
    if (ret) { fprintf(stderr, "VI_SetChnAttr[DISP1]: %#x\n", ret); return -1; }
    ret = RK_MPI_VI_EnableChn(VI_PIPE_CAM1, VI_CHN_DISP);
    if (ret) { fprintf(stderr, "VI_EnableChn[DISP1]: %#x\n", ret); return -1; }

    printf("VI: cam0 (pipe0: ch4 IVS + ch5 DISP) + cam1 (pipe1: ch5 DISP)\n");
    return 0;
}

/* ========================================================================
 * VO init — 2 channels (cam0 full + cam1 PiP), no miniPiP yet
 * ====================================================================== */
static int vo_init(void)
{
    VO_PUB_ATTR_S VoPubAttr;
    VO_VIDEO_LAYER_ATTR_S stLayerAttr;
    VO_CSC_S VideoCSC;
    memset(&VoPubAttr, 0, sizeof(VoPubAttr));
    memset(&stLayerAttr, 0, sizeof(stLayerAttr));
    memset(&VideoCSC, 0, sizeof(VideoCSC));

    VoPubAttr.enIntfType = VO_INTF_MIPI;
    VoPubAttr.enIntfSync = VO_OUTPUT_DEFAULT;

    int ret = RK_MPI_VO_SetPubAttr(VO_DEV, &VoPubAttr);
    if (ret) { fprintf(stderr, "VO_SetPubAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_Enable(VO_DEV);
    if (ret) { fprintf(stderr, "VO_Enable: %#x\n", ret); return -1; }

    ret = RK_MPI_VO_SetLayerDispBufLen(VO_LAYER, 2);
    if (ret) fprintf(stderr, "VO_SetLayerDispBufLen: %#x (non-fatal)\n", ret);

    ret = RK_MPI_VO_GetPubAttr(VO_DEV, &VoPubAttr);
    if (ret) { fprintf(stderr, "VO_GetPubAttr: %#x\n", ret); return -1; }
    int dispW = VoPubAttr.stSyncInfo.u16Hact;
    int dispH = VoPubAttr.stSyncInfo.u16Vact;
    if (!dispW) { dispW = DISP_W; dispH = DISP_H; }

    stLayerAttr.stDispRect.s32X = 0;
    stLayerAttr.stDispRect.s32Y = 0;
    stLayerAttr.stDispRect.u32Width = dispW;
    stLayerAttr.stDispRect.u32Height = dispH;
    stLayerAttr.stImageSize.u32Width = dispW;
    stLayerAttr.stImageSize.u32Height = dispH;
    stLayerAttr.u32DispFrmRt = 30;
    stLayerAttr.enPixFormat = RK_FMT_RGB888;
    VideoCSC.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
    VideoCSC.u32Contrast = 50;
    VideoCSC.u32Hue = 50;
    VideoCSC.u32Luma = 50;
    VideoCSC.u32Satuature = 50;

    ret = RK_MPI_VO_BindLayer(VO_LAYER, VO_DEV, VO_LAYER_MODE_GRAPHIC);
    if (ret) { fprintf(stderr, "VO_BindLayer: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetLayerAttr(VO_LAYER, &stLayerAttr);
    if (ret) { fprintf(stderr, "VO_SetLayerAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetLayerSpliceMode(VO_LAYER, VO_SPLICE_MODE_RGA);
    if (ret) fprintf(stderr, "VO_SetLayerSpliceMode: %#x (non-fatal)\n", ret);
    ret = RK_MPI_VO_EnableLayer(VO_LAYER);
    if (ret) { fprintf(stderr, "VO_EnableLayer: %#x\n", ret); return -1; }

    VO_CHN_ATTR_S VoChnAttr;

    /* chn 0: cam0 full screen, priority=0, ROT90 */
    memset(&VoChnAttr, 0, sizeof(VoChnAttr));
    VoChnAttr.bDeflicker = RK_FALSE;
    VoChnAttr.u32Priority = 0;
    VoChnAttr.stRect.s32X = 0;
    VoChnAttr.stRect.s32Y = 0;
    VoChnAttr.stRect.u32Width = dispW;
    VoChnAttr.stRect.u32Height = dispH;
    VoChnAttr.enRotation = ROTATION_90;
    ret = RK_MPI_VO_SetChnAttr(VO_LAYER, VO_CHN_CAM0, &VoChnAttr);
    if (ret) { fprintf(stderr, "VO_SetChnAttr[cam0]: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableChn(VO_LAYER, VO_CHN_CAM0);
    if (ret) { fprintf(stderr, "VO_EnableChn[cam0]: %#x\n", ret); return -1; }

    /* chn 1: cam1 PiP, priority=1, ROT90 */
    int pip_w = dispW / 2, pip_h = dispH / 2;
    memset(&VoChnAttr, 0, sizeof(VoChnAttr));
    VoChnAttr.bDeflicker = RK_FALSE;
    VoChnAttr.u32Priority = 1;
    VoChnAttr.stRect.s32X = dispW - pip_w;
    VoChnAttr.stRect.s32Y = dispH - pip_h;
    VoChnAttr.stRect.u32Width = pip_w;
    VoChnAttr.stRect.u32Height = pip_h;
    VoChnAttr.enRotation = ROTATION_90;
    ret = RK_MPI_VO_SetChnAttr(VO_LAYER, VO_CHN_CAM1, &VoChnAttr);
    if (ret) { fprintf(stderr, "VO_SetChnAttr[cam1]: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableChn(VO_LAYER, VO_CHN_CAM1);
    if (ret) { fprintf(stderr, "VO_EnableChn[cam1]: %#x\n", ret); return -1; }

    /* Bind VI→VO */
    MPP_CHN_S vi_chn, vo_chn;
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 0; vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO; vo_chn.s32DevId = VO_LAYER; vo_chn.s32ChnId = VO_CHN_CAM0;
    ret = RK_MPI_SYS_Bind(&vi_chn, &vo_chn);
    if (ret) { fprintf(stderr, "SYS_Bind VI0→VO0: %#x\n", ret); return -1; }

    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 1; vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO; vo_chn.s32DevId = VO_LAYER; vo_chn.s32ChnId = VO_CHN_CAM1;
    ret = RK_MPI_SYS_Bind(&vi_chn, &vo_chn);
    if (ret) { fprintf(stderr, "SYS_Bind VI1→VO1: %#x\n", ret); return -1; }

    printf("VO: %dx%d, chn0 (cam0 full) + chn1 (cam1 PiP)\n", dispW, dispH);
    return 0;
}

/* ========================================================================
 * IVS init — bind VI ch4 → IVS
 * ====================================================================== */
static int ivs_init(void)
{
    IVS_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.enMode = IVS_MODE_MD_OD;
    attr.u32PicWidth = CAM_W;
    attr.u32PicHeight = CAM_H;
    attr.u32MaxWidth = CAM_W;
    attr.u32MaxHeight = CAM_H;
    attr.enPixelFormat = RK_FMT_YUV420SP;
    attr.s32Gop = 30;
    attr.bSmearEnable = RK_FALSE;
    attr.bWeightpEnable = RK_TRUE;
    attr.bMDEnable = RK_TRUE;
    attr.s32MDInterval = 1;
    attr.bMDNightMode = RK_TRUE;
    attr.u32MDSensibility = 5;
    attr.bODEnable = RK_FALSE;

    int ret = RK_MPI_IVS_CreateChn(IVS_CHN, &attr);
    if (ret) { fprintf(stderr, "IVS_CreateChn: %#x\n", ret); return -1; }

    IVS_MD_ATTR_S stMdAttr;
    memset(&stMdAttr, 0, sizeof(stMdAttr));
    ret = RK_MPI_IVS_GetMdAttr(IVS_CHN, &stMdAttr);
    if (ret) { fprintf(stderr, "IVS_GetMdAttr: %#x\n", ret); return -1; }
    stMdAttr.s32ThreshSad = 50;
    stMdAttr.s32ThreshMove = 2;
    ret = RK_MPI_IVS_SetMdAttr(IVS_CHN, &stMdAttr);
    if (ret) { fprintf(stderr, "IVS_SetMdAttr: %#x\n", ret); return -1; }

    MPP_CHN_S vi_chn, ivs_chn;
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 0; vi_chn.s32ChnId = VI_CHN_IVS;
    ivs_chn.enModId = RK_ID_IVS; ivs_chn.s32DevId = 0; ivs_chn.s32ChnId = IVS_CHN;
    ret = RK_MPI_SYS_Bind(&vi_chn, &ivs_chn);
    if (ret) { fprintf(stderr, "SYS_Bind VI→IVS: %#x\n", ret); return -1; }

    printf("IVS: MD on cam0 %dx%d\n", CAM_W, CAM_H);
    return 0;
}

/* ========================================================================
 * Staged ch6 operations
 * ====================================================================== */

/* Stage 2: SetChnAttr(ch6) only — u32Depth=1 for GetChnFrame access */
static int stage_setchn6(void)
{
    VI_CHN_ATTR_S chnAttr;
    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.stIspOpt.u32BufCount = 2;
    chnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chnAttr.stSize.u32Width = CAM_W;
    chnAttr.stSize.u32Height = CAM_H;
    chnAttr.enPixelFormat = RK_FMT_YUV420SP;
    chnAttr.u32Depth = 1;  /* allow GetChnFrame for diagnostics */
    chnAttr.stFrameRate.s32SrcFrameRate = 30;
    chnAttr.stFrameRate.s32DstFrameRate = 4;
    int ret = RK_MPI_VI_SetChnAttr(VI_PIPE_CAM0, VI_CHN_MINIPIP, &chnAttr);
    printf("[STAGE2] VI_SetChnAttr(ch3, online, depth=1): %#x\n", ret);
    return ret;
}

/* Stage 3: EnableChn(ch3), no bind — online mode */
static int stage_enablechn6(void)
{
    int ret = RK_MPI_VI_EnableChn(VI_PIPE_CAM0, VI_CHN_MINIPIP);
    printf("[STAGE3] VI_EnableChn(ch3 online): %#x\n", ret);
    if (ret == RK_SUCCESS)
        g_ch6_active = 1;
    return ret;
}

/* Stage 4: Bind ch6→VO chn3 */
static int stage_bindchn6(void)
{
    /* Create VO chn3 first */
    VO_CHN_ATTR_S VoChnAttr;
    memset(&VoChnAttr, 0, sizeof(VoChnAttr));
    VoChnAttr.bDeflicker = RK_FALSE;
    VoChnAttr.u32Priority = 3;
    VoChnAttr.stRect.s32X = DISP_W - 180;
    VoChnAttr.stRect.s32Y = 0;
    VoChnAttr.stRect.u32Width = 180;
    VoChnAttr.stRect.u32Height = 320;
    VoChnAttr.enRotation = ROTATION_90;
    int ret = RK_MPI_VO_SetChnAttr(VO_LAYER, VO_CHN_CAM2, &VoChnAttr);
    printf("[STAGE4] VO_SetChnAttr(chn3): %#x\n", ret);
    if (ret) return ret;
    ret = RK_MPI_VO_EnableChn(VO_LAYER, VO_CHN_CAM2);
    printf("[STAGE4] VO_EnableChn(chn3): %#x\n", ret);
    if (ret) return ret;

    MPP_CHN_S vi_chn, vo_chn;
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 0; vi_chn.s32ChnId = VI_CHN_MINIPIP;
    vo_chn.enModId = RK_ID_VO; vo_chn.s32DevId = VO_LAYER; vo_chn.s32ChnId = VO_CHN_CAM2;
    ret = RK_MPI_SYS_Bind(&vi_chn, &vo_chn);
    printf("[STAGE4] SYS_Bind(ch3→VO3): %#x\n", ret);
    return ret;
}

/* Stage 5: Unbind + DisableChn(ch3) */
static void stage_disablechn6(void)
{
    g_ch6_active = 0;
    MPP_CHN_S vi_chn, vo_chn;
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 0; vi_chn.s32ChnId = VI_CHN_MINIPIP;
    vo_chn.enModId = RK_ID_VO; vo_chn.s32DevId = VO_LAYER; vo_chn.s32ChnId = VO_CHN_CAM2;
    int ret = RK_MPI_SYS_UnBind(&vi_chn, &vo_chn);
    printf("[STAGE5] SYS_UnBind(ch3→VO3): %#x\n", ret);

    ret = RK_MPI_VO_DisableChn(VO_LAYER, VO_CHN_CAM2);
    printf("[STAGE5] VO_DisableChn(chn3): %#x\n", ret);

    ret = RK_MPI_VI_DisableChn(VI_PIPE_CAM0, VI_CHN_MINIPIP);
    printf("[STAGE5] VI_DisableChn(ch3): %#x\n", ret);
}

/* ========================================================================
 * Teardown with return-code logging
 * ====================================================================== */
static void teardown(void)
{
    int ret;
    MPP_CHN_S vi_chn, vo_chn;

    /* Unbind miniPiP if it was bound */
    /* (handled in stage_disablechn6 if stage 5 ran) */

    /* Unbind IVS */
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 0; vi_chn.s32ChnId = VI_CHN_IVS;
    vo_chn.enModId = RK_ID_IVS; vo_chn.s32DevId = 0; vo_chn.s32ChnId = IVS_CHN;
    ret = RK_MPI_SYS_UnBind(&vi_chn, &vo_chn);
    printf("[TEARDOWN] SYS_UnBind(VI4→IVS): %#x\n", ret);

    ret = RK_MPI_IVS_DestroyChn(IVS_CHN);
    printf("[TEARDOWN] IVS_DestroyChn: %#x\n", ret);

    /* Unbind cam1 */
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 1; vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO; vo_chn.s32DevId = VO_LAYER; vo_chn.s32ChnId = VO_CHN_CAM1;
    ret = RK_MPI_SYS_UnBind(&vi_chn, &vo_chn);
    printf("[TEARDOWN] SYS_UnBind(VI1→VO1): %#x\n", ret);

    /* Unbind cam0 */
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 0; vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO; vo_chn.s32DevId = VO_LAYER; vo_chn.s32ChnId = VO_CHN_CAM0;
    ret = RK_MPI_SYS_UnBind(&vi_chn, &vo_chn);
    printf("[TEARDOWN] SYS_UnBind(VI0→VO0): %#x\n", ret);

    /* Disable VO channels */
    ret = RK_MPI_VO_DisableChn(VO_LAYER, VO_CHN_CAM1);
    printf("[TEARDOWN] VO_DisableChn(cam1): %#x\n", ret);
    ret = RK_MPI_VO_DisableChn(VO_LAYER, VO_CHN_CAM0);
    printf("[TEARDOWN] VO_DisableChn(cam0): %#x\n", ret);
    ret = RK_MPI_VO_DisableLayer(VO_LAYER);
    printf("[TEARDOWN] VO_DisableLayer: %#x\n", ret);
    ret = RK_MPI_VO_Disable(VO_DEV);
    printf("[TEARDOWN] VO_Disable: %#x\n", ret);
    ret = RK_MPI_VO_UnBindLayer(VO_LAYER, VO_DEV);
    printf("[TEARDOWN] VO_UnBindLayer: %#x\n", ret);

    /* Disable VI channels */
    ret = RK_MPI_VI_DisableChn(VI_PIPE_CAM1, VI_CHN_DISP);
    printf("[TEARDOWN] VI_DisableChn(pipe1/ch5): %#x\n", ret);
    ret = RK_MPI_VI_DisableChn(VI_PIPE_CAM0, VI_CHN_DISP);
    printf("[TEARDOWN] VI_DisableChn(pipe0/ch5): %#x\n", ret);
    ret = RK_MPI_VI_DisableChn(VI_PIPE_CAM0, VI_CHN_IVS);
    printf("[TEARDOWN] VI_DisableChn(pipe0/ch4): %#x\n", ret);

    /* Disable VI devices */
    ret = RK_MPI_VI_DisableDev(VI_DEV_CAM1);
    printf("[TEARDOWN] VI_DisableDev(1): %#x\n", ret);
    ret = RK_MPI_VI_DisableDev(VI_DEV_CAM0);
    printf("[TEARDOWN] VI_DisableDev(0): %#x\n", ret);

    ret = RK_MPI_SYS_Exit();
    printf("[TEARDOWN] SYS_Exit: %#x\n", ret);
    printf("cleanup done\n");
}

/* ========================================================================
 * Main — staged test
 * ====================================================================== */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    prctl(PR_SET_NAME, "vi_staged_test");

    printf("\n=== vi_vo_staged_test (ch3 MINIPIP, online) ===\n");
    printf("    Staged ch3 diagnostic: does online ch3/scale1 kill IVS?\n");
    printf("    ch4 (IVS online) + ch5 (DISP online) → then add ch3 online\n\n");

    int ret = RK_MPI_SYS_Init();
    if (ret) { fprintf(stderr, "SYS_Init: %#x\n", ret); return 1; }

    if (vi_init() < 0) { teardown(); return 1; }
    if (vo_init() < 0) { teardown(); return 1; }
    if (ivs_init() < 0) { teardown(); return 1; }

    /* Start monitor threads */
    pthread_t ivs_tid, gf_tid, ch6_tid;
    pthread_create(&ivs_tid, NULL, ivs_thread, NULL);
    pthread_create(&gf_tid, NULL, getframe_thread, NULL);
    pthread_create(&ch6_tid, NULL, getframe_ch6_thread, NULL);

    /* ---- STAGE 1: baseline, wait 5s ---- */
    printf("\n--- STAGE 1: baseline (ch4+ch5 only), wait 5s ---\n");
    reset_stats();
    sleep(5);
    print_stats("stage1-baseline");

    if (g_exit) goto done;

    /* ---- STAGE 2: SetChnAttr(ch3) only ---- */
    printf("\n--- STAGE 2: VI_SetChnAttr(ch3 online), wait 3s ---\n");
    if (stage_setchn6() != RK_SUCCESS) { printf("STAGE2 FAILED, aborting\n"); goto done; }
    reset_stats();
    sleep(3);
    print_stats("stage2-setattr");

    if (g_exit) goto done;

    /* ---- STAGE 3: EnableChn(ch3 online), no bind, wait 10s ---- */
    printf("\n--- STAGE 3: VI_EnableChn(ch3 online) NO bind, wait 10s ---\n");
    if (stage_enablechn6() != RK_SUCCESS) { printf("STAGE3 FAILED, aborting\n"); goto done; }
    reset_stats();
    sleep(10);
    print_stats("stage3-enable-online");

    if (g_exit) goto done;

    /* ---- STAGE 4: Bind ch3→VO chn3 ---- */
    printf("\n--- STAGE 4: SYS_Bind(ch3→VO3), wait 3s ---\n");
    if (stage_bindchn6() != RK_SUCCESS) { printf("STAGE4 FAILED, aborting\n"); goto done; }
    reset_stats();
    sleep(3);
    print_stats("stage4-bind");

    if (g_exit) goto done;

    /* ---- STAGE 5: Unbind + DisableChn(ch3) ---- */
    printf("\n--- STAGE 5: Disable ch3, wait 5s (check recovery) ---\n");
    stage_disablechn6();
    reset_stats();
    sleep(5);
    print_stats("stage5-after-disable");

done:
    g_exit = 1;
    g_ch6_active = 0;
    printf("\n--- stopping threads ---\n");
    pthread_join(ivs_tid, NULL);
    pthread_join(gf_tid, NULL);
    pthread_join(ch6_tid, NULL);

    teardown();
    return 0;
}
