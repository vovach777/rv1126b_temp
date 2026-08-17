/* avs_mega_fanout_test.c — AVS-синхронизированный fan-out для RV1126B
 *
 * Архитектура (мега-кадр как источник истины):
 *
 *   VI0 (RGB) ─┐
 *              ├─→ AVS (bSyncPipe=1) → мега-кадр 1920×2160 (NOBLEND_VER)
 *   VI1 (IR)  ─┘                              │
 *                                              ├── RGA crop RGB half → DMA rgb_buf
 *                                              ├── RGA crop IR half  → DMA ir_buf
 *                                              └── RGA crop display_ch half + rot90 → VO
 *
 *   Главный поток: GetChnFrame(AVS) → RGA split → ReleaseChnFrame
 *   NN потоки: по таймеру берут latest DMA буфер (mutex), имитируют track()
 *   display_ch переключается раз в 2 секунды
 *
 *   Преимущество: AVS гарантирует что RGB и IR — один и тот же момент времени.
 *   LockedBuf не нужен — мега-кадр уже синхронизирован.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#include "rk_defines.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_comm_avs.h"
#include "rk_comm_vpss.h"
#include "rk_comm_vo.h"
#include "rk_comm_mb.h"
#include "rk_comm_video.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_avs.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_cal.h"

#include "im2d.h"
#include "rga.h"
#include "dma_alloc.h"

#define CAM_W           1920
#define CAM_H           1080
#define MEGA_W          CAM_W           /* 1920 */
#define MEGA_H          (CAM_H * 2)     /* 2160 (NOBLEND_VER: RGB top, IR bottom) */
#define DISP_W          720
#define DISP_H          1280
#define NUM_CAMS        2
#define AVS_GRP_ID      0
#define AVS_CHN_ID      0
#define VO_LAYER        0
#define VO_DEV          0
#define VO_CHN          0

static volatile int g_exit = 0;
static void sig_handler(int s) { (void)s; g_exit = 1; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ========================================================================
 * Latest-frame buffer для NN (double-buffer + mutex)
 * Главный поток пишет в back, NN читает front. Swap после записи.
 * ====================================================================== */
typedef struct {
    pthread_mutex_t mtx;
    int fd;             /* DMA buf fd */
    void *va;           /* virtual addr */
    int w, h;           /* dimensions */
    long long pts;      /* PTS of source frame */
    int ready;          /* 1 = есть свежий кадр */
    int frame_count;    /* сколько кадров записано */
} nn_buf_t;

static int nn_buf_init(nn_buf_t *b, int w, int h) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mtx, NULL);
    b->w = w;
    b->h = h;
    int size = w * h * 3 / 2;  /* NV12 */
    int rc = dma_buf_alloc(DMA_HEAP_UNCACHE_PATH, size, &b->fd, &b->va);
    if (rc < 0) {
        fprintf(stderr, "nn_buf_init: dma_buf_alloc failed\n");
        return -1;
    }
    return 0;
}

static void nn_buf_destroy(nn_buf_t *b) {
    if (b->fd >= 0) {
        int size = b->w * b->h * 3 / 2;
        dma_buf_free(size, &b->fd, b->va);
    }
    pthread_mutex_destroy(&b->mtx);
}

