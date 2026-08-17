/* vpss_fanout_test.c — тест VPSS fan-out для двух камер на RV1126B
 *
 * Архитектура (как в старом percomedia, но VPSS вместо RGA fan-out):
 *
 *   VI0 (RGB) → VPSS grp0
 *                 ├── CH_NN   (depth=1) → NN consumer thread (track)
 *                 └── CH_DISP (depth=2) → display если display_ch==0
 *
 *   VI1 (IR)  → VPSS grp1
 *                 ├── CH_NN   (depth=1) → NN consumer thread (track)
 *                 └── CH_DISP (depth=2) → display если display_ch==1
 *
 *   display_ch переключается раз в 5 секунд (0→1→0→...)
 *   Непрочитанный display channel не душит NN channel (проверено).
 *
 *   RGA: NV12 → BGR888 + rotate 90 + scale → VO (overlay plane)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_comm_vpss.h"
#include "rk_comm_vo.h"
#include "im2d.h"
#include "rga.h"

#define CAM_W       1920
#define CAM_H       1080
#define DISP_W      720
#define DISP_H      1280
#define NUM_CAMS    2

/* VPSS group IDs — по одной на камеру */
#define VPSS_GRP_RGB    2
#define VPSS_GRP_IR     3

/* Channel IDs внутри каждой group */
#define CH_NN       0   /* для нейронки (depth=1, свежий кадр) */
#define CH_DISP     1   /* для дисплея (depth=2) */

/* VI: dev=i, pipe=i, chn=i (как в percomedia_stub) */
#define VO_LAYER    0
#define VO_DEV      0
#define VO_CHN      0

