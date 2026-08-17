/* vi_vo_compose_alpha_test.c — 2 камеры + green border + PiP alpha эксперимент
 *
 * Архитектура (по паттерну rkadk_dual_disp_test + rkadk_ui):
 *
 *   GC2093 #1 → VI dev 0 / pipe 0
 *                ├─ ext chn 4 → SYS_Bind → IVS MD
 *                └─ ext chn 5 → SYS_Bind → VO chn 0 (full screen, priority=0, ROT90)
 *
 *   GC2093 #2 → VI dev 1 / pipe 1
 *                └─ ext chn 5 → SYS_Bind → VO chn 1 (PiP 1/4, priority=1, ROT90)
 *                                u32FgAlpha = pip_alpha (CLI, default 128)
 *
 *   App → MMZ RGBA8888 → VO chn 2 (green border, priority=2, SendFrame)
 *                         alpha=255 при MD, alpha=0 без MD
 *
 *   VO layer 0: GRAPHIC + RGA splice (composites chn 0 + chn 1 + chn 2)
 *   VOP2: один OVERLAY plane → MIPI display 720x1280
 *
 * Запуск:
 *   systemctl stop s30gui
 *   /tmp/vi_vo_compose_alpha_test [pip_alpha]
 *   sleep 5 && modetest -M rockchip -w 59:zpos:0
 *
 *   pip_alpha: 0-255, default 128 (полупрозрачный PiP)
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
#include "rk_mpi_mmz.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_comm_vo.h"
#include "rk_comm_ivs.h"

/* ---- Размеры ---- */
#define CAM_W       1920
#define CAM_H       1080
#define IVS_W       1920
#define IVS_H       1080
#define DISP_W      720
#define DISP_H      1280

/* ---- VI ---- */
#define VI_DEV_CAM0     0
#define VI_PIPE_CAM0    0
#define VI_DEV_CAM1     1
#define VI_PIPE_CAM1    1

#define VI_CHN_IVS      4
#define VI_CHN_DISP     5

/* ---- VO ---- */
#define VO_LAYER    0
#define VO_DEV      0
#define VO_CHN_CAM0     0   /* full screen */
#define VO_CHN_CAM1     1   /* PiP */
#define VO_CHN_UI       2   /* green border */

/* ---- IVS ---- */
#define IVS_CHN     0

/* ---- Green border ---- */
#define BORDER_THICKNESS  20

static volatile int g_exit = 0;
static int g_pip_alpha = 128;  /* 0=прозрачный, 255=непрозрачный */
static void sig_handler(int s) { (void)s; g_exit = 1; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static volatile int g_md_frame_count = 0;
static volatile int g_md_event_count = 0;
static volatile int g_md_active = 0;

/* ========================================================================
 * VI init — two cameras, ext channels, no VPSS
 * ====================================================================== */
static int vi_init_cam(int dev_id, int pipe_id, int enable_ivs)
{
    int ret = 0;

    /* Dev */
    VI_DEV_ATTR_S stDevAttr;
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    ret = RK_MPI_VI_GetDevAttr(dev_id, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(dev_id, &stDevAttr);
        if (ret) { fprintf(stderr, "VI_SetDevAttr[%d]: %#x\n", dev_id, ret); return -1; }
    }
    ret = RK_MPI_VI_GetDevIsEnable(dev_id);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(dev_id);
        if (ret) { fprintf(stderr, "VI_EnableDev[%d]: %#x\n", dev_id, ret); return -1; }
        VI_DEV_BIND_PIPE_S stBindPipe;
        memset(&stBindPipe, 0, sizeof(stBindPipe));
        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = pipe_id;
        ret = RK_MPI_VI_SetDevBindPipe(dev_id, &stBindPipe);
        if (ret) { fprintf(stderr, "VI_SetDevBindPipe[%d]: %#x\n", dev_id, ret); return -1; }
    }

    /* IVS ext chn (only for cam 0) */
    if (enable_ivs) {
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
        ret = RK_MPI_VI_SetChnAttr(pipe_id, VI_CHN_IVS, &chnAttr);
        if (ret) { fprintf(stderr, "VI_SetChnAttr[IVS pipe%d]: %#x\n", pipe_id, ret); return -1; }
        ret = RK_MPI_VI_EnableChn(pipe_id, VI_CHN_IVS);
        if (ret) { fprintf(stderr, "VI_EnableChn[IVS pipe%d]: %#x\n", pipe_id, ret); return -1; }
    }

    /* Display ext chn */
    VI_CHN_ATTR_S chnAttr;
    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.stIspOpt.u32BufCount = 3;
    chnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chnAttr.stSize.u32Width = CAM_W;
    chnAttr.stSize.u32Height = CAM_H;
    chnAttr.enPixelFormat = RK_FMT_YUV420SP;
    chnAttr.u32Depth = 0;  /* bound to VO */
    ret = RK_MPI_VI_SetChnAttr(pipe_id, VI_CHN_DISP, &chnAttr);
    if (ret) { fprintf(stderr, "VI_SetChnAttr[DISP pipe%d]: %#x\n", pipe_id, ret); return -1; }
    ret = RK_MPI_VI_EnableChn(pipe_id, VI_CHN_DISP);
    if (ret) { fprintf(stderr, "VI_EnableChn[DISP pipe%d]: %#x\n", pipe_id, ret); return -1; }

    printf("VI: dev %d pipe %d — ext chn %d%s + ext chn %d (DISP %dx%d, bound)\n",
           dev_id, pipe_id, enable_ivs ? VI_CHN_IVS : -1,
           enable_ivs ? " (IVS)" : "",
           VI_CHN_DISP, CAM_W, CAM_H);
    return 0;
}