/* Главный поток: записать новый кадр в NN буфер (RGA copy из мега-кадра) */
static int nn_buf_write(nn_buf_t *b, MB_BLK mega_mb, int mega_w, int mega_h,
                        int crop_x, int crop_y, int crop_w, int crop_h) {
    int src_fd = RK_MPI_MB_Handle2Fd(mega_mb);
    rga_buffer_t src = wrapbuffer_fd_t(src_fd, mega_w, mega_h,
                                       mega_w, mega_h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_fd_t(b->fd, b->w, b->h,
                                       b->w, b->h, RK_FORMAT_YCbCr_420_SP);
    im_rect srect = { crop_x, crop_y, crop_w, crop_h };
    im_rect drect = { 0, 0, b->w, b->h };
    im_rect prect = {0};
    rga_buffer_t pat = {};
    IM_STATUS st = improcess(src, dst, pat, srect, drect, prect, IM_SYNC);
    if (st != IM_STATUS_SUCCESS) {
        fprintf(stderr, "nn_buf_write: improcess failed: %d\n", (int)st);
        return -1;
    }
    pthread_mutex_lock(&b->mtx);
    b->ready = 1;
    b->frame_count++;
    pthread_mutex_unlock(&b->mtx);
    return 0;
}

/* ========================================================================
 * NN consumer thread — читает latest буфер, имитирует track()
 * ====================================================================== */
typedef struct {
    nn_buf_t *buf;
    const char *name;
    volatile int running;
    int track_count;
    double last_report;
    int interval_ms;  /* как часто делать track (мс) */
} nn_ctx_t;

static void *nn_thread(void *arg)
{
    nn_ctx_t *ctx = (nn_ctx_t *)arg;
    ctx->track_count = 0;
    ctx->last_report = now_ms();
    int last_seen = -1;

    while (ctx->running && !g_exit) {
        /* Ждём interval_ms перед следующим track */
        usleep(ctx->interval_ms * 1000);

        pthread_mutex_lock(&ctx->buf->mtx);
        int ready = ctx->buf->ready;
        int fc = ctx->buf->frame_count;
        pthread_mutex_unlock(&ctx->buf->mtx);

        if (!ready) continue;
        if (fc == last_seen) continue;  /* уже обработали этот кадр */
        last_seen = fc;

        /* track() — заглушка: просто считаем */
        ctx->track_count++;

        double now = now_ms();
        if (now - ctx->last_report > 5000) {
            double elapsed = (now - ctx->last_report) / 1000.0;
            printf("[NN %s] %d tracks in %.1fs (%.1f/s), latest frame #%d\n",
                   ctx->name, ctx->track_count, elapsed,
                   ctx->track_count / elapsed, fc);
            ctx->track_count = 0;
            ctx->last_report = now;
        }
    }
    return NULL;
}

/* ========================================================================
 * VI init — обе камеры
 * ====================================================================== */
static int vi_init(void)
{
    for (int i = 0; i < NUM_CAMS; i++) {
        VI_DEV_ATTR_S stDevAttr;
        memset(&stDevAttr, 0, sizeof(stDevAttr));
        int ret = RK_MPI_VI_GetDevAttr(i, &stDevAttr);
        if (ret == RK_ERR_VI_NOT_CONFIG) {
            ret = RK_MPI_VI_SetDevAttr(i, &stDevAttr);
            if (ret) { fprintf(stderr, "VI_SetDevAttr[%d]: %#x\n", i, ret); return -1; }
        }
        ret = RK_MPI_VI_GetDevIsEnable(i);
        if (ret != RK_SUCCESS) {
            ret = RK_MPI_VI_EnableDev(i);
            if (ret) { fprintf(stderr, "VI_EnableDev[%d]: %#x\n", i, ret); return -1; }
            VI_DEV_BIND_PIPE_S stBindPipe;
            memset(&stBindPipe, 0, sizeof(stBindPipe));
            stBindPipe.u32Num = 1;
            stBindPipe.PipeId[0] = i;
            stBindPipe.bUserStartPipe[0] = RK_TRUE;
            ret = RK_MPI_VI_SetDevBindPipe(i, &stBindPipe);
            if (ret) { fprintf(stderr, "VI_SetDevBindPipe[%d]: %#x\n", i, ret); return -1; }
        }

        VI_CHN_ATTR_S chnAttr;
        memset(&chnAttr, 0, sizeof(chnAttr));
        chnAttr.stIspOpt.u32BufCount = 3;
        chnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
        chnAttr.stIspOpt.stMaxSize.u32Width = CAM_W;
        chnAttr.stIspOpt.stMaxSize.u32Height = CAM_H;
        chnAttr.stSize.u32Width = CAM_W;
        chnAttr.stSize.u32Height = CAM_H;
        chnAttr.enPixelFormat = RK_FMT_YUV420SP;
        chnAttr.enCompressMode = COMPRESS_MODE_NONE;
        chnAttr.u32Depth = 0;

        ret = RK_MPI_VI_SetChnAttr(i, 0, &chnAttr);
        if (ret) { fprintf(stderr, "VI_SetChnAttr[%d]: %#x\n", i, ret); return -1; }
        ret = RK_MPI_VI_EnableChnExt(i, 0);
        if (ret) { fprintf(stderr, "VI_EnableChnExt[%d]: %#x\n", i, ret); return -1; }
    }
    for (int i = 0; i < NUM_CAMS; i++) {
        int ret = RK_MPI_VI_StartPipe(i);
        if (ret) { fprintf(stderr, "VI_StartPipe[%d]: %#x\n", i, ret); return -1; }
    }
    printf("VI: 2 cameras OK (%dx%d NV12 each)\n", CAM_W, CAM_H);
    return 0;
}

static void vi_deinit(void)
{
    for (int i = 0; i < NUM_CAMS; i++) {
        RK_MPI_VI_DisableChn(i, 0);
        RK_MPI_VI_StopPipe(i);
        RK_MPI_VI_DisableDev(i);
    }
}

/* ========================================================================
 * AVS init — синхронизация двух камер → мега-кадр
 * ====================================================================== */
static int avs_init(void)
{
    AVS_MOD_PARAM_S stModParam;
    AVS_GRP_ATTR_S stGrpAttr;
    memset(&stModParam, 0, sizeof(stModParam));
    memset(&stGrpAttr, 0, sizeof(stGrpAttr));

    stModParam.u32WorkingSetSize = 0;
    stModParam.enMBSource = MB_SOURCE_PRIVATE;

    stGrpAttr.enMode = AVS_MODE_NOBLEND_VER;  /* RGB top, IR bottom */
    stGrpAttr.u32PipeNum = NUM_CAMS;
    stGrpAttr.bSyncPipe = 1;  /* синхронизация! */
    stGrpAttr.stGainAttr.enMode = AVS_GAIN_MODE_AUTO;
    stGrpAttr.stOutAttr.enPrjMode = AVS_PROJECTION_EQUIRECTANGULAR;
    stGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stGrpAttr.stInAttr.stSize.u32Width = CAM_W;
    stGrpAttr.stInAttr.stSize.u32Height = CAM_H;
    stGrpAttr.stOutAttr.fDistance = 5;
    stGrpAttr.stInAttr.enParamSource = AVS_PARAM_SOURCE_CALIB;
    stGrpAttr.stInAttr.stCalib.pCalibFilePath = "/tmp/avs-test/rk_2_1920x1080.json";
    stGrpAttr.jsonPath = "/tmp/avs-test/rk_2_1920x1080.json";

    int ret = RK_MPI_AVS_SetModParam(&stModParam);
    if (ret) { fprintf(stderr, "AVS_SetModParam: %#x\n", ret); return -1; }
    ret = RK_MPI_AVS_CreateGrp(AVS_GRP_ID, &stGrpAttr);
    if (ret) { fprintf(stderr, "AVS_CreateGrp: %#x\n", ret); return -1; }

    /* LDCH (Lens Distortion Correction) — нужно для AVS */
    {
        AVS_FINAL_LUT_S pstFinalLut;
        PIC_BUF_ATTR_S stBufAttr;
        MB_PIC_CAL_S pic_cal[NUM_CAMS];
        MB_EXT_CONFIG_S stMbExtConfig;
        void *ldch_data[NUM_CAMS];
        memset(&pstFinalLut, 0, sizeof(pstFinalLut));
        for (int i = 0; i < NUM_CAMS; i++) {
            memset(&stBufAttr, 0, sizeof(stBufAttr));
            memset(&pic_cal[i], 0, sizeof(pic_cal[i]));
            stBufAttr.u32Width = CAM_W;
            stBufAttr.u32Height = CAM_H;
            ret = RK_MPI_CAL_AVS_GetFinalLutBufferSize(&stBufAttr, &pic_cal[i]);
            if (ret || pic_cal[i].u32MBSize == 0) continue;
            ldch_data[i] = malloc(pic_cal[i].u32MBSize);
            memset(&stMbExtConfig, 0, sizeof(stMbExtConfig));
            stMbExtConfig.pu8VirAddr = (RK_U8 *)ldch_data[i];
            stMbExtConfig.u64Size = pic_cal[i].u32MBSize;
            ret = RK_MPI_SYS_CreateMB(&pstFinalLut.pLdchBlk[i], &stMbExtConfig);
            if (ret) { free(ldch_data[i]); continue; }
        }
        ret = RK_MPI_AVS_GetFinalLut(AVS_GRP_ID, &pstFinalLut);
        for (int i = 0; i < NUM_CAMS; i++) {
            if (pstFinalLut.pLdchBlk[i]) RK_MPI_SYS_Free(pstFinalLut.pLdchBlk[i]);
            if (ldch_data[i]) free(ldch_data[i]);
        }
    }

    /* AVS channel */
    AVS_CHN_ATTR_S stChnAttr;
    memset(&stChnAttr, 0, sizeof(stChnAttr));
    stChnAttr.u32Width = MEGA_W;
    stChnAttr.u32Height = MEGA_H;
    stChnAttr.enCompressMode = COMPRESS_MODE_NONE;
    stChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stChnAttr.u32Depth = 1;
    stChnAttr.u32FrameBufCnt = 3;
    stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    stChnAttr.stFrameRate.s32DstFrameRate = -1;

    ret = RK_MPI_AVS_SetChnAttr(AVS_GRP_ID, AVS_CHN_ID, &stChnAttr);
    if (ret) { fprintf(stderr, "AVS_SetChnAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_AVS_EnableChn(AVS_GRP_ID, AVS_CHN_ID);
    if (ret) { fprintf(stderr, "AVS_EnableChn: %#x\n", ret); return -1; }

    /* Bind VI → AVS */
    for (int i = 0; i < NUM_CAMS; i++) {
        MPP_CHN_S vi_chn, avs_in;
        vi_chn.enModId = RK_ID_VI;   vi_chn.s32DevId = i;  vi_chn.s32ChnId = 0;
        avs_in.enModId = RK_ID_AVS;  avs_in.s32DevId = AVS_GRP_ID;  avs_in.s32ChnId = i;
        ret = RK_MPI_SYS_Bind(&vi_chn, &avs_in);
        if (ret) { fprintf(stderr, "Bind VI%d→AVS: %#x\n", i, ret); return -1; }
    }

    ret = RK_MPI_AVS_StartGrp(AVS_GRP_ID);
    if (ret) { fprintf(stderr, "AVS_StartGrp: %#x\n", ret); return -1; }

    printf("AVS: grp=%d, mode=NOBLEND_VER, sync=1, mega=%dx%d\n",
           AVS_GRP_ID, MEGA_W, MEGA_H);
    return 0;
}

static void avs_deinit(void)
{
    for (int i = 0; i < NUM_CAMS; i++) {
        MPP_CHN_S vi_chn, avs_in;
        vi_chn.enModId = RK_ID_VI;   vi_chn.s32DevId = i;  vi_chn.s32ChnId = 0;
        avs_in.enModId = RK_ID_AVS;  avs_in.s32DevId = AVS_GRP_ID;  avs_in.s32ChnId = i;
        RK_MPI_SYS_UnBind(&vi_chn, &avs_in);
    }
    RK_MPI_AVS_DisableChn(AVS_GRP_ID, AVS_CHN_ID);
    RK_MPI_AVS_StopGrp(AVS_GRP_ID);
    RK_MPI_AVS_DestroyGrp(AVS_GRP_ID);
}

/* ========================================================================
 * VO init — layer 0, VIDEO mode, BGR888, RGA splice
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
    VoChnAttr.enRotation = ROTATION_0;
    ret = RK_MPI_VO_SetChnAttr(VO_LAYER, VO_CHN, &VoChnAttr);
    if (ret) { fprintf(stderr, "VO_SetChnAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableChn(VO_LAYER, VO_CHN);
    if (ret) { fprintf(stderr, "VO_EnableChn: %#x\n", ret); return -1; }

    printf("VO: %dx%d, layer=%d (VIDEO+RGA splice)\n", s_voDispW, s_voDispH, VO_LAYER);
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
 * RGA: crop display half from mega + rotate 90 + scale → MMZ → VO
 * ====================================================================== */
static MB_BLK s_mmz[2] = {NULL, NULL};
static int s_mmzIdx = 0;

static int rga_init(void)
{
    int mmz_size = s_voDispW * s_voDispH * 3;  /* BGR888 */
    for (int i = 0; i < 2; i++) {
        int ret = RK_MPI_MMZ_Alloc(&s_mmz[i], mmz_size, 0);
        if (ret) { fprintf(stderr, "MMZ_Alloc[%d]: %#x\n", i, ret); return -1; }
    }
    return 0;
}

static void rga_deinit(void)
{
    for (int i = 0; i < 2; i++)
        if (s_mmz[i]) RK_MPI_MMZ_Free(s_mmz[i]);
}

static int rga_display(MB_BLK mega_mb, int display_ch, MB_BLK dst_blk)
{
    int src_fd = RK_MPI_MB_Handle2Fd(mega_mb);
    int dst_fd = RK_MPI_MB_Handle2Fd(dst_blk);
    rga_buffer_t src = wrapbuffer_fd_t(src_fd, MEGA_W, MEGA_H,
                                       MEGA_W, MEGA_H, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_fd_t(dst_fd, s_voDispW, s_voDispH,
                                       s_voDispW, s_voDispH, RK_FORMAT_BGR_888);
    /* NOBLEND_VER: RGB = top half (y=0), IR = bottom half (y=CAM_H) */
    im_rect srect = {
        .x = 0,
        .y = (display_ch == 0) ? 0 : CAM_H,
        .width = CAM_W,
        .height = CAM_H
    };
    im_rect drect = { 0, 0, s_voDispW, s_voDispH };
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
int main(void)
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int ret = RK_MPI_SYS_Init();
    if (ret) { fprintf(stderr, "SYS_Init: %#x\n", ret); return 1; }

    if (vi_init() < 0) goto cleanup_sys;
    if (avs_init() < 0) goto cleanup_vi;
    if (vo_init() < 0) goto cleanup_avs;
    if (rga_init() < 0) goto cleanup_vo;

    /* NN буферы (NV12 CAM_W×CAM_H) — по одному на камеру */
    nn_buf_t nn_rgb_buf, nn_ir_buf;
    if (nn_buf_init(&nn_rgb_buf, CAM_W, CAM_H) < 0) goto cleanup_rga;
    if (nn_buf_init(&nn_ir_buf, CAM_W, CAM_H) < 0) goto cleanup_nn_rgb;

    /* NN потоки */
    nn_ctx_t nn_ctx[2];
    pthread_t nn_tid[2];
    nn_ctx[0].buf = &nn_rgb_buf;  nn_ctx[0].name = "RGB";  nn_ctx[0].running = 1;
    nn_ctx[0].interval_ms = 100;  /* ~10 track/sec */
    nn_ctx[1].buf = &nn_ir_buf;   nn_ctx[1].name = "IR";   nn_ctx[1].running = 1;
    nn_ctx[1].interval_ms = 100;
    pthread_create(&nn_tid[0], NULL, nn_thread, &nn_ctx[0]);
    pthread_create(&nn_tid[1], NULL, nn_thread, &nn_ctx[1]);

    printf("\n=== AVS mega fan-out test ===\n");
    printf("Mega: %dx%d (RGB top, IR bottom), sync=1\n", MEGA_W, MEGA_H);
    printf("NN: RGB + IR threads, track every 100ms\n");
    printf("Display: switch RGB↔IR every 2 sec\n\n");

    int display_ch = 0;
    double t0 = now_ms();
    double lastSwitch = t0;
    double lastReport = t0;
    int dispFrames = 0;
    int megaFrames = 0;
    int lastDispFrames = 0;

    while (!g_exit) {
        double elapsed = now_ms() - t0;
        if (elapsed > 60000) break;

        /* Get мега-кадра из AVS */
        VIDEO_FRAME_INFO_S mega;
        memset(&mega, 0, sizeof(mega));
        ret = RK_MPI_AVS_GetChnFrame(AVS_GRP_ID, AVS_CHN_ID, &mega, 1000);
        if (ret) {
            if (megaFrames < 5)
                fprintf(stderr, "AVS_GetChnFrame failed: %#x\n", ret);
            continue;
        }
        megaFrames++;
        long long pts = (long long)mega.stVFrame.u64PTS;

        /* RGA split: копируем RGB и IR половинки в NN буферы */
        nn_buf_write(&nn_rgb_buf, mega.stVFrame.pMbBlk,
                     MEGA_W, MEGA_H, 0, 0, CAM_W, CAM_H);       /* RGB = top */
        nn_buf_write(&nn_ir_buf, mega.stVFrame.pMbBlk,
                     MEGA_W, MEGA_H, 0, CAM_H, CAM_W, CAM_H);   /* IR = bottom */

        /* Display: crop выбранной половинки + rotate 90 → VO */
        double now = now_ms();
        if (now - lastSwitch > 2000) {
            display_ch ^= 1;
            lastSwitch = now;
            printf("[%.1fs] display_ch → %d (%s)\n", elapsed / 1000.0,
                   display_ch, display_ch == 0 ? "RGB" : "IR");
        }

        MB_BLK cur = s_mmz[s_mmzIdx];
        s_mmzIdx ^= 1;
        if (rga_display(mega.stVFrame.pMbBlk, display_ch, cur) == 0) {
            send_to_vo(cur, pts);
            dispFrames++;
        }

        RK_MPI_AVS_ReleaseChnFrame(AVS_GRP_ID, AVS_CHN_ID, &mega);

        /* FPS отчёт раз в 5 сек */
        if (now - lastReport > 5000) {
            double fps = (dispFrames - lastDispFrames) / ((now - lastReport) / 1000.0);
            printf("[%.1fs] MEGA: %d frames, DISP %s: %.1f fps\n",
                   elapsed / 1000.0, megaFrames,
                   display_ch == 0 ? "RGB" : "IR", fps);
            lastReport = now;
            lastDispFrames = dispFrames;
        }
    }

    double total = (now_ms() - t0) / 1000.0;
    printf("\n=== RESULT: %d mega frames, %d display frames in %.1fs ===\n",
           megaFrames, dispFrames, total);

    /* Stop NN threads */
    for (int i = 0; i < 2; i++) {
        nn_ctx[i].running = 0;
        pthread_join(nn_tid[i], NULL);
    }

    nn_buf_destroy(&nn_ir_buf);
cleanup_nn_rgb:
    nn_buf_destroy(&nn_rgb_buf);
cleanup_rga:
    rga_deinit();
cleanup_vo:
    vo_deinit();
cleanup_avs:
    avs_deinit();
cleanup_vi:
    vi_deinit();
cleanup_sys:
    RK_MPI_SYS_Exit();
    printf("cleanup done\n");
    return 0;
}