static volatile int g_exit = 0;
static void sig_handler(int s) { (void)s; g_exit = 1; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ========================================================================
 * NN consumer thread — читает CH_NN из VPSS group, имитирует track()
 * ====================================================================== */
typedef struct {
    int grp;
    int cam_id;
    const char *name;
    volatile int running;
    int frame_count;
    double last_report;
} nn_ctx_t;

static void *nn_thread(void *arg)
{
    nn_ctx_t *ctx = (nn_ctx_t *)arg;
    ctx->frame_count = 0;
    ctx->last_report = now_ms();

    while (ctx->running && !g_exit) {
        VIDEO_FRAME_INFO_S vf;
        memset(&vf, 0, sizeof(vf));
        int ret = RK_MPI_VPSS_GetChnFrame(ctx->grp, CH_NN, &vf, 1000);
        if (ret) {
            if (ctx->frame_count == 0)
                fprintf(stderr, "[NN %s] GetChnFrame failed: %#x\n", ctx->name, ret);
            continue;
        }

        /* track() — заглушка: просто считаем кадры */
        ctx->frame_count++;

        /* Отчёт раз в 5 сек */
        double now = now_ms();
        if (now - ctx->last_report > 5000) {
            double fps = ctx->frame_count / ((now - ctx->last_report) / 1000.0);
            printf("[NN %s] %d frames, %.1f fps\n", ctx->name, ctx->frame_count, fps);
            ctx->frame_count = 0;
            ctx->last_report = now;
        }

        RK_MPI_VPSS_ReleaseChnFrame(ctx->grp, CH_NN, &vf);
    }
    return NULL;
}

/* ========================================================================
 * VI init — обе камеры (dev=i, pipe=i, chn=i)
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

        ret = RK_MPI_VI_SetChnAttr(i, i, &chnAttr);
        if (ret) { fprintf(stderr, "VI_SetChnAttr[%d]: %#x\n", i, ret); return -1; }
        ret = RK_MPI_VI_EnableChnExt(i, i);
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
        RK_MPI_VI_DisableChn(i, i);
        RK_MPI_VI_StopPipe(i);
        RK_MPI_VI_DisableDev(i);
    }
}

/* ========================================================================
 * VPSS init — по одной group на камеру, 2 channel каждая (NN + DISP)
 * ====================================================================== */
static int vpss_init(void)
{
    int grps[NUM_CAMS] = { VPSS_GRP_RGB, VPSS_GRP_IR };

    for (int i = 0; i < NUM_CAMS; i++) {
        int grp = grps[i];

        /* Group */
        VPSS_GRP_ATTR_S gattr;
        memset(&gattr, 0, sizeof(gattr));
        gattr.u32MaxW = CAM_W;
        gattr.u32MaxH = CAM_H;
        gattr.enPixelFormat = RK_FMT_YUV420SP;
        gattr.stFrameRate.s32SrcFrameRate = -1;
        gattr.stFrameRate.s32DstFrameRate = -1;
        gattr.enCompressMode = COMPRESS_MODE_NONE;
        gattr.enVProcDev = VIDEO_PROC_DEV_VPSS;

        int ret = RK_MPI_VPSS_CreateGrp(grp, &gattr);
        if (ret) { fprintf(stderr, "VPSS_CreateGrp[%d]: %#x\n", grp, ret); return -1; }

        /* CH_NN — для нейронки (depth=1, свежий кадр) */
        VPSS_CHN_ATTR_S cattr_nn;
        memset(&cattr_nn, 0, sizeof(cattr_nn));
        cattr_nn.enChnMode = VPSS_CHN_MODE_USER;
        cattr_nn.enDynamicRange = DYNAMIC_RANGE_SDR8;
        cattr_nn.enPixelFormat = RK_FMT_YUV420SP;
        cattr_nn.stFrameRate.s32SrcFrameRate = -1;
        cattr_nn.stFrameRate.s32DstFrameRate = -1;
        cattr_nn.u32Width = CAM_W;
        cattr_nn.u32Height = CAM_H;
        cattr_nn.stMaxSize.u32Width = CAM_W;
        cattr_nn.stMaxSize.u32Height = CAM_H;
        cattr_nn.enCompressMode = COMPRESS_MODE_NONE;
        cattr_nn.u32FrameBufCnt = 64;
        cattr_nn.u32Depth = 1;

        ret = RK_MPI_VPSS_SetChnAttr(grp, CH_NN, &cattr_nn);
        if (ret) { fprintf(stderr, "VPSS_SetChnAttr[%d,NN]: %#x\n", grp, ret); return -1; }
        ret = RK_MPI_VPSS_EnableChn(grp, CH_NN);
        if (ret) { fprintf(stderr, "VPSS_EnableChn[%d,NN]: %#x\n", grp, ret); return -1; }

        /* CH_DISP — для дисплея (depth=2) */
        VPSS_CHN_ATTR_S cattr_disp;
        memset(&cattr_disp, 0, sizeof(cattr_disp));
        cattr_disp.enChnMode = VPSS_CHN_MODE_USER;
        cattr_disp.enDynamicRange = DYNAMIC_RANGE_SDR8;
        cattr_disp.enPixelFormat = RK_FMT_YUV420SP;
        cattr_disp.stFrameRate.s32SrcFrameRate = -1;
        cattr_disp.stFrameRate.s32DstFrameRate = -1;
        cattr_disp.u32Width = CAM_W;
        cattr_disp.u32Height = CAM_H;
        cattr_disp.stMaxSize.u32Width = CAM_W;
        cattr_disp.stMaxSize.u32Height = CAM_H;
        cattr_disp.enCompressMode = COMPRESS_MODE_NONE;
        cattr_disp.u32FrameBufCnt = 64;
        cattr_disp.u32Depth = 1;

        ret = RK_MPI_VPSS_SetChnAttr(grp, CH_DISP, &cattr_disp);
        if (ret) { fprintf(stderr, "VPSS_SetChnAttr[%d,DISP]: %#x\n", grp, ret); return -1; }
        ret = RK_MPI_VPSS_EnableChn(grp, CH_DISP);
        if (ret) { fprintf(stderr, "VPSS_EnableChn[%d,DISP]: %#x\n", grp, ret); return -1; }

        ret = RK_MPI_VPSS_StartGrp(grp);
        if (ret) { fprintf(stderr, "VPSS_StartGrp[%d]: %#x\n", grp, ret); return -1; }
        ret = RK_MPI_VPSS_SetVProcDev(grp, VIDEO_PROC_DEV_VPSS);
        if (ret) { fprintf(stderr, "VPSS_SetVProcDev[%d]: %#x\n", grp, ret); return -1; }

        /* Bind VI[i,i] → VPSS[grp, 0] (group input) */
        MPP_CHN_S vi_chn, vpss_in;
        vi_chn.enModId = RK_ID_VI;
        vi_chn.s32DevId = i;
        vi_chn.s32ChnId = i;
        vpss_in.enModId = RK_ID_VPSS;
        vpss_in.s32DevId = grp;
        vpss_in.s32ChnId = 0;
        ret = RK_MPI_SYS_Bind(&vi_chn, &vpss_in);
        if (ret) { fprintf(stderr, "SYS_Bind VI[%d]→VPSS[%d]: %#x\n", i, grp, ret); return -1; }
    }
    printf("VPSS: grp%d (RGB) + grp%d (IR), each NN+DISP channels\n",
           VPSS_GRP_RGB, VPSS_GRP_IR);
    return 0;
}

static void vpss_deinit(void)
{
    int grps[NUM_CAMS] = { VPSS_GRP_RGB, VPSS_GRP_IR };
    for (int i = 0; i < NUM_CAMS; i++) {
        int grp = grps[i];
        MPP_CHN_S vi_chn, vpss_in;
        vi_chn.enModId = RK_ID_VI;
        vi_chn.s32DevId = i;
        vi_chn.s32ChnId = i;
        vpss_in.enModId = RK_ID_VPSS;
        vpss_in.s32DevId = grp;
        vpss_in.s32ChnId = 0;
        RK_MPI_SYS_UnBind(&vi_chn, &vpss_in);
        RK_MPI_VPSS_DisableChn(grp, CH_DISP);
        RK_MPI_VPSS_DisableChn(grp, CH_NN);
        RK_MPI_VPSS_StopGrp(grp);
        RK_MPI_VPSS_DestroyGrp(grp);
    }
}

/* ========================================================================
 * VO init — layer 0, VIDEO mode, BGR888, RGA splice (overlay plane)
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
 * RGA: NV12 → BGR888 + rotate 90 + scale → display
 * ====================================================================== */
static MB_BLK s_mmz[2] = {NULL, NULL};
static int s_mmzIdx = 0;

static int rga_init(void)
{
    int mmz_size = s_voDispW * s_voDispH * 3;
    for (int i = 0; i < 2; i++) {
        int ret = RK_MPI_MMZ_Alloc(&s_mmz[i], mmz_size, 0);
        if (ret) { fprintf(stderr, "MMZ_Alloc[%d]: %#x\n", i, ret); return -1; }
    }
    return 0;
}

static void rga_deinit(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_mmz[i]) RK_MPI_MMZ_Free(s_mmz[i]);
    }
}

