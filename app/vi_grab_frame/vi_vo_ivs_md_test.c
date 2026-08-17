/* vi_vo_ivs_md_test.c — тест VI ext channels → IVS MD + RGA → VO для RV1126B
 *
 * Архитектура (по образцу rkipc/src/rv1126b_ipc/video.c):
 *
 *   GC2093 → VI pipe 0
 *             ├─ ext chn 4 → SYS_Bind → IVS MD (1920x1080, нативный кадр)
 *             └─ ext chn 5 → GetChnFrame → RGA (ROT90 + scale) → Rockit VO (OVERLAY)
 *
 *   VI_EXT_CHN_MODE + mirrorCmsc=0  → VI сам делает scaling (без VPSS)
 *
 *   Поток IVS: RK_MPI_IVS_GetResults → g_motion_detected
 *
 *   Main loop:
 *     если g_motion_detected → кадр с камеры → RGA → VO
 *     если !g_motion_detected → зелёный буфер → VO
 *
 * Запуск на factory:
 *   systemctl stop s30gui
 *   /tmp/vi_vo_ivs_md_test --time 0
 *   sleep 5 && modetest -M rockchip -w 59:zpos:0   # OVERLAY поверх PRIMARY
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
#include "im2d.h"
#include "rga.h"

/* ---- Размеры ---- */
#define CAM_W       1920
#define CAM_H       1080
#define DISP_W      720
#define DISP_H      1280
#define IVS_W       1920
#define IVS_H       1080

/* ---- VI (по образцу rkipc/video.c) ----
 * pipe 0, dev 0
 * chn 0 — physical (ISP main path) — не используется напрямую
 * ext chn 4 — для IVS MD (как g_vi_for_npu_ivs_id в rkipc)
 * ext chn 5 — для display (как g_vi_for_vo_chn_id в rkipc)
 */
#define VI_PIPE         0
#define VI_DEV          0
#define VI_CHN_PHY      0   /* physical chn (ISP) */
#define VI_CHN_IVS      4   /* ext chn для IVS MD */
#define VI_CHN_DISP     5   /* ext chn для display */

/* ---- VO ---- */
#define VO_LAYER    0
#define VO_DEV      0
#define VO_CHN      0

/* ---- IVS ---- */
#define IVS_CHN     0

/* ---- Порог MD (по умолчанию — высокая чувствительность, full frame) ---- */
static int   g_md_area_pct   = 1;    /* процент площади кадра (1% = ~20736 px) */
static int   g_md_sensibility = 3;   /* 1=low, 2=mid, 3=high */
static int   g_md_thresh_sad = 80;   /* [0,4095] — порог разности пикселей */
static int   g_md_thresh_move = 2;   /* [0,4] — порог движения */
static int   g_md_timeout_ms = 3000; /* нет движения 3 сек → зелёный */

/* ---- Зелёный цвет (BGR888) ---- */
#define GREEN_R 0
#define GREEN_G 255
#define GREEN_B 0