static int vi_init(void)
{
    int ret;

    /* VI_EXT_CHN_MODE + mirrorCmsc=0 — глобально, один раз */
    VI_PARAM_MOD_S stModParam;
    memset(&stModParam, 0, sizeof(stModParam));
    stModParam.enViModType = VI_EXT_CHN_MODE;
    stModParam.stExtChnParam.mirrorCmsc = 0;
    ret = RK_MPI_VI_SetModParam(&stModParam);
    if (ret) fprintf(stderr, "VI_SetModParam: %#x (non-fatal)\n", ret);

    /* Camera 0: dev 0, pipe 0, with IVS */
    if (vi_init_cam(VI_DEV_CAM0, VI_PIPE_CAM0, 1) < 0) return -1;
    /* Camera 1: dev 1, pipe 1, no IVS */
    if (vi_init_cam(VI_DEV_CAM1, VI_PIPE_CAM1, 0) < 0) return -1;

    return 0;
}

static void vi_deinit(void)
{
    RK_MPI_VI_DisableChn(VI_PIPE_CAM1, VI_CHN_DISP);
    RK_MPI_VI_DisableChn(VI_PIPE_CAM0, VI_CHN_DISP);
    RK_MPI_VI_DisableChn(VI_PIPE_CAM0, VI_CHN_IVS);
    RK_MPI_VI_DisableDev(VI_DEV_CAM1);
    RK_MPI_VI_DisableDev(VI_DEV_CAM0);
}

