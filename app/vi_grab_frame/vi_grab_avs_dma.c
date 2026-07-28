/*
 * vi_grab_avs_dma.c — CLI для RV1126B: AVS → RGA crop+rotate → DMA буфер (zero-copy)
 *
 * Отличие от vi_grab_avs.c:
 *   - Выходной буфер RGA — DMA (через /dev/dma_heap/system-uncached), не malloc
 *   - DMA буфер можно: сохранить в файл (слепок), отправить в VO (дисплей),
 *     передать в rknn (NPU), или освободить.
 *   - Полный zero-copy: rockit dmabuf → RGA → DMA dmabuf → VO/rknn (без CPU copy)
 *
 * Пайплайн:
 *   VI dev0/1 → VI pipe0/1 → AVS grp0 → AVS chn0 (мега-кадр)
 *     → RGA crop+rotate → DMA буфер (cam0, cam1)
 *       → [save] fwrite (слепок для отладки)
 *       → [vo]   pan-and-scan: crop окно по синусоиде, rotate 90 → VO (дисплей 720×1280)
 *       → [free] dma_buf_free (просто освободить, benchmark)
 *
 * Использование:
 *   # Сохранить 2 половинки в файлы (как vi_grab_avs --split, но через DMA)
 *   ./vi_grab_avs_dma -w 1920 -h 1080 --action save
 *
 *   # Отправить на дисплей (HDMI/LCD) через VO
 *   ./vi_grab_avs_dma -w 1920 -h 1080 --action vo --vo-layer 0 --vo-chn 0
 *
 *   # Просто прогнать пайплайн, ничего не сохраняя (тест пропускной способности)
 *   ./vi_grab_avs_dma -w 1920 -h 1080 --action free -n 100
 *
 *   # С поворотом 90°
 *   ./vi_grab_avs_dma -w 1920 -h 1080 --action save --rotate-cam 90
 *
 * Параметры (дополнительно к vi_grab_avs):
 *   --action    save|vo|free  что делать с DMA буфером (по умолчанию save)
 *   --vo-layer  N             VO layer для --action vo (по умолчанию 0)
 *   --vo-chn    N             VO channel для --action vo (по умолчанию 0)
 *
 * Остальные параметры как в vi_grab_avs.c (-w -h -m -n -s --rotate-cam -t -v).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#include "rk_defines.h"
#include "rk_debug.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_comm_avs.h"
#include "rk_comm_mb.h"
#include "rk_comm_vo.h"
#include "rk_comm_video.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_avs.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_mmz.h"

/* RGA (2D hardware accelerator) for split/rotate */
#include "im2d.h"
#include "rga.h"

/* DMA buffer allocation via /dev/dma_heap/ (zero-copy для RGA/VO/NPU) */
#include "dma_alloc.h"

#define DEFAULT_TIMEOUT_MS  2000
#define DEFAULT_CHANNEL_ID  0    /* RKISP_MAINPATH */
#define DEFAULT_FRAME_COUNT 1
#define NUM_SENSORS         2
#define AVS_GRP_ID          0
#define AVS_CHN_ID          0

/* Что делать с DMA буфером после RGA */
typedef enum {
    ACTION_SAVE,   /* fwrite в файл (слепок для отладки) */
    ACTION_VO,     /* отправить в VO (дисплей) */
    ACTION_FREE    /* просто освободить (benchmark) */
} action_t;

typedef struct {
    int width;
    int height;
    int mode;          /* AVS_MODE_E */
    int channelId;
    int frameCount;
    int skipFrames;
    int rotateCam;     /* 0/90/180/270 — поворот каждой половинки в RGA */
    int timeoutMs;
    int verbose;
    int bSyncPipe;
    char calibFile[256];
    char outputPrefix[128];
    action_t action;
    int voDevId;
    int voLayer;
    int voChn;
    int voDispW;    /* display width (from VO driver, for pan-and-scan) */
    int voDispH;    /* display height (from VO driver, for pan-and-scan) */
    MB_BLK panBlk[2]; /* double-buffered MMZ for pan-and-scan (VO holds one, we fill other) */
    int panIdx;     /* current buffer index (0 or 1) */
    int panBufSize; /* size of each panBlk buffer */
} app_ctx_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -w <W> -h <H> [options]\n"
        "\n"
        "Hardware stitch two sensors via AVS, split+rotate via RGA into DMA buffers.\n"
        "DMA buffers can be: saved to file, sent to VO (display), or freed.\n"
        "\n"
        "Options:\n"
        "  -w, --width <W>       single sensor width (required)\n"
        "  -h, --height <H>      single sensor height (required)\n"
        "  -m, --mode <MODE>     AVS mode: hor, ver, blend (default: hor)\n"
        "  -c, --channel <ID>    VI channel id (default: %d = MAINPATH)\n"
        "  -o, --output <PFX>    output file prefix (default: \"cam\")\n"
        "  -n, --count <N>       number of frames to process (default: %d)\n"
        "  -s, --skip <N>        discard first N frames for warmup (default: 0)\n"
        "  --rotate-cam <DEG>    rotate each half in RGA: 0/90/180/270 (default: 0)\n"
        "  -t, --timeout <MS>    GetChnFrame timeout in ms (default: %d)\n"
        "  --calib <FILE>        calibration XML path (for blend mode)\n"
        "  --no-sync             disable bSyncPipe (not recommended)\n"
        "  --action <ACT>        what to do with DMA buffer: save|vo|free (default: save)\n"
        "  --vo-dev <N>          VO device id for --action vo (default: 0)\n"
        "  --vo-layer <N>        VO layer for --action vo (default: 1)\n"
        "  --vo-chn <N>          VO channel for --action vo (default: 0)\n"
        "  -v, --verbose         verbose output\n"
        "  --help                show this help\n"
        "\n"
        "Actions:\n"
        "  save   — write DMA buffer to file (snapshot for debugging)\n"
        "  vo     — pan-and-scan: crop window from panorama by sine wave, rotate 90, send to display\n"
        "  free   — just release DMA buffer (benchmark pipeline throughput)\n"
        "\n"
        "Examples:\n"
        "  %s -w 1920 -h 1080 --action save\n"
        "  %s -w 1920 -h 1080 --action save --rotate-cam 90\n"
        "  %s -w 1920 -h 1080 --action vo --vo-dev 0 --vo-layer 1 --vo-chn 0 -n 300  # pan-and-scan display\n"
        "  %s -w 1920 -h 1080 --action free -n 100   # benchmark\n",
        prog, DEFAULT_CHANNEL_ID, DEFAULT_FRAME_COUNT, DEFAULT_TIMEOUT_MS,
        prog, prog, prog, prog);
}