static volatile int g_exit = 0;
static void sig_handler(int s) { (void)s; g_exit = 1; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ========================================================================
 * Состояние MD
 * ====================================================================== */
static volatile int g_motion_detected = 0;
static volatile double g_last_motion_time = 0;
static volatile int g_md_frame_count = 0;
static volatile int g_md_event_count = 0;

/* ========================================================================
 * VI init — по образцу rkipc_vi_dev_init() + rkipc_vi_ext_init()
 * Без VPSS, без StartPipe, без bUserStartPipe.
 * VI_EXT_CHN_MODE + mirrorCmsc=0 → VI сам делает scaling.
 * ====================================================================== */
static int vi_init(void)
{
    int ret = 0;

    /* 1. Dev + Pipe (как rkipc_vi_dev_init) */
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
        /* Bind dev→pipe (как в rkipc: u32Num = pipe_id_, без bUserStartPipe) */
        VI_DEV_BIND_PIPE_S stBindPipe;
        memset(&stBindPipe, 0, sizeof(stBindPipe));
        stBindPipe.u32Num = VI_PIPE;
        stBindPipe.PipeId[0] = VI_PIPE;
        ret = RK_MPI_VI_SetDevBindPipe(VI_DEV, &stBindPipe);
        if (ret) { fprintf(stderr, "VI_SetDevBindPipe: %#x\n", ret); return -1; }
    }

    /* 2. VI_EXT_CHN_MODE + mirrorCmsc=0 (как rkipc_vi_dev_init, строки 406-430) */
    VI_PARAM_MOD_S stModParam;
    memset(&stModParam, 0, sizeof(stModParam));
    stModParam.enViModType = VI_EXT_CHN_MODE;
    stModParam.stExtChnParam.mirrorCmsc = 0;  /* 0 = VI сам scaling, 1 = VPSS */
    stModParam.stExtChnParam.extChn[0] = 0;
    stModParam.stExtChnParam.extChn[1] = 0;
    stModParam.stExtChnParam.extChn[2] = 0;
    stModParam.stExtChnParam.extChn[3] = 0;
    ret = RK_MPI_VI_SetModParam(&stModParam);
    if (ret) fprintf(stderr, "VI_SetModParam: %#x (non-fatal)\n", ret);

    /* 3. Ext chn 4 — для IVS MD (как g_vi_for_npu_ivs_id, строки 783-808)
     * Physical chn 0 НЕ включается в non-FEC mode (rkipc_vi_ext_init) */
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

    /* 5. Ext chn 5 — для display, читается через GetChnFrame (не Bind!)
     * В rkipc chn5 (g_vi_for_vo_chn_id) идёт через SYS_Bind → VO, depth=0.
     * У нас chn5 читается руками → depth=1 (как enable_npu для chn4 в rkipc). */
    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.stIspOpt.u32BufCount = 3;
    chnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chnAttr.stIspOpt.stMaxSize.u32Width = CAM_W;
    chnAttr.stIspOpt.stMaxSize.u32Height = CAM_H;
    chnAttr.stSize.u32Width = CAM_W;
    chnAttr.stSize.u32Height = CAM_H;
    chnAttr.enPixelFormat = RK_FMT_YUV420SP;
    chnAttr.enCompressMode = COMPRESS_MODE_NONE;
    chnAttr.u32Depth = 1;  /* GetChnFrame consumer → depth=1 */
    ret = RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN_DISP, &chnAttr);
    if (ret) { fprintf(stderr, "VI_SetChnAttr[DISP]: %#x\n", ret); return -1; }
    ret = RK_MPI_VI_EnableChn(VI_PIPE, VI_CHN_DISP);
    if (ret) { fprintf(stderr, "VI_EnableChn[DISP]: %#x\n", ret); return -1; }

    printf("VI: pipe %d, ext chn %d (IVS %dx%d) + ext chn %d (DISP %dx%d)\n",
           VI_PIPE, VI_CHN_IVS, IVS_W, IVS_H, VI_CHN_DISP, CAM_W, CAM_H);
    return 0;
}

static void vi_deinit(void)
{
    /* По образцу rkipc_vi_ext_deinit (строки 829-850) */
    RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN_DISP);
    RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN_IVS);
    /* По образцу rkipc_vi_dev_deinit (строки 435-439) */
    RK_MPI_VI_DisableDev(VI_DEV);
}

/* ========================================================================
 * VO init — layer 0, VIDEO mode, BGR888, RGA splice, ROTATION_0
 * (rotate делает RGA, не VO — как в percomedia_stub)
 * ====================================================================== */