static int rga_process(VIDEO_FRAME_INFO_S *vf, MB_BLK dst_blk)
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
int main(void)
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int ret = RK_MPI_SYS_Init();
    if (ret) { fprintf(stderr, "SYS_Init: %#x\n", ret); return 1; }

    if (vi_init() < 0) goto cleanup_sys;
    if (vpss_init() < 0) goto cleanup_vi;
    if (vo_init() < 0) goto cleanup_vpss;
    if (rga_init() < 0) goto cleanup_vo;

    int grps[NUM_CAMS] = { VPSS_GRP_RGB, VPSS_GRP_IR };
    const char *cam_names[NUM_CAMS] = { "RGB", "IR" };

    /* NN consumer threads — по одной на камеру */
    nn_ctx_t nn_ctx[NUM_CAMS];
    pthread_t nn_tid[NUM_CAMS];
    for (int i = 0; i < NUM_CAMS; i++) {
        nn_ctx[i].grp = grps[i];
        nn_ctx[i].cam_id = i;
        nn_ctx[i].name = cam_names[i];
        nn_ctx[i].running = 1;
        pthread_create(&nn_tid[i], NULL, nn_thread, &nn_ctx[i]);
    }

    /* Display loop — читаем CH_DISP только выбранной камеры */
    printf("\n=== TEST: 2 VPSS groups, display_ch switch every 5 sec ===\n\n");

    int display_ch = 0;
    double t0 = now_ms();
    double lastSwitch = t0;
    double lastReport = t0;
    int dispFrames = 0;
    int lastDispFrames = 0;

    while (!g_exit) {
        double elapsed = now_ms() - t0;
        if (elapsed > 60000) break;  /* 60 sec test */

        /* Переключение display_ch раз в 5 секунд */
        double now = now_ms();
        if (now - lastSwitch > 2000) {
            display_ch ^= 1;
            lastSwitch = now;
            printf("[%.1fs] display_ch → %d (%s)\n", elapsed / 1000.0, display_ch,
                   cam_names[display_ch]);
        }

        VIDEO_FRAME_INFO_S vf;
        memset(&vf, 0, sizeof(vf));
        ret = RK_MPI_VPSS_GetChnFrame(grps[display_ch], CH_DISP, &vf, 1000);
        if (ret) {
            if (dispFrames < 5)
                fprintf(stderr, "VPSS_GetChnFrame[disp,%d] failed: %#x\n", display_ch, ret);
            continue;
        }

        MB_BLK cur = s_mmz[s_mmzIdx];
        s_mmzIdx ^= 1;

        if (rga_process(&vf, cur) == 0) {
            send_to_vo(cur, (long long)vf.stVFrame.u64PTS);
            dispFrames++;
        }

        RK_MPI_VPSS_ReleaseChnFrame(grps[display_ch], CH_DISP, &vf);

        /* FPS отчёт раз в 5 сек */
        if (now - lastReport > 5000) {
            double fps = (dispFrames - lastDispFrames) / ((now - lastReport) / 1000.0);
            printf("[%.1fs] DISP %s: %.1f fps (total %d frames)\n",
                   elapsed / 1000.0, cam_names[display_ch], fps, dispFrames);
            lastReport = now;
            lastDispFrames = dispFrames;
        }
    }

    double total = (now_ms() - t0) / 1000.0;
    printf("\n=== RESULT: %d display frames in %.1fs = %.1f fps ===\n",
           dispFrames, total, dispFrames / total);

    /* Stop NN threads */
    for (int i = 0; i < NUM_CAMS; i++) {
        nn_ctx[i].running = 0;
        pthread_join(nn_tid[i], NULL);
    }

cleanup_rga:
    rga_deinit();
cleanup_vo:
    vo_deinit();
cleanup_vpss:
    vpss_deinit();
cleanup_vi:
    vi_deinit();
cleanup_sys:
    RK_MPI_SYS_Exit();
    printf("cleanup done\n");
    return 0;
}