static int parse_mode(const char *s) {
    if (!strcmp(s, "hor"))    return AVS_MODE_NOBLEND_HOR;
    if (!strcmp(s, "ver"))    return AVS_MODE_NOBLEND_VER;
    if (!strcmp(s, "blend"))  return AVS_MODE_BLEND;
    fprintf(stderr, "Unknown mode: %s (use hor/ver/blend)\n", s);
    return -1;
}

static int parse_args(app_ctx_t *ctx, int argc, char **argv) {
    static struct option long_opts[] = {
        {"width",   required_argument, 0, 'w'},
        {"height",  required_argument, 0, 'h'},
        {"mode",    required_argument, 0, 'm'},
        {"channel", required_argument, 0, 'c'},
        {"output",  required_argument, 0, 'o'},
        {"count",   required_argument, 0, 'n'},
        {"skip",    required_argument, 0, 's'},
        {"timeout", required_argument, 0, 't'},
        {"calib",      required_argument, 0, 1000},
        {"no-sync",    no_argument,       0, 1001},
        {"rotate-cam", required_argument, 0, 1003},
        {"action",     required_argument, 0, 1004},
        {"vo-dev",     required_argument, 0, 1007},
        {"vo-layer",   required_argument, 0, 1005},
        {"vo-chn",     required_argument, 0, 1006},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    memset(ctx, 0, sizeof(*ctx));
    ctx->width = 0;
    ctx->height = 0;
    ctx->mode = AVS_MODE_NOBLEND_HOR;
    ctx->channelId = DEFAULT_CHANNEL_ID;
    ctx->frameCount = DEFAULT_FRAME_COUNT;
    ctx->skipFrames = 0;
    ctx->rotateCam = 0;
    ctx->timeoutMs = DEFAULT_TIMEOUT_MS;
    ctx->verbose = 0;
    ctx->bSyncPipe = 1;
    ctx->calibFile[0] = '\0';
    ctx->action = ACTION_SAVE;
    ctx->voDevId = 0;
    ctx->voLayer = 1;  /* layer 1 = Smart/UI (rkipc uses it, works with weston) */
    ctx->voChn = 0;
    ctx->voDispW = 720;
    ctx->voDispH = 1280;
    strcpy(ctx->outputPrefix, "cam");

    int opt;
    while ((opt = getopt_long(argc, argv, "w:h:m:c:o:n:s:t:v", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'w': ctx->width = atoi(optarg); break;
            case 'h': ctx->height = atoi(optarg); break;
            case 'm': {
                int m = parse_mode(optarg);
                if (m < 0) return -1;
                ctx->mode = m;
                break;
            }
            case 'c': ctx->channelId = atoi(optarg); break;
            case 'o': strncpy(ctx->outputPrefix, optarg, sizeof(ctx->outputPrefix) - 1); break;
            case 'n': ctx->frameCount = atoi(optarg); break;
            case 's': ctx->skipFrames = atoi(optarg); break;
            case 't': ctx->timeoutMs = atoi(optarg); break;
            case 1000: strncpy(ctx->calibFile, optarg, sizeof(ctx->calibFile) - 1); break;
            case 1001: ctx->bSyncPipe = 0; break;
            case 1003: {
                int r = atoi(optarg);
                if (r != 0 && r != 90 && r != 180 && r != 270) {
                    fprintf(stderr, "Invalid --rotate-cam: %d (use 0/90/180/270)\n", r);
                    return -1;
                }
                ctx->rotateCam = r;
                break;
            }
            case 1004: {
                if (!strcmp(optarg, "vo"))        ctx->action = ACTION_VO;
                else if (!strcmp(optarg, "free")) ctx->action = ACTION_FREE;
                else                              ctx->action = ACTION_SAVE;
                break;
            }
            case 1005: ctx->voLayer = atoi(optarg); break;
            case 1006: ctx->voChn = atoi(optarg); break;
            case 1007: ctx->voDevId = atoi(optarg); break;
            case 'v': ctx->verbose = 1; break;
            case '?':
            default:
                usage(argv[0]);
                return -1;
        }
    }

    if (ctx->width <= 0 || ctx->height <= 0) {
        fprintf(stderr, "Error: width and height are required\n\n");
        usage(argv[0]);
        return -1;
    }

    if (ctx->mode == AVS_MODE_BLEND && ctx->calibFile[0] == '\0') {
        fprintf(stderr, "Error: blend mode requires --calib <FILE>\n\n");
        usage(argv[0]);
        return -1;
    }

    return 0;
}

static long long get_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Преобразовать градусы в RGA rotation flag */
static int deg_to_rga_rotation(int deg) {
    switch (deg) {
        case 90:  return IM_HAL_TRANSFORM_ROT_90;
        case 180: return IM_HAL_TRANSFORM_ROT_180;
        case 270: return IM_HAL_TRANSFORM_ROT_270;
        default:  return 0;
    }
}

/* Заглушка для pat параметра improcess (не используется) */
static rga_buffer_t pat_dummy(void) {
    rga_buffer_t p;
    memset(&p, 0, sizeof(p));
    return p;
}

/*
 * rga_split_to_dma — нарезать мега-кадр на 2 половинки через RGA в DMA буферы.
 *
 * ZERO-COPY полностью:
 *   src — dmabuf fd от rockit MB (RGA читает через IOMMU)
 *   dst — DMA буфер от /dev/dma_heap/system-uncached (RGA пишет через IOMMU)
 *
 * Возвращает 0 при успехе. Заполняет out_fds[2], out_vas[2], out_w, out_h.
 */
static int rga_split_to_dma(MB_BLK mb, int mega_w, int mega_h,
                            int cam_w, int cam_h, int mode, int rotation,
                            int out_fds[2], void *out_vas[2],
                            int *out_w, int *out_h, int verbose) {
    int rga_rot = deg_to_rga_rotation(rotation);
    int ow = (rotation == 90 || rotation == 270) ? cam_h : cam_w;
    int oh = (rotation == 90 || rotation == 270) ? cam_w : cam_h;
    int osize = ow * oh * 3 / 2;  /* NV12 */

    /* src — zero-copy из rockit dmabuf */
    int src_fd = RK_MPI_MB_Handle2Fd(mb);
    if (src_fd < 0) {
        fprintf(stderr, "[rga] Handle2Fd failed\n");
        return -1;
    }
    rga_buffer_t src = wrapbuffer_fd_t(src_fd, mega_w, mega_h,
                                       mega_w, mega_h, RK_FORMAT_YCbCr_420_SP);

    /* dst — 2 DMA буфера (uncached: RGA пишет через IOMMU, CPU читает без sync) */
    int cam;
    for (cam = 0; cam < NUM_SENSORS; cam++) {
        int rc = dma_buf_alloc(DMA_HEAP_UNCACHE_PATH, osize, &out_fds[cam], &out_vas[cam]);
        if (rc < 0) {
            fprintf(stderr, "[rga] cam%d dma_buf_alloc failed\n", cam);
            if (cam == 1) dma_buf_free(osize, &out_fds[0], out_vas[0]);
            return -1;
        }
    }

    for (cam = 0; cam < NUM_SENSORS; cam++) {
        /* srect — координаты половинки в мега-кадре (crop region) */
        im_rect srect;
        memset(&srect, 0, sizeof(srect));
        if (mode == AVS_MODE_NOBLEND_VER) {
            srect.x = 0;
            srect.y = (cam == 0) ? 0 : cam_h;
            srect.width = cam_w; srect.height = cam_h;
        } else {
            srect.x = (cam == 0) ? 0 : cam_w;
            srect.y = 0;
            srect.width = cam_w; srect.height = cam_h;
        }

        im_rect drect; memset(&drect, 0, sizeof(drect));
        drect.width = ow; drect.height = oh;
        im_rect prect; memset(&prect, 0, sizeof(prect));

        /* dst — каждый раз новый DMA буфер */
        rga_buffer_t dst = wrapbuffer_fd_t(out_fds[cam], ow, oh, ow, oh,
                                           RK_FORMAT_YCbCr_420_SP);

        int usage = rga_rot | IM_SYNC;
        IM_STATUS st = improcess(src, dst, pat_dummy(), srect, drect, prect, usage);
        if (st != IM_STATUS_SUCCESS) {
            fprintf(stderr, "[rga] cam%d improcess failed: %d (rot=%d, crop=[%d,%d,%d,%d])\n",
                    cam, (int)st, rotation, srect.x, srect.y, srect.width, srect.height);
            return -1;
        }
        if (verbose)
            printf("  [rga] cam%d: crop=[%d,%d,%d,%d] → %dx%d rot=%d → DMA fd=%d\n",
                   cam, srect.x, srect.y, srect.width, srect.height,
                   ow, oh, rotation, out_fds[cam]);
    }

    *out_w = ow; *out_h = oh;
    return 0;
}

/*
 * rga_pan_to_dma — pan-and-scan: вырезать окно crop_w×crop_h из мега-кадра,
 * позиция окна меняется по синусоиде (эффект плавного движения по панораме),
 * повернуть на 90° → dst_w×dst_h (портрет для DSI дисплея).
 *
 * ZERO-COPY: src — rockit dmabuf, dst — DMA буфер.
 * frame_idx — номер кадра (для синусоиды), mega_w/h — размер мега-кадра.
 * Возвращает 0 при успехе. Заполняет out_fd, out_va, out_w, out_h.
 */
static int rga_pan_to_mmz(MB_BLK mb, int mega_w, int mega_h,
                          int crop_w, int crop_h, int dst_w, int dst_h,
                          int frame_idx, int verbose,
                          MB_BLK dst_blk,
                          int *out_w, int *out_h) {
    /* Синусоиды: окно плавно гуляет по панораме туда-обратно */
    float t = (float)frame_idx;
    float sx = sinf(t * 0.04f) * 0.5f + 0.5f;       /* медленно по X */
    float sy = sinf(t * 0.06f + 1.7f) * 0.5f + 0.5f; /* медленно по Y, сдвиг фазы */
    int max_x = mega_w - crop_w;
    int max_y = mega_h - crop_h;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    int crop_x = (int)(sx * max_x) & ~1;  /* NV12: even alignment (UV plane) */
    int crop_y = (int)(sy * max_y) & ~1;

    int src_fd = RK_MPI_MB_Handle2Fd(mb);
    int dst_fd = RK_MPI_MB_Handle2Fd(dst_blk);
    if (src_fd < 0 || dst_fd < 0) {
        fprintf(stderr, "[rga] pan: Handle2Fd failed (src=%d dst=%d)\n", src_fd, dst_fd);
        return -1;
    }

    rga_buffer_t src = wrapbuffer_fd_t(src_fd, mega_w, mega_h,
                                       mega_w, mega_h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_fd_t(dst_fd, dst_w, dst_h, dst_w, dst_h,
                                       RK_FORMAT_BGRA_8888);

    im_rect srect = { .x = crop_x, .y = crop_y, .width = crop_w, .height = crop_h };
    im_rect drect = { .width = dst_w, .height = dst_h };
    im_rect prect = {0};

    /* crop + rotate 90 + scale (если crop ≠ dst) в одной операции */
    int usage = IM_HAL_TRANSFORM_ROT_90 | IM_SYNC;
    /* TEST: skip RGA convert to check grab time */
#if 1
    IM_STATUS st = improcess(src, dst, pat_dummy(), srect, drect, prect, usage);
    if (st != IM_STATUS_SUCCESS) {
        fprintf(stderr, "[rga] pan: improcess failed: %d (crop=[%d,%d,%d,%d] → %dx%d rot=90)\n",
                (int)st, crop_x, crop_y, crop_w, crop_h, dst_w, dst_h);
        return -1;
    }
    /* No flush needed — non-cacheable MMZ buffer */
#endif

    if (verbose && (frame_idx % 30 == 0))
        printf("  [rga] pan: crop=[%d,%d,%d,%d] → %dx%d rot=90 (sin x=%.2f y=%.2f)\n",
               crop_x, crop_y, crop_w, crop_h, dst_w, dst_h, sx, sy);

    *out_w = dst_w; *out_h = dst_h;
    return 0;
}

/* Сохранить DMA буфер в файл (слепок для отладки) */
static int save_dma_to_file(void *va, int size, const char *prefix,
                            int cam, int w, int h, long long pts) {
    char fname[300];
    snprintf(fname, sizeof(fname), "%s%d_%dx%d_pts%lld_nv12.raw",
             prefix, cam, w, h, pts);
    FILE *fp = fopen(fname, "wb");
    if (!fp) { fprintf(stderr, "[save] cam%d: cannot open %s\n", cam, fname); return -1; }
    size_t wr = fwrite(va, 1, size, fp);
    fclose(fp);
    printf("  [save] cam%d → %s (%zu bytes)\n", cam, fname, wr);
    return 0;
}

/*
 * Отправить MMZ буфер в VO (дисплей) — rockit-managed buffer.
 * VO splice RGA can access MMZ buffer (like rk_ui.c SendFrame).
 */
static int send_mmz_to_vo(MB_BLK blk, int w, int h,
                          int vo_layer, int vo_chn, long long pts) {
    VIDEO_FRAME_INFO_S vf;
    memset(&vf, 0, sizeof(vf));
    vf.stVFrame.pMbBlk        = blk;
    vf.stVFrame.u32Width      = w;
    vf.stVFrame.u32Height     = h;
    vf.stVFrame.u32VirWidth   = w;
    vf.stVFrame.u32VirHeight  = h;
    vf.stVFrame.enPixelFormat = RK_FMT_BGRA8888;  /* BGRA8888 — matches layer */
    vf.stVFrame.u64PTS        = pts;

    RK_S32 ret = RK_MPI_VO_SendFrame(vo_layer, vo_chn, &vf, 1000);  /* wait up to 1s */
    if (ret != RK_SUCCESS)
        fprintf(stderr, "[vo] cam: VO_SendFrame failed: %#x\n", ret);
    return (ret == RK_SUCCESS) ? 0 : -1;
}

/* Вычислить размер мега-кадра */
static void calc_mega_size(app_ctx_t *ctx, int *mega_w, int *mega_h) {
    if (ctx->mode == AVS_MODE_NOBLEND_HOR) {
        *mega_w = ctx->width * NUM_SENSORS;
        *mega_h = ctx->height;
    } else if (ctx->mode == AVS_MODE_NOBLEND_VER) {
        *mega_w = ctx->width;
        *mega_h = ctx->height * NUM_SENSORS;
    } else {
        *mega_w = ctx->width * NUM_SENSORS;
        *mega_h = ctx->height;
    }
}

static volatile sig_atomic_t g_exit = 0;
static void sig_handler(int s) { g_exit = 1; (void)s; }

int main(int argc, char **argv) {
    app_ctx_t ctx;
    int ret, i, frame;
    int vo_inited = 0;

    if (parse_args(&ctx, argc, argv) != 0)
        return 1;

    int mega_w, mega_h;
    calc_mega_size(&ctx, &mega_w, &mega_h);

    const char *mode_str = ctx.mode == AVS_MODE_NOBLEND_HOR ? "NOBLEND_HOR" :
                           ctx.mode == AVS_MODE_NOBLEND_VER ? "NOBLEND_VER" :
                           "BLEND";
    const char *action_str = ctx.action == ACTION_SAVE ? "save" :
                             ctx.action == ACTION_VO   ? "vo"  : "free";

    printf("vi_grab_avs_dma: %dx%d per sensor, mode=%s, mega=%dx%d, rot=%d, action=%s\n",
           ctx.width, ctx.height, mode_str, mega_w, mega_h, ctx.rotateCam, action_str);

    signal(SIGINT, sig_handler);

    /* 1. Инициализация MPI */
    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_SYS_Init failed: %#x\n", ret);
        return 1;
    }
    if (ctx.verbose) printf("RK_MPI_SYS_Init OK\n");

    /* 2. VI device init для каждого сенсора */
    for (i = 0; i < NUM_SENSORS; i++) {
        VI_DEV_ATTR_S stDevAttr;
        VI_DEV_BIND_PIPE_S stBindPipe;
        memset(&stDevAttr, 0, sizeof(stDevAttr));
        memset(&stBindPipe, 0, sizeof(stBindPipe));

        ret = RK_MPI_VI_GetDevAttr(i, &stDevAttr);
        if (ret == RK_ERR_VI_NOT_CONFIG) {
            ret = RK_MPI_VI_SetDevAttr(i, &stDevAttr);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "dev %d: SetDevAttr failed: %#x\n", i, ret);
                goto cleanup_sys;
            }
        }

        ret = RK_MPI_VI_GetDevIsEnable(i);
        if (ret != RK_SUCCESS) {
            ret = RK_MPI_VI_EnableDev(i);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "dev %d: EnableDev failed: %#x\n", i, ret);
                goto cleanup_dev;
            }
            stBindPipe.u32Num = 1;
            stBindPipe.PipeId[0] = i;
            stBindPipe.bUserStartPipe[0] = RK_TRUE;
            ret = RK_MPI_VI_SetDevBindPipe(i, &stBindPipe);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "dev %d: SetDevBindPipe failed: %#x\n", i, ret);
                goto cleanup_dev;
            }
        }
        if (ctx.verbose) printf("dev %d: enabled\n", i);
    }

    /* 3. VI каналы — EnableChnExt + StartPipe (group mode) */
    for (i = 0; i < NUM_SENSORS; i++) {
        VI_CHN_ATTR_S vi_chn_attr;
        memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
        vi_chn_attr.stIspOpt.u32BufCount = 3;
        vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
        vi_chn_attr.stSize.u32Width = ctx.width;
        vi_chn_attr.stSize.u32Height = ctx.height;
        vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
        vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
        vi_chn_attr.u32Depth = 0;

        ret = RK_MPI_VI_SetChnAttr(i, ctx.channelId, &vi_chn_attr);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "sensor %d: SetChnAttr failed: %#x\n", i, ret);
            goto cleanup_dev;
        }
        ret = RK_MPI_VI_EnableChnExt(i, ctx.channelId);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "sensor %d: EnableChnExt failed: %#x\n", i, ret);
            goto cleanup_chn;
        }
    }

    for (i = 0; i < NUM_SENSORS; i++) {
        ret = RK_MPI_VI_StartPipe(i);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "pipe %d: StartPipe failed: %#x\n", i, ret);
            goto cleanup_chn;
        }
    }

    /* 4. AVS init */
    AVS_MOD_PARAM_S stAvsModParam;
    AVS_GRP_ATTR_S stAvsGrpAttr;
    memset(&stAvsModParam, 0, sizeof(stAvsModParam));
    memset(&stAvsGrpAttr, 0, sizeof(stAvsGrpAttr));

    stAvsModParam.u32WorkingSetSize = 0;
    stAvsModParam.enMBSource = MB_SOURCE_PRIVATE;

    stAvsGrpAttr.enMode = ctx.mode;
    stAvsGrpAttr.u32PipeNum = NUM_SENSORS;
    stAvsGrpAttr.bSyncPipe = ctx.bSyncPipe;
    stAvsGrpAttr.stGainAttr.enMode = AVS_GAIN_MODE_AUTO;
    stAvsGrpAttr.stOutAttr.enPrjMode = AVS_PROJECTION_EQUIRECTANGULAR;
    stAvsGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stAvsGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stAvsGrpAttr.stInAttr.stSize.u32Width = ctx.width;
    stAvsGrpAttr.stInAttr.stSize.u32Height = ctx.height;
    stAvsGrpAttr.stOutAttr.fDistance = 5;

    if (ctx.calibFile[0] != '\0') {
        stAvsGrpAttr.stInAttr.enParamSource = AVS_PARAM_SOURCE_CALIB;
        stAvsGrpAttr.stInAttr.stCalib.pCalibFilePath = ctx.calibFile;
    } else {
        stAvsGrpAttr.stInAttr.enParamSource = AVS_PARAM_SOURCE_LUT;
        stAvsGrpAttr.stInAttr.stLUT.enAccuracy = AVS_LUT_ACCURACY_HIGH;
        stAvsGrpAttr.stInAttr.stLUT.enFuseWidth = AVS_FUSE_WIDTH_MEDIUM;
        stAvsGrpAttr.stInAttr.stLUT.stLutStep.enStepX = AVS_LUT_STEP_MEDIUM;
        stAvsGrpAttr.stInAttr.stLUT.stLutStep.enStepY = AVS_LUT_STEP_MEDIUM;
    }

    ret = RK_MPI_AVS_SetModParam(&stAvsModParam);
    if (ret != RK_SUCCESS) { fprintf(stderr, "AVS_SetModParam: %#x\n", ret); goto cleanup_pipe; }

    ret = RK_MPI_AVS_CreateGrp(AVS_GRP_ID, &stAvsGrpAttr);
    if (ret != RK_SUCCESS) { fprintf(stderr, "AVS_CreateGrp: %#x\n", ret); goto cleanup_pipe; }

    /* LDCH (Lens Distortion Correction) — нужно для AVS */
    {
        AVS_FINAL_LUT_S pstFinalLut;
        PIC_BUF_ATTR_S stBufAttr;
        MB_PIC_CAL_S pic_cal[NUM_SENSORS];
        MB_EXT_CONFIG_S stMbExtConfig;
        void *ldch_data[NUM_SENSORS];

        memset(&pstFinalLut, 0, sizeof(pstFinalLut));
        for (i = 0; i < NUM_SENSORS; i++) {
            memset(&stBufAttr, 0, sizeof(stBufAttr));
            memset(&pic_cal[i], 0, sizeof(pic_cal[i]));
            stBufAttr.u32Width = ctx.width;
            stBufAttr.u32Height = ctx.height;
            ret = RK_MPI_CAL_AVS_GetFinalLutBufferSize(&stBufAttr, &pic_cal[i]);
            if (ret != RK_SUCCESS || pic_cal[i].u32MBSize == 0) continue;

            ldch_data[i] = malloc(pic_cal[i].u32MBSize);
            memset(&stMbExtConfig, 0, sizeof(stMbExtConfig));
            stMbExtConfig.pu8VirAddr = (RK_U8 *)ldch_data[i];
            stMbExtConfig.u64Size = pic_cal[i].u32MBSize;
            ret = RK_MPI_SYS_CreateMB(&pstFinalLut.pLdchBlk[i], &stMbExtConfig);
            if (ret != RK_SUCCESS) { free(ldch_data[i]); continue; }
        }
        ret = RK_MPI_AVS_GetFinalLut(AVS_GRP_ID, &pstFinalLut);
        for (i = 0; i < NUM_SENSORS; i++) {
            if (pstFinalLut.pLdchBlk[i]) RK_MPI_SYS_Free(pstFinalLut.pLdchBlk[i]);
            if (ldch_data[i]) free(ldch_data[i]);
        }
    }

    /* 5. AVS channel */
    AVS_CHN_ATTR_S stAvsChnAttr;
    memset(&stAvsChnAttr, 0, sizeof(stAvsChnAttr));
    stAvsChnAttr.u32Width = mega_w;
    stAvsChnAttr.u32Height = mega_h;
    stAvsChnAttr.enCompressMode = COMPRESS_MODE_NONE;
    stAvsChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stAvsChnAttr.u32Depth = 1;
    stAvsChnAttr.u32FrameBufCnt = 2;
    stAvsChnAttr.stFrameRate.s32SrcFrameRate = -1;
    stAvsChnAttr.stFrameRate.s32DstFrameRate = -1;

    ret = RK_MPI_AVS_SetChnAttr(AVS_GRP_ID, AVS_CHN_ID, &stAvsChnAttr);
    if (ret != RK_SUCCESS) { fprintf(stderr, "AVS_SetChnAttr: %#x\n", ret); goto cleanup_avs_grp; }
    ret = RK_MPI_AVS_EnableChn(AVS_GRP_ID, AVS_CHN_ID);
    if (ret != RK_SUCCESS) { fprintf(stderr, "AVS_EnableChn: %#x\n", ret); goto cleanup_avs_grp; }

    /* 6. Bind VI → AVS */
    for (i = 0; i < NUM_SENSORS; i++) {
        MPP_CHN_S vi_chn, avs_in_chn;
        vi_chn.enModId = RK_ID_VI;
        vi_chn.s32DevId = i;
        vi_chn.s32ChnId = ctx.channelId;
        avs_in_chn.enModId = RK_ID_AVS;
        avs_in_chn.s32DevId = AVS_GRP_ID;
        avs_in_chn.s32ChnId = i;
        ret = RK_MPI_SYS_Bind(&vi_chn, &avs_in_chn);
        if (ret != RK_SUCCESS) { fprintf(stderr, "Bind VI%d→AVS: %#x\n", i, ret); goto cleanup_avs_chn; }
    }

    /* 7. Start AVS group */
    ret = RK_MPI_AVS_StartGrp(AVS_GRP_ID);
    if (ret != RK_SUCCESS) { fprintf(stderr, "AVS_StartGrp: %#x\n", ret); goto cleanup_bind; }

    /* 7b. VO init (only if --action vo) — DSI display 720x1280, layer 1, VO_INTF_MIPI */
    if (ctx.action == ACTION_VO) {
        VO_PUB_ATTR_S VoPubAttr;
        VO_VIDEO_LAYER_ATTR_S stLayerAttr;
        VO_CSC_S VideoCSC;
        VO_CHN_ATTR_S VoChnAttr;
        memset(&VoPubAttr, 0, sizeof(VoPubAttr));
        memset(&stLayerAttr, 0, sizeof(stLayerAttr));
        memset(&VideoCSC, 0, sizeof(VideoCSC));
        memset(&VoChnAttr, 0, sizeof(VoChnAttr));

        /* DSI display: 720x1280 (portrait). rkipc uses VO_INTF_MIPI, dev=0, layer=1. */
        VoPubAttr.enIntfType = VO_INTF_MIPI;
        VoPubAttr.enIntfSync = VO_OUTPUT_DEFAULT;

        /* Init order: BindLayer first, then SetPubAttr/Enable (old order — avoids RGA conflicts) */
        ret = RK_MPI_VO_BindLayer(ctx.voLayer, ctx.voDevId, VO_LAYER_MODE_GRAPHIC);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_BindLayer: %#x\n", ret); goto cleanup_bind; }
        ret = RK_MPI_VO_SetPubAttr(ctx.voDevId, &VoPubAttr);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetPubAttr: %#x\n", ret); goto cleanup_bind; }
        ret = RK_MPI_VO_Enable(ctx.voDevId);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_Enable: %#x\n", ret); goto cleanup_bind; }

        /* Get real display dimensions from driver */
        ret = RK_MPI_VO_GetPubAttr(ctx.voDevId, &VoPubAttr);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_GetPubAttr: %#x\n", ret); goto cleanup_vo_dev; }
        if (VoPubAttr.stSyncInfo.u16Hact == 0 || VoPubAttr.stSyncInfo.u16Vact == 0) {
            VoPubAttr.stSyncInfo.u16Hact = 720;
            VoPubAttr.stSyncInfo.u16Vact = 1280;
        }
        int disp_w = VoPubAttr.stSyncInfo.u16Hact;
        int disp_h = VoPubAttr.stSyncInfo.u16Vact;
        ctx.voDispW = disp_w;
        ctx.voDispH = disp_h;
        if (ctx.verbose) printf("VO: display %dx%d (from driver)\n", disp_w, disp_h);

        stLayerAttr.stDispRect.s32X = 0;
        stLayerAttr.stDispRect.s32Y = 0;
        stLayerAttr.stDispRect.u32Width = disp_w;
        stLayerAttr.stDispRect.u32Height = disp_h;
        stLayerAttr.stImageSize.u32Width = disp_w;
        stLayerAttr.stImageSize.u32Height = disp_h;
        stLayerAttr.u32DispFrmRt = 30;
        /* BGRA8888: our RGA converts NV12→BGRA8888, bypass passes MMZ to DSI */
        stLayerAttr.enPixFormat = RK_FMT_BGRA8888;
        VideoCSC.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
        VideoCSC.u32Contrast = 50;
        VideoCSC.u32Hue = 50;
        VideoCSC.u32Luma = 50;
        VideoCSC.u32Satuature = 50;

        ret = RK_MPI_VO_SetLayerAttr(ctx.voLayer, &stLayerAttr);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetLayerAttr: %#x\n", ret); goto cleanup_vo_dev; }
        /* Splice mode (not bypass): VO uses RGA to composite frame to DSI.
           Bypass mode gives black screen without weston (DRM master issue). */
        stLayerAttr.bBypassFrame = RK_FALSE;
        ret = RK_MPI_VO_SetLayerAttr(ctx.voLayer, &stLayerAttr);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetLayerAttr bypass: %#x\n", ret); goto cleanup_vo_dev; }
        ret = RK_MPI_VO_EnableLayer(ctx.voLayer);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_EnableLayer: %#x\n", ret); goto cleanup_vo_dev; }
        ret = RK_MPI_VO_SetLayerCSC(ctx.voLayer, &VideoCSC);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetLayerCSC: %#x\n", ret); goto cleanup_vo_layer; }

        VoChnAttr.bDeflicker = RK_FALSE;
        VoChnAttr.u32Priority = 1;
        VoChnAttr.stRect.s32X = 0;
        VoChnAttr.stRect.s32Y = 0;
        VoChnAttr.stRect.u32Width = disp_w;
        VoChnAttr.stRect.u32Height = disp_h;
        ret = RK_MPI_VO_SetChnAttr(ctx.voLayer, ctx.voChn, &VoChnAttr);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetChnAttr: %#x\n", ret); goto cleanup_vo_layer; }
        ret = RK_MPI_VO_EnableChn(ctx.voLayer, ctx.voChn);
        if (ret != RK_SUCCESS) { fprintf(stderr, "VO_EnableChn: %#x\n", ret); goto cleanup_vo_layer; }

        vo_inited = 1;
        if (ctx.verbose) printf("VO: layer=%d dev=%d chn=%d enabled (%dx%d DSI)\n",
                                ctx.voLayer, ctx.voDevId, ctx.voChn, disp_w, disp_h);
    }

    /* 8. Захват и обработка */
    printf("Pipeline: VI×2 → AVS (mega %dx%d) → RGA crop+rotate(rot=%d) → DMA → %s\n",
           mega_w, mega_h, ctx.rotateCam, action_str);
    printf("Waiting for frames (sync=%d)...\n", ctx.bSyncPipe);

    int total = ctx.skipFrames + ctx.frameCount;
    int saved = 0;
    for (frame = 0; frame < total && !g_exit; frame++) {
        VIDEO_FRAME_INFO_S stMegaFrame;
        memset(&stMegaFrame, 0, sizeof(stMegaFrame));

        static long long t_prev_start = 0;
        static long long t_prev_pts = 0;
        long long t_start = get_now_ms();
        ret = RK_MPI_AVS_GetChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stMegaFrame, ctx.timeoutMs);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "Frame %d: AVS_GetChnFrame failed: %#x\n", frame, ret);
            continue;
        }
        long long t_grab = get_now_ms() - t_start;
        int w = stMegaFrame.stVFrame.u32Width;
        int h = stMegaFrame.stVFrame.u32Height;
        long long pts_us = (long long)stMegaFrame.stVFrame.u64PTS;
        long long wall_delta = t_prev_start ? (t_start - t_prev_start) : 0;
        long long pts_delta = t_prev_pts ? (pts_us - t_prev_pts) : 0;
        t_prev_start = t_start;
        t_prev_pts = pts_us;

        if (frame < ctx.skipFrames) {
            if (ctx.verbose) printf("Frame %d [skip]: pts=%lldus grab=%lldms\n", frame, pts_us, t_grab);
            RK_MPI_AVS_ReleaseChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stMegaFrame);
            continue;
        }

        printf("Frame %d [proc]: %dx%d pts=%lldus grab=%lldms wall_delta=%lldms pts_delta=%lldms → %s\n",
               frame, w, h, pts_us, t_grab, wall_delta, pts_delta/1000, action_str);

        int osize;
        long long t_act_start = get_now_ms();

        if (ctx.action == ACTION_VO) {
            /* Pan-and-scan: crop окно из мега-кадра по синусоиде, rotate 90 → дисплей */
            int pan_w, pan_h;
            /* crop 1280×720 (landscape) из мега, rotate 90 → 720×1280 (портрет) */
            int crop_w = 1280, crop_h = 720;
            if (crop_w > w) crop_w = w;
            if (crop_h > h) crop_h = h;
            /* Allocate double-buffered MMZ (VO holds one buffer, we fill the other) */
            if (ctx.panBlk[0] == NULL) {
                int psize = ctx.voDispW * ctx.voDispH * 4;  /* BGRA8888 */
                for (int b = 0; b < 2; b++) {
                    RK_S32 rc = RK_MPI_MMZ_Alloc(&ctx.panBlk[b], psize, 0);  /* non-cacheable */
                    if (rc != RK_SUCCESS) {
                        fprintf(stderr, "  MMZ_Alloc pan[%d] failed: %#x\n", b, rc);
                        RK_MPI_AVS_ReleaseChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stMegaFrame);
                        goto cleanup_bind;
                    }
                }
                ctx.panBufSize = psize;
                ctx.panIdx = 0;
            }
            MB_BLK cur_blk = ctx.panBlk[ctx.panIdx];
            ctx.panIdx ^= 1;  /* swap for next frame */
            ret = rga_pan_to_mmz(stMegaFrame.stVFrame.pMbBlk,
                                 w, h, crop_w, crop_h,
                                 ctx.voDispW, ctx.voDispH,
                                 frame, ctx.verbose,
                                 cur_blk, &pan_w, &pan_h);
            if (ret) {
                fprintf(stderr, "  rga_pan_to_mmz failed\n");
                RK_MPI_AVS_ReleaseChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stMegaFrame);
                continue;
            }
            send_mmz_to_vo(cur_blk, pan_w, pan_h,
                           ctx.voLayer, ctx.voChn, pts_us);
        } else {
            /* save/free: split на 2 половинки (как раньше) */
            int out_fds[2] = {-1, -1};
            void *out_vas[2] = {NULL, NULL};
            int out_w, out_h;
            ret = rga_split_to_dma(stMegaFrame.stVFrame.pMbBlk,
                                   w, h, ctx.width, ctx.height,
                                   ctx.mode, ctx.rotateCam,
                                   out_fds, out_vas, &out_w, &out_h, ctx.verbose);
            if (ret) {
                fprintf(stderr, "  rga_split_to_dma failed\n");
                RK_MPI_AVS_ReleaseChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stMegaFrame);
                continue;
            }
            osize = out_w * out_h * 3 / 2;
            int cam;
            switch (ctx.action) {
            case ACTION_SAVE:
                for (cam = 0; cam < NUM_SENSORS; cam++)
                    save_dma_to_file(out_vas[cam], osize, ctx.outputPrefix,
                                     cam, out_w, out_h, pts_us);
                break;
            case ACTION_FREE:
                break;
            default:
                break;
            }
            for (cam = 0; cam < NUM_SENSORS; cam++) {
                if (out_fds[cam] >= 0)
                    dma_buf_free(osize, &out_fds[cam], out_vas[cam]);
            }
        }

        RK_MPI_AVS_ReleaseChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stMegaFrame);
        long long t_act = get_now_ms() - t_act_start;
        if (ctx.verbose && (saved % 10 == 0))
            printf("  [act] frame %d: act=%lldms (rga+vo/save)\n", frame, t_act);
        saved++;
    }
    printf("Done: skipped=%d, processed=%d/%d (action=%s)\n",
           ctx.skipFrames, saved, ctx.frameCount, action_str);