/* ========================================================================
 * VO init — GRAPHIC + RGA splice, 3 channels
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
    if (ret) { fprintf(stderr, "VO_SetLayerSpliceMode: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableLayer(VO_LAYER);
    if (ret) { fprintf(stderr, "VO_EnableLayer: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetLayerCSC(VO_LAYER, &VideoCSC);
    if (ret) fprintf(stderr, "VO_SetLayerCSC: %#x (non-fatal)\n", ret);

    /* ---- Chn 0: cam 0, full screen, priority=0, ROT90 ---- */
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

    /* ---- Chn 1: cam 1, PiP (1/4 screen), priority=1, ROT90, semi-transparent ---- */
    int pip_w = dispW / 2;   /* 360 */
    int pip_h = dispH / 2;   /* 640 */
    int pip_x = dispW - pip_w;  /* bottom-right */
    int pip_y = dispH - pip_h;
    memset(&VoChnAttr, 0, sizeof(VoChnAttr));
    VoChnAttr.bDeflicker = RK_FALSE;
    VoChnAttr.u32Priority = 1;
    VoChnAttr.stRect.s32X = pip_x;
    VoChnAttr.stRect.s32Y = pip_y;
    VoChnAttr.stRect.u32Width = pip_w;
    VoChnAttr.stRect.u32Height = pip_h;
    VoChnAttr.enRotation = ROTATION_90;
    VoChnAttr.u32FgAlpha = g_pip_alpha;  /* global alpha for PiP */
    VoChnAttr.u32BgAlpha = 0;
    ret = RK_MPI_VO_SetChnAttr(VO_LAYER, VO_CHN_CAM1, &VoChnAttr);
    if (ret) { fprintf(stderr, "VO_SetChnAttr[cam1]: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableChn(VO_LAYER, VO_CHN_CAM1);
    if (ret) { fprintf(stderr, "VO_EnableChn[cam1]: %#x\n", ret); return -1; }

    /* ---- Chn 2: UI overlay, full screen, priority=2, RGBA8888 via SendFrame ---- */
    memset(&VoChnAttr, 0, sizeof(VoChnAttr));
    VoChnAttr.bDeflicker = RK_FALSE;
    VoChnAttr.u32Priority = 2;
    VoChnAttr.stRect.s32X = 0;
    VoChnAttr.stRect.s32Y = 0;
    VoChnAttr.stRect.u32Width = dispW;
    VoChnAttr.stRect.u32Height = dispH;
    VoChnAttr.u32FgAlpha = 255;
    VoChnAttr.u32BgAlpha = 0;
    VoChnAttr.u32MaxChnQueue = 2;
    VoChnAttr.enRotation = ROTATION_0;  /* UI canvas already in display orientation */
    ret = RK_MPI_VO_SetChnAttr(VO_LAYER, VO_CHN_UI, &VoChnAttr);
    if (ret) { fprintf(stderr, "VO_SetChnAttr[ui]: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableChn(VO_LAYER, VO_CHN_UI);
    if (ret) { fprintf(stderr, "VO_EnableChn[ui]: %#x\n", ret); return -1; }

    /* ---- SYS_Bind VI → VO ---- */
    MPP_CHN_S vi_chn, vo_chn;

    /* cam 0 → VO chn 0 */
    vi_chn.enModId = RK_ID_VI;
    vi_chn.s32DevId = 0;
    vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO;
    vo_chn.s32DevId = VO_LAYER;
    vo_chn.s32ChnId = VO_CHN_CAM0;
    ret = RK_MPI_SYS_Bind(&vi_chn, &vo_chn);
    if (ret) { fprintf(stderr, "SYS_Bind VI0→VO0: %#x\n", ret); return -1; }

    /* cam 1 → VO chn 1 */
    vi_chn.enModId = RK_ID_VI;
    vi_chn.s32DevId = 1;
    vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO;
    vo_chn.s32DevId = VO_LAYER;
    vo_chn.s32ChnId = VO_CHN_CAM1;
    ret = RK_MPI_SYS_Bind(&vi_chn, &vo_chn);
    if (ret) { fprintf(stderr, "SYS_Bind VI1→VO1: %#x\n", ret); return -1; }

    printf("VO: %dx%d, layer=%d (GRAPHIC+RGA)\n", dispW, dispH, VO_LAYER);
    printf("  chn %d: cam0 full screen (priority=0, ROT90)\n", VO_CHN_CAM0);
    printf("  chn %d: cam1 PiP (%d,%d,%dx%d) (priority=1, ROT90, fgAlpha=%d)\n",
           VO_CHN_CAM1, pip_x, pip_y, pip_w, pip_h, g_pip_alpha);
    printf("  chn %d: UI green border (priority=2, SendFrame RGBA8888)\n", VO_CHN_UI);
    return 0;
}

static void vo_deinit(void)
{
    MPP_CHN_S vi_chn, vo_chn;

    /* Unbind cam 1 */
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 1; vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO; vo_chn.s32DevId = VO_LAYER; vo_chn.s32ChnId = VO_CHN_CAM1;
    RK_MPI_SYS_UnBind(&vi_chn, &vo_chn);

    /* Unbind cam 0 */
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 0; vi_chn.s32ChnId = VI_CHN_DISP;
    vo_chn.enModId = RK_ID_VO; vo_chn.s32DevId = VO_LAYER; vo_chn.s32ChnId = VO_CHN_CAM0;
    RK_MPI_SYS_UnBind(&vi_chn, &vo_chn);

    RK_MPI_VO_DisableChn(VO_LAYER, VO_CHN_UI);
    RK_MPI_VO_DisableChn(VO_LAYER, VO_CHN_CAM1);
    RK_MPI_VO_DisableChn(VO_LAYER, VO_CHN_CAM0);
    RK_MPI_VO_DisableLayer(VO_LAYER);
    RK_MPI_VO_Disable(VO_DEV);
    RK_MPI_VO_UnBindLayer(VO_LAYER, VO_DEV);
}

/* ========================================================================
 * IVS MD init — bind VI ext chn4 → IVS (cam 0 only)
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

    printf("IVS: MD on cam0 %dx%d, sensibility=3, threshSad=80, threshMove=2\n", IVS_W, IVS_H);
    return 0;
}

static void ivs_deinit(void)
{
    MPP_CHN_S vi_chn, ivs_chn;
    vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = 0; vi_chn.s32ChnId = VI_CHN_IVS;
    ivs_chn.enModId = RK_ID_IVS; ivs_chn.s32DevId = 0; ivs_chn.s32ChnId = IVS_CHN;
    RK_MPI_SYS_UnBind(&vi_chn, &ivs_chn);
    RK_MPI_IVS_DestroyChn(IVS_CHN);
}

/* ========================================================================
 * UI canvas — RGBA8888 green border on MMZ
 * ====================================================================== */
typedef struct {
    MB_BLK mblk;
    void *ptr;
    int width;
    int height;
    int size;
} ui_canvas_t;

static int ui_canvas_init(ui_canvas_t *c, int w, int h)
{
    c->width = w;
    c->height = h;
    c->size = w * h * 4;  /* RGBA8888 */
    int ret = RK_MPI_MMZ_Alloc(&c->mblk, c->size, 0);
    if (ret) { fprintf(stderr, "MMZ_Alloc: %#x\n", ret); return -1; }
    c->ptr = RK_MPI_MB_Handle2VirAddr(c->mblk);
    if (!c->ptr) { fprintf(stderr, "MMZ_Handle2VirAddr failed\n"); return -1; }
    memset(c->ptr, 0, c->size);  /* fully transparent */
    RK_MPI_SYS_MmzFlushCache(c->mblk, RK_FALSE);
    printf("UI canvas: %dx%d RGBA8888 (%d bytes)\n", w, h, c->size);
    return 0;
}

static void ui_canvas_deinit(ui_canvas_t *c)
{
    RK_MPI_MMZ_Free(c->mblk);
}

/* Draw green border (opaque) on transparent canvas */
static void ui_draw_green_border(ui_canvas_t *c, int thickness)
{
    uint8_t *p = (uint8_t *)c->ptr;
    int w = c->width;
    int h = c->height;

    /* Clear to transparent */
    memset(p, 0, c->size);

    /* Draw green border: top + bottom + left + right */
    /* RGBA = (R=0, G=255, B=0, A=255) */
    /* Top strip */
    for (int y = 0; y < thickness && y < h; y++) {
        uint8_t *row = p + (y * w) * 4;
        for (int x = 0; x < w; x++) {
            row[x*4+0] = 0;    /* B */
            row[x*4+1] = 255;  /* G */
            row[x*4+2] = 0;    /* R */
            row[x*4+3] = 255;  /* A */
        }
    }
    /* Bottom strip */
    for (int y = h - thickness; y < h; y++) {
        if (y < 0) continue;
        uint8_t *row = p + (y * w) * 4;
        for (int x = 0; x < w; x++) {
            row[x*4+0] = 0;
            row[x*4+1] = 255;
            row[x*4+2] = 0;
            row[x*4+3] = 255;
        }
    }
    /* Left strip */
    for (int y = 0; y < h; y++) {
        uint8_t *row = p + (y * w) * 4;
        for (int x = 0; x < thickness && x < w; x++) {
            row[x*4+0] = 0;
            row[x*4+1] = 255;
            row[x*4+2] = 0;
            row[x*4+3] = 255;
        }
    }
    /* Right strip */
    for (int y = 0; y < h; y++) {
        uint8_t *row = p + (y * w) * 4;
        for (int x = w - thickness; x < w; x++) {
            if (x < 0) continue;
            row[x*4+0] = 0;
            row[x*4+1] = 255;
            row[x*4+2] = 0;
            row[x*4+3] = 255;
        }
    }

    RK_MPI_SYS_MmzFlushCache(c->mblk, RK_FALSE);
}

/* Clear canvas to fully transparent */
static void ui_draw_transparent(ui_canvas_t *c)
{
    memset(c->ptr, 0, c->size);
    RK_MPI_SYS_MmzFlushCache(c->mblk, RK_FALSE);
}

static int ui_send(ui_canvas_t *c)
{
    VIDEO_FRAME_INFO_S stFrame;
    memset(&stFrame, 0, sizeof(stFrame));
    stFrame.stVFrame.u32Width = c->width;
    stFrame.stVFrame.u32Height = c->height;
    stFrame.stVFrame.u32VirWidth = c->width;
    stFrame.stVFrame.u32VirHeight = c->height;
    stFrame.stVFrame.enPixelFormat = RK_FMT_RGBA8888;
    stFrame.stVFrame.pMbBlk = c->mblk;
    return RK_MPI_VO_SendFrame(VO_LAYER, VO_CHN_UI, &stFrame, 1000);
}

/* ========================================================================
 * IVS results thread
 * ====================================================================== */
static void *ivs_results_thread(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "IvsMdResults", 0, 0, 0);

    int md_area_threshold = IVS_W * IVS_H * 1 / 100;
    double last_report = now_ms();
    int frame_count = 0;
    int motion_detected = 0;
    double last_motion_time = 0;
    double motion_timeout_ms = 3000;  /* green border stays 3s after last motion */

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
                motion_detected = 1;
                last_motion_time = now_ms();
            }

            RK_MPI_IVS_ReleaseResults(IVS_CHN, &stResults);
        } else {
            usleep(50000);
        }

        /* Check motion timeout */
        if (motion_detected && (now_ms() - last_motion_time > motion_timeout_ms)) {
            motion_detected = 0;
        }
        g_md_active = motion_detected;

        double now = now_ms();
        if (now - last_report > 5000) {
            printf("[IVS] %d frames, %d motion events, MD=%s\n",
                   frame_count, g_md_event_count, g_md_active ? "ACTIVE" : "idle");
            fflush(stdout);
            frame_count = 0;
            g_md_event_count = 0;
            last_report = now;
        }
    }
    return NULL;
}