static int s_voDispW = DISP_W;
static int s_voDispH = DISP_H;

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

    int ret = RK_MPI_VO_BindLayer(VO_LAYER, VO_DEV, VO_LAYER_MODE_VIDEO);
    if (ret) { fprintf(stderr, "VO_BindLayer: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetPubAttr(VO_DEV, &VoPubAttr);
    if (ret) { fprintf(stderr, "VO_SetPubAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_Enable(VO_DEV);
    if (ret) { fprintf(stderr, "VO_Enable: %#x\n", ret); return -1; }

    ret = RK_MPI_VO_GetPubAttr(VO_DEV, &VoPubAttr);
    s_voDispW = VoPubAttr.stSyncInfo.u16Hact;
    s_voDispH = VoPubAttr.stSyncInfo.u16Vact;
    if (!s_voDispW) { s_voDispW = DISP_W; s_voDispH = DISP_H; }

    stLayerAttr.stDispRect.s32X = 0;
    stLayerAttr.stDispRect.s32Y = 0;
    stLayerAttr.stDispRect.u32Width = s_voDispW;
    stLayerAttr.stDispRect.u32Height = s_voDispH;
    stLayerAttr.stImageSize.u32Width = s_voDispW;
    stLayerAttr.stImageSize.u32Height = s_voDispH;
    stLayerAttr.u32DispFrmRt = 30;
    stLayerAttr.enPixFormat = RK_FMT_BGR888;
    stLayerAttr.bBypassFrame = RK_FALSE;
    VideoCSC.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
    VideoCSC.u32Contrast = 50;
    VideoCSC.u32Hue = 50;
    VideoCSC.u32Luma = 50;
    VideoCSC.u32Satuature = 50;

    ret = RK_MPI_VO_SetLayerAttr(VO_LAYER, &stLayerAttr);
    if (ret) { fprintf(stderr, "VO_SetLayerAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetLayerSpliceMode(VO_LAYER, VO_SPLICE_MODE_RGA);
    if (ret) { fprintf(stderr, "VO_SetLayerSpliceMode: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableLayer(VO_LAYER);
    if (ret) { fprintf(stderr, "VO_EnableLayer: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetLayerCSC(VO_LAYER, &VideoCSC);
    if (ret) { fprintf(stderr, "VO_SetLayerCSC: %#x\n", ret); return -1; }

    VoChnAttr.stRect.s32X = 0;
    VoChnAttr.stRect.s32Y = 0;
    VoChnAttr.stRect.u32Width = s_voDispW;
    VoChnAttr.stRect.u32Height = s_voDispH;
    VoChnAttr.enRotation = ROTATION_0;  /* rotate делает RGA, не VO */
    ret = RK_MPI_VO_SetChnAttr(VO_LAYER, VO_CHN, &VoChnAttr);
    if (ret) { fprintf(stderr, "VO_SetChnAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableChn(VO_LAYER, VO_CHN);
    if (ret) { fprintf(stderr, "VO_EnableChn: %#x\n", ret); return -1; }

    printf("VO: %dx%d, layer=%d (VIDEO+RGA splice, ROTATION_0)\n",
           s_voDispW, s_voDispH, VO_LAYER);
    return 0;
}

static void vo_deinit(void)
{
    RK_MPI_VO_DisableChn(VO_LAYER, VO_CHN);
    RK_MPI_VO_DisableLayer(VO_LAYER);
    RK_MPI_VO_UnBindLayer(VO_LAYER, VO_DEV);
    RK_MPI_VO_Disable(VO_DEV);
}

/* ========================================================================
 * IVS MD init — motion detection, bind VI ext chn → IVS (напрямую, без VPSS)
 * По образцу rkipc_pipe_npu_ivs_init (строки 1763-1790)
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
    attr.u32MDSensibility = g_md_sensibility;
    attr.bODEnable = RK_FALSE;
    attr.s32ODInterval = 1;
    attr.s32ODPercent = 6;

    int ret = RK_MPI_IVS_CreateChn(IVS_CHN, &attr);
    if (ret) { fprintf(stderr, "IVS_CreateChn: %#x\n", ret); return -1; }

    IVS_MD_ATTR_S stMdAttr;
    memset(&stMdAttr, 0, sizeof(stMdAttr));
    ret = RK_MPI_IVS_GetMdAttr(IVS_CHN, &stMdAttr);
    if (ret) { fprintf(stderr, "IVS_GetMdAttr: %#x\n", ret); return -1; }
    stMdAttr.s32ThreshSad = g_md_thresh_sad;
    stMdAttr.s32ThreshMove = g_md_thresh_move;
    stMdAttr.s32SwitchSad = 0;
    stMdAttr.bFlycatkinFlt = RK_TRUE;
    stMdAttr.s32ThresDustMove = 3;
    stMdAttr.s32ThresDustBlk = 3;
    stMdAttr.s32ThresDustChng = 50;
    ret = RK_MPI_IVS_SetMdAttr(IVS_CHN, &stMdAttr);
    if (ret) { fprintf(stderr, "IVS_SetMdAttr: %#x\n", ret); return -1; }

    /* Bind VI ext chn → IVS (как rkipc, строки 1771-1780) */
    MPP_CHN_S vi_chn, ivs_chn;
    vi_chn.enModId = RK_ID_VI;
    vi_chn.s32DevId = 0;
    vi_chn.s32ChnId = VI_CHN_IVS;
    ivs_chn.enModId = RK_ID_IVS;
    ivs_chn.s32DevId = 0;
    ivs_chn.s32ChnId = IVS_CHN;
    ret = RK_MPI_SYS_Bind(&vi_chn, &ivs_chn);
    if (ret) { fprintf(stderr, "SYS_Bind VI→IVS: %#x\n", ret); return -1; }

    printf("IVS: MD on %dx%d, sensibility=%d, threshSad=%d, threshMove=%d\n",
           IVS_W, IVS_H, g_md_sensibility, g_md_thresh_sad, g_md_thresh_move);
    return 0;
}

static void ivs_deinit(void)
{
    /* По образцу rkipc (строки 1801-1808) */
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
 * IVS results thread — читает MD результаты
 * ====================================================================== */
static void *ivs_results_thread(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "IvsMdResults", 0, 0, 0);

    int md_area_threshold = IVS_W * IVS_H * g_md_area_pct / 100;
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
                g_motion_detected = 1;
                g_last_motion_time = now_ms();
                g_md_event_count++;
            }

            RK_MPI_IVS_ReleaseResults(IVS_CHN, &stResults);
        } else {
            usleep(50000);
        }

        double now = now_ms();
        if (now - last_report > 5000) {
            printf("[IVS] %d frames, %d motion events, area_threshold=%d (=%d%%)\n",
                   frame_count, g_md_event_count, md_area_threshold, g_md_area_pct);
            fflush(stdout);
            frame_count = 0;
            g_md_event_count = 0;
            last_report = now;
        }
    }
    return NULL;
}

/* ========================================================================
 * RGA: NV12 → BGR888 + rotate 90 + scale → display
 * ====================================================================== */
static MB_BLK s_mmz[2] = {NULL, NULL};
static MB_BLK s_green_mmz = NULL;
static int s_mmzIdx = 0;

static int mmz_init(void)
{
    int mmz_size = s_voDispW * s_voDispH * 3;
    for (int i = 0; i < 2; i++) {
        int ret = RK_MPI_MMZ_Alloc(&s_mmz[i], mmz_size, 0);
        if (ret) { fprintf(stderr, "MMZ_Alloc[%d]: %#x\n", i, ret); return -1; }
    }
    int ret = RK_MPI_MMZ_Alloc(&s_green_mmz, mmz_size, 0);
    if (ret) { fprintf(stderr, "MMZ_Alloc[green]: %#x\n", ret); return -1; }

    /* Заполняем зелёным через RGA imfill */
    int green_fd = RK_MPI_MB_Handle2Fd(s_green_mmz);
    rga_buffer_t dst = wrapbuffer_fd_t(green_fd, s_voDispW, s_voDispH,
                                        s_voDispW, s_voDispH, RK_FORMAT_BGR_888);
    im_rect drect = {0, 0, s_voDispW, s_voDispH};
    IM_STATUS st = imfill(dst, drect, 0x00FF00);
    if (st != IM_STATUS_SUCCESS) {
        fprintf(stderr, "imfill green: %d (non-fatal, trying manual)\n", st);
        void *ptr = RK_MPI_MB_Handle2VirAddr(s_green_mmz);
        if (ptr) {
            unsigned char *p = (unsigned char *)ptr;
            for (int i = 0; i < s_voDispW * s_voDispH; i++) {
                p[i * 3 + 0] = GREEN_B;
                p[i * 3 + 1] = GREEN_G;
                p[i * 3 + 2] = GREEN_R;
            }
        }
    }
    printf("MMZ: 2 camera buffers + 1 green buffer (%dx%d BGR888)\n",
           s_voDispW, s_voDispH);
    return 0;
}

static void mmz_deinit(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_mmz[i]) RK_MPI_MMZ_Free(s_mmz[i]);
    }
    if (s_green_mmz) RK_MPI_MMZ_Free(s_green_mmz);
}

static int rga_process_camera(VIDEO_FRAME_INFO_S *vf, MB_BLK dst_blk)
{
    int w = vf->stVFrame.u32Width;
    int h = vf->stVFrame.u32Height;
    int vw = vf->stVFrame.u32VirWidth;
    int vh = vf->stVFrame.u32VirHeight;

    int src_fd = RK_MPI_MB_Handle2Fd(vf->stVFrame.pMbBlk);
    int dst_fd = RK_MPI_MB_Handle2Fd(dst_blk);
    rga_buffer_t src = wrapbuffer_fd_t(src_fd, w, h, vw, vh, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_fd_t(dst_fd, s_voDispW, s_voDispH,
                                        s_voDispW, s_voDispH, RK_FORMAT_BGR_888);
    im_rect srect = {0, 0, w, h};
    im_rect drect = {0, 0, s_voDispW, s_voDispH};
    im_rect prect = {0};
    rga_buffer_t pat = {};
    int usage = IM_HAL_TRANSFORM_ROT_90 | IM_SYNC;
    IM_STATUS st = improcess(src, dst, pat, srect, drect, prect, usage);
    return (st == IM_STATUS_SUCCESS) ? 0 : -1;
}

static void send_to_vo(MB_BLK blk, long long pts)
{
    VIDEO_FRAME_INFO_S vof;
    memset(&vof, 0, sizeof(vof));
    vof.stVFrame.pMbBlk = blk;
    vof.stVFrame.u32Width = s_voDispW;
    vof.stVFrame.u32Height = s_voDispH;
    vof.stVFrame.u32VirWidth = s_voDispW;
    vof.stVFrame.u32VirHeight = s_voDispH;
    vof.stVFrame.enPixelFormat = RK_FMT_BGR888;
    vof.stVFrame.u64PTS = pts;
    RK_MPI_VO_SendFrame(VO_LAYER, VO_CHN, &vof, 1000);
}

/* ========================================================================
 * Main
 * ====================================================================== */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --area N       area threshold %% (default: 1, range 1-100)\n"
        "  --sens N       sensibility 1=low 2=mid 3=high (default: 3)\n"
        "  --sad N        SAD threshold 0-4095 (default: 80)\n"
        "  --move N       move threshold 0-4 (default: 2)\n"
        "  --timeout N    no-motion timeout ms (default: 3000)\n"
        "  --time N       test duration sec (default: 120, 0=forever)\n"
        "  -h, --help     this help\n"
        "\n"
        "Examples:\n"
        "  %s                       # high sensitivity (default)\n"
        "  %s --area 10 --sens 1    # very low: 10%% area, low sensibility\n"
        "  %s --area 1 --sens 3     # high: 1%% area, high sensibility\n"
        "  %s --time 0              # run forever\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int test_duration = 120;  /* seconds, 0 = forever */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (i + 1 < argc && !strcmp(argv[i], "--area")) {
            g_md_area_pct = atoi(argv[++i]);
        } else if (i + 1 < argc && !strcmp(argv[i], "--sens")) {
            g_md_sensibility = atoi(argv[++i]);
        } else if (i + 1 < argc && !strcmp(argv[i], "--sad")) {
            g_md_thresh_sad = atoi(argv[++i]);
        } else if (i + 1 < argc && !strcmp(argv[i], "--move")) {
            g_md_thresh_move = atoi(argv[++i]);
        } else if (i + 1 < argc && !strcmp(argv[i], "--timeout")) {
            g_md_timeout_ms = atoi(argv[++i]);
        } else if (i + 1 < argc && !strcmp(argv[i], "--time")) {
            test_duration = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* Clamp values */
    if (g_md_area_pct < 1) g_md_area_pct = 1;
    if (g_md_area_pct > 100) g_md_area_pct = 100;
    if (g_md_sensibility < 1) g_md_sensibility = 1;
    if (g_md_sensibility > 3) g_md_sensibility = 3;
    if (g_md_thresh_sad < 0) g_md_thresh_sad = 0;
    if (g_md_thresh_sad > 4095) g_md_thresh_sad = 4095;
    if (g_md_thresh_move < 0) g_md_thresh_move = 0;
    if (g_md_thresh_move > 4) g_md_thresh_move = 4;

    printf("\n=== vi_vo_ivs_md_test (VI ext channels, no VPSS) ===\n");
    printf("    VI pipe %d (GC2093 %dx%d)\n", VI_PIPE, CAM_W, CAM_H);
    printf("      → ext chn %d → IVS MD (%dx%d, нативный кадр)\n",
           VI_CHN_IVS, IVS_W, IVS_H);
    printf("      → ext chn %d → RGA ROT90 → VO (%dx%d MIPI OVERLAY)\n",
           VI_CHN_DISP, DISP_W, DISP_H);
    printf("    MD: area>=%d%%, sens=%d, sad=%d, move=%d, timeout=%dms\n",
           g_md_area_pct, g_md_sensibility, g_md_thresh_sad,
           g_md_thresh_move, g_md_timeout_ms);
    printf("    Motion → camera, else GREEN screen\n");
    printf("    Duration: %s\n\n", test_duration > 0 ? "120 sec" : "forever");
    fflush(stdout);

    int ret = RK_MPI_SYS_Init();
    if (ret) { fprintf(stderr, "SYS_Init: %#x\n", ret); return 1; }

    if (vi_init() < 0) goto cleanup_sys;
    if (vo_init() < 0) goto cleanup_vi;
    if (ivs_init() < 0) goto cleanup_vo;
    if (mmz_init() < 0) goto cleanup_ivs;

    /* IVS results thread */
    pthread_t ivs_tid;
    pthread_create(&ivs_tid, NULL, ivs_results_thread, NULL);

    printf("\n=== RUNNING (Ctrl-C to stop) ===\n\n"); fflush(stdout);

    double t0 = now_ms();
    double last_report = t0;
    int cam_frames = 0;
    int green_frames = 0;
    int last_cam = 0;
    int last_green = 0;

    while (!g_exit) {
        double elapsed = now_ms() - t0;
        if (test_duration > 0 && elapsed > test_duration * 1000) break;

        double now = now_ms();
        if (g_motion_detected && (now - g_last_motion_time > g_md_timeout_ms)) {
            g_motion_detected = 0;
        }

        /* Читаем кадр из VI ext chn 5 (display) */
        VIDEO_FRAME_INFO_S vf;
        memset(&vf, 0, sizeof(vf));
        ret = RK_MPI_VI_GetChnFrame(VI_PIPE, VI_CHN_DISP, &vf, 1000);
        if (ret) {
            if (cam_frames + green_frames < 5)
                fprintf(stderr, "VI_GetChnFrame[DISP]: %#x\n", ret);
            continue;
        }

        if (g_motion_detected) {
            /* Движение есть → кадр с камеры → RGA ROT90 → VO */
            MB_BLK cur = s_mmz[s_mmzIdx];
            s_mmzIdx ^= 1;
            if (rga_process_camera(&vf, cur) == 0) {
                send_to_vo(cur, (long long)vf.stVFrame.u64PTS);
                cam_frames++;
            }
        } else {
            /* Нет движения → зелёный экран */
            send_to_vo(s_green_mmz, (long long)vf.stVFrame.u64PTS);
            green_frames++;
        }

        RK_MPI_VI_ReleaseChnFrame(VI_PIPE, VI_CHN_DISP, &vf);

        if (now - last_report > 3000) {
            int total = cam_frames + green_frames;
            double fps = (total - last_cam - last_green) / ((now - last_report) / 1000.0);
            printf("[%.1fs] %s | cam=%d green=%d (total=%d, %.1ffps) | MD: %s\n",
                   elapsed / 1000.0,
                   g_motion_detected ? "MOTION" : "GREEN ",
                   cam_frames - last_cam, green_frames - last_green,
                   total, fps,
                   g_motion_detected ? "DETECTED" : "idle");
            fflush(stdout);
            last_report = now;
            last_cam = cam_frames;
            last_green = green_frames;
        }
    }

    double total = (now_ms() - t0) / 1000.0;
    if (total > 0 && (cam_frames + green_frames) > 0) {
        printf("\n=== RESULT: %d cam + %d green = %d frames in %.1fs (%.1f fps) ===\n",
               cam_frames, green_frames, cam_frames + green_frames, total,
               (cam_frames + green_frames) / total);
    }

    g_exit = 1;
    pthread_join(ivs_tid, NULL);

cleanup_mmz:
    mmz_deinit();
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