cleanup_bind:
    for (int b = 0; b < 2; b++) {
        if (ctx.panBlk[b]) { RK_MPI_MMZ_Free(ctx.panBlk[b]); ctx.panBlk[b] = NULL; }
    }
    for (i = 0; i < NUM_SENSORS; i++) {
        MPP_CHN_S vi_chn, avs_in_chn;
        vi_chn.enModId = RK_ID_VI; vi_chn.s32DevId = i; vi_chn.s32ChnId = ctx.channelId;
        avs_in_chn.enModId = RK_ID_AVS; avs_in_chn.s32DevId = AVS_GRP_ID; avs_in_chn.s32ChnId = i;
        RK_MPI_SYS_UnBind(&vi_chn, &avs_in_chn);
    }
cleanup_avs_chn:
    RK_MPI_AVS_DisableChn(AVS_GRP_ID, AVS_CHN_ID);
cleanup_avs_grp:
    RK_MPI_AVS_StopGrp(AVS_GRP_ID);
    RK_MPI_AVS_DestroyGrp(AVS_GRP_ID);
cleanup_vo_layer:
    if (vo_inited) {
        RK_MPI_VO_DisableChn(ctx.voLayer, ctx.voChn);
        RK_MPI_VO_DisableLayer(ctx.voLayer);
        RK_MPI_VO_UnBindLayer(ctx.voLayer, ctx.voDevId);
    }
cleanup_vo_dev:
    if (vo_inited) RK_MPI_VO_Disable(ctx.voDevId);
cleanup_pipe:
    for (i = 0; i < NUM_SENSORS; i++) RK_MPI_VI_StopPipe(i);
cleanup_chn:
    for (i = 0; i < NUM_SENSORS; i++) RK_MPI_VI_DisableChnExt(i, ctx.channelId);
cleanup_dev:
    for (i = 0; i < NUM_SENSORS; i++) RK_MPI_VI_DisableDev(i);
cleanup_sys:
    RK_MPI_SYS_Exit();
    return ret;
}