/* ========================================================================
 * UI thread — updates green border based on MD state
 * ====================================================================== */
static void *ui_thread(void *arg)
{
    ui_canvas_t *canvas = (ui_canvas_t *)arg;
    prctl(PR_SET_NAME, "UiGreenBorder", 0, 0, 0);

    int last_md_state = -1;
    double last_send = 0;

    while (!g_exit) {
        int md = g_md_active;

        /* Only send when state changes, or periodically to keep VO fed */
        if (md != last_md_state) {
            if (md) {
                ui_draw_green_border(canvas, BORDER_THICKNESS);
            } else {
                ui_draw_transparent(canvas);
            }
            int ret = ui_send(canvas);
            if (ret) fprintf(stderr, "UI send: %#x\n", ret);
            last_md_state = md;
            last_send = now_ms();
            printf("[UI] MD=%s → %s\n", md ? "ACTIVE" : "idle",
                   md ? "green border" : "transparent");
            fflush(stdout);
        }

        /* Resend every 2s to keep VO buffer fresh */
        double now = now_ms();
        if (now - last_send > 2000) {
            ui_send(canvas);
            last_send = now;
        }

        usleep(100000);  /* 100ms poll */
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

    if (argc > 1) {
        g_pip_alpha = atoi(argv[1]);
        if (g_pip_alpha < 0) g_pip_alpha = 0;
        if (g_pip_alpha > 255) g_pip_alpha = 255;
    }

    printf("\n=== vi_vo_compose_alpha_test (2 cameras + green border + PiP alpha) ===\n");
    printf("    Cam0: GC2093 #1 → VI dev0/pipe0 → VO chn0 (full screen, ROT90)\n");
    printf("    Cam1: GC2093 #2 → VI dev1/pipe1 → VO chn1 (PiP 1/4, ROT90, fgAlpha=%d)\n", g_pip_alpha);
    printf("    UI:   RGBA8888 → VO chn2 (green border, priority=2)\n");
    printf("    IVS:  MD on cam0 → drives green border\n");
    printf("    VO layer 0: GRAPHIC + RGA splice\n\n");
    fflush(stdout);

    int ret = RK_MPI_SYS_Init();
    if (ret) { fprintf(stderr, "SYS_Init: %#x\n", ret); return 1; }

    ui_canvas_t canvas;

    if (vi_init() < 0) goto cleanup_sys;
    if (vo_init() < 0) goto cleanup_vi;
    if (ivs_init() < 0) goto cleanup_vo;
    if (ui_canvas_init(&canvas, DISP_W, DISP_H) < 0) goto cleanup_ivs;

    /* Send initial transparent canvas */
    ui_draw_transparent(&canvas);
    ui_send(&canvas);

    pthread_t ivs_tid, ui_tid;
    pthread_create(&ivs_tid, NULL, ivs_results_thread, NULL);
    pthread_create(&ui_tid, NULL, ui_thread, &canvas);

    printf("\n=== RUNNING (Ctrl-C to stop) ===\n");
    printf("    Video: cam0 full + cam1 PiP on display\n");
    printf("    Green border appears when MD detects motion\n");
    printf("    Run: modetest -M rockchip -w 59:zpos:0\n\n");
    fflush(stdout);

    double t0 = now_ms();
    while (!g_exit) {
        double elapsed = now_ms() - t0;
        if (elapsed > 300000) break;  /* 5 min */
        sleep(3);
        printf("[%.0fs] IVS frames=%d events=%d MD=%s\n",
               elapsed / 1000.0, g_md_frame_count, g_md_event_count,
               g_md_active ? "ACTIVE" : "idle");
        fflush(stdout);
    }

    g_exit = 1;
    pthread_join(ivs_tid, NULL);
    pthread_join(ui_tid, NULL);

    ui_canvas_deinit(&canvas);
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
