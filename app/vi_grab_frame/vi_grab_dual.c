/*
 * vi_grab_dual.c — CLI-программа для RV1126B: независимая работа с двух сенсоров
 *
 * Режимы (--action):
 *   save (default) — одновременный захват с двух камер (pthread_barrier sync),
 *                    сохранение пары кадров в файлы
 *   vo             — поочерёдный вывод на дисплей: RGA scale+rotate (1920x1080 →
 *                    720x1280 через rotate 90 + scale) → VO. Переключение камер
 *                    раз в секунду (--switch-interval сек, по умолчанию 1)
 *   free           — одновременный захват без сохранения (benchmark)
 *
 * Основано на rkipc dual_ipc (app/rkipc/src/rv1126b_dual_ipc/video/video.c)
 * VO init взято из vi_grab_avs_dma.c
 *
 * Использование:
 *   # Одновременный захват с двух сенсоров (1920x1080 каждый)
 *   ./vi_grab_dual -w 1920 -h 1080
 *   → sensor0_1920x1080_nv12.raw
 *   → sensor1_1920x1080_nv12.raw
 *
 *   # Вывод на дисплей с переключением камер раз в секунду
 *   ./vi_grab_dual -w 1920 -h 1080 --action vo --vo-dev 0 --vo-layer 1 --vo-chn 0 -n 1800
 *
 *   # 10 пар кадров
 *   ./vi_grab_dual -w 1920 -h 1080 -n 10
 *
 * Параметры:
 *   -w, --width0    ширина сенсора 0 (обязательно)
 *   -h, --height0   высота сенсора 0 (обязательно)
 *   -W, --width1    ширина сенсора 1 (по умолчанию = width0)
 *   -H, --height1   высота сенсора 1 (по умолчанию = height0)
 *   -c, --channel   VI channel id (по умолчанию 0)
 *   -o, --output    префикс имени файла (по умолчанию "sensor")
 *   -n, --count     сколько пар кадров (по умолчанию 1)
 *   -t, --timeout   таймаут GetChnFrame в мс (по умолчанию 1000)
 *   -v, --verbose   подробный вывод
 *   --action        save|vo|free (по умолчанию save)
 *   --vo-dev        VO device id (по умолчанию 0)
 *   --vo-layer      VO layer (по умолчанию 1)
 *   --vo-chn        VO channel (по умолчанию 0)
 *   --switch-interval  секунды между переключением камер в vo режиме (по умолчанию 1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>

#include "rk_defines.h"
#include "rk_debug.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_comm_vo.h"
#include "rk_comm_vpss.h"
#include "rk_comm_video.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_mmz.h"

/* RGA (2D hardware accelerator) for scale+rotate */
#include "im2d.h"
#include "rga.h"

#define DEFAULT_TIMEOUT_MS  1000
#define DEFAULT_CHANNEL_ID  0
#define DEFAULT_FRAME_COUNT 1
#define NUM_SENSORS         2
#define DEFAULT_RECT_INSET  50
#define DEFAULT_RECT_THICK  4
#define VPSS_GRP_BASE       2  /* VPSS group ids: 2,3 (avoid 0,1 reserved) */
#define VPSS_CHN_ID         0

/* Что делать с кадром */
typedef enum {
    ACTION_SAVE,   /* fwrite в файл */
    ACTION_VO,     /* отправить в VO (дисплей) */
    ACTION_FREE    /* просто освободить (benchmark) */
} action_t;

typedef struct {
    int width;
    int height;
    int devId;
    int pipeId;
    int channelId;
    int timeoutMs;
    int verbose;
    /* результаты */
    int got_frame;
    int frame_width;
    int frame_height;
    long long pts_us;
    int data_len;
    void *data;
    int save_ok;
    char saved_file[300];
} sensor_ctx_t;

typedef struct {
    sensor_ctx_t sensors[NUM_SENSORS];
    int frameCount;
    int verbose;
    char outputPrefix[128];
    pthread_barrier_t barrier;
    action_t action;
    int voDevId;
    int voLayer;
    int voChn;
    int voDispW;
    int voDispH;
    int switchInterval;  /* секунды между переключением камер */
    MB_BLK panBlk[2];     /* double-buffered MMZ for VO */
    int panIdx;
    int panBufSize;
    int rectEnabled;      /* --rect: рисовать рамку цвета камеры */
    int rectInset;        /* отступ рамки от края (по умолчанию 50) */
    int rectThick;        /* толщина рамки (по умолчанию 4) */
    int useVpss;          /* --vpss: VI → VPSS → GetChnFrame (вместо VI напрямую) */
    int vpssScale;        /* --vpss-scale: VPSS scale до pre-rotate размера дисплея */
    int vpssRotate;       /* --vpss-rotate: VPSS rotate 90 (без RGA) */
    int vpssOffline;      /* --vpss-offline: VPSS offline mode (SendFrame, rotate via GDC) */
    VIDEO_FRAME_INFO_S viHeldFrame[NUM_SENSORS]; /* VI frames held during offline VPSS */
} app_ctx_t;

static volatile int g_exit = 0;
static void sig_handler(int sig) { (void)sig; g_exit = 1; }

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -w <W0> -h <H0> [options]\n"
        "\n"
        "Independent two-sensor capture. Actions:\n"
        "  save — simultaneous grab both sensors (pthread sync), write files\n"
        "  vo   — alternate display: RGA scale+rotate → VO, switch cameras every N sec\n"
        "  free — simultaneous grab, no save (benchmark)\n"
        "\n"
        "Options:\n"
        "  -w, --width0 <W>        sensor 0 width (required)\n"
        "  -h, --height0 <H>       sensor 0 height (required)\n"
        "  -W, --width1 <W>        sensor 1 width (default: = width0)\n"
        "  -H, --height1 <H>       sensor 1 height (default: = height0)\n"
        "  -c, --channel <ID>      VI channel id (default: %d)\n"
        "  -o, --output <PFX>      output file prefix (default: \"sensor\")\n"
        "  -n, --count <N>         number of frame pairs (default: %d)\n"
        "  -t, --timeout <MS>      GetChnFrame timeout in ms (default: %d)\n"
        "  -v, --verbose           verbose output\n"
        "  --action <ACT>          save|vo|free (default: save)\n"
        "  --vo-dev <N>            VO device id for vo (default: 0)\n"
        "  --vo-layer <N>          VO layer for vo (default: 1)\n"
        "  --vo-chn <N>            VO channel for vo (default: 0)\n"
        "  --switch-interval <S>   seconds between camera switch in vo (default: 1)\n"
        "  --rect                  draw colored border rect (cam0=green, cam1=red)\n"
        "  --rect-inset <N>        rect inset from edge in px (default: %d)\n"
        "  --rect-thick <N>        rect border thickness in px (default: %d)\n"
        "  --vpss                  route VI → VPSS (passthrough) → GetChnFrame\n"
        "                          (default: VI directly, no VPSS)\n"
        "  --vpss-scale            with --vpss + --action vo: VPSS scales to\n"
        "                          pre-rotate display size (1280x720), RGA rotate only\n"
        "  --vpss-rotate           with --vpss: VPSS rotates 90° (no RGA needed)\n"
        "                          VPSS does scale+rotate, output = display size\n"
        "  --vpss-offline          with --vpss: VPSS offline mode via SendFrame\n"
        "                          (no VI bind, rotate via GDC, enables fan-out)\n"
        "  --help                  show this help\n"
        "\n"
        "Examples:\n"
        "  %s -w 1920 -h 1080\n"
        "  %s -w 1920 -h 1080 -n 10\n"
        "  %s -w 1920 -h 1080 --action vo --vo-dev 0 --vo-layer 1 --vo-chn 0 -n 1800\n"
        "  %s -w 1920 -h 1080 --action free -n 100\n",
        prog, DEFAULT_CHANNEL_ID, DEFAULT_FRAME_COUNT, DEFAULT_TIMEOUT_MS,
        DEFAULT_RECT_INSET, DEFAULT_RECT_THICK,
        prog, prog, prog, prog);
}

static int parse_args(app_ctx_t *app, int argc, char **argv) {
    static struct option long_opts[] = {
        {"width0",          required_argument, 0, 'w'},
        {"height0",         required_argument, 0, 'h'},
        {"width1",          required_argument, 0, 'W'},
        {"height1",         required_argument, 0, 'H'},
        {"channel",         required_argument, 0, 'c'},
        {"output",          required_argument, 0, 'o'},
        {"count",           required_argument, 0, 'n'},
        {"timeout",         required_argument, 0, 't'},
        {"verbose",         no_argument,       0, 'v'},
        {"action",          required_argument, 0, 1001},
        {"vo-dev",          required_argument, 0, 1002},
        {"vo-layer",        required_argument, 0, 1003},
        {"vo-chn",          required_argument, 0, 1004},
        {"switch-interval", required_argument, 0, 1005},
        {"rect",            no_argument,       0, 1006},
        {"rect-inset",      required_argument, 0, 1007},
        {"rect-thick",      required_argument, 0, 1008},
        {"vpss",            no_argument,       0, 1009},
        {"vpss-scale",      no_argument,       0, 1010},
        {"vpss-rotate",     no_argument,       0, 1011},
        {"vpss-offline",    no_argument,       0, 1012},
        {"help",            no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    int w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    int channel = DEFAULT_CHANNEL_ID;
    int count = DEFAULT_FRAME_COUNT;
    int timeout = DEFAULT_TIMEOUT_MS;
    int verbose = 0;
    char prefix[128] = "sensor";
    action_t action = ACTION_SAVE;
    int voDev = 0, voLayer = 1, voChn = 0;
    int switchInt = 1;
    int rectEn = 0, rectInset = DEFAULT_RECT_INSET, rectThick = DEFAULT_RECT_THICK;
    int useVpss = 0;
    int vpssScale = 0;
    int vpssRotate = 0;
    int vpssOffline = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "w:h:W:H:c:o:n:t:v", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'w': w0 = atoi(optarg); break;
            case 'h': h0 = atoi(optarg); break;
            case 'W': w1 = atoi(optarg); break;
            case 'H': h1 = atoi(optarg); break;
            case 'c': channel = atoi(optarg); break;
            case 'o': strncpy(prefix, optarg, sizeof(prefix) - 1); break;
            case 'n': count = atoi(optarg); break;
            case 't': timeout = atoi(optarg); break;
            case 'v': verbose = 1; break;
            case 1001:
                if (strcmp(optarg, "vo") == 0) action = ACTION_VO;
                else if (strcmp(optarg, "free") == 0) action = ACTION_FREE;
                else action = ACTION_SAVE;
                break;
            case 1002: voDev = atoi(optarg); break;
            case 1003: voLayer = atoi(optarg); break;
            case 1004: voChn = atoi(optarg); break;
            case 1005: switchInt = atoi(optarg); break;
            case 1006: rectEn = 1; break;
            case 1007: rectInset = atoi(optarg); break;
            case 1008: rectThick = atoi(optarg); break;
            case 1009: useVpss = 1; break;
            case 1010: vpssScale = 1; break;
            case 1011: vpssRotate = 1; break;
            case 1012: vpssOffline = 1; useVpss = 1; break;  /* offline implies --vpss */
            case '?':
            default:
                usage(argv[0]);
                return -1;
        }
    }

    if (w0 <= 0 || h0 <= 0) {
        fprintf(stderr, "Error: width0 and height0 are required\n\n");
        usage(argv[0]);
        return -1;
    }
    if (w1 <= 0) w1 = w0;
    if (h1 <= 0) h1 = h0;

    memset(app, 0, sizeof(*app));
    for (int i = 0; i < NUM_SENSORS; i++) {
        app->sensors[i].devId = i;
        app->sensors[i].pipeId = i;
        app->sensors[i].channelId = channel;
        app->sensors[i].timeoutMs = timeout;
        app->sensors[i].verbose = verbose;
    }
    app->sensors[0].width = w0;
    app->sensors[0].height = h0;
    app->sensors[1].width = w1;
    app->sensors[1].height = h1;
    app->frameCount = count;
    app->verbose = verbose;
    strncpy(app->outputPrefix, prefix, sizeof(app->outputPrefix) - 1);
    app->action = action;
    app->voDevId = voDev;
    app->voLayer = voLayer;
    app->voChn = voChn;
    app->switchInterval = switchInt;
    app->rectEnabled = rectEn;
    app->rectInset = rectInset;
    app->rectThick = rectThick;
    app->useVpss = useVpss;
    app->vpssScale = vpssScale;
    app->vpssRotate = vpssRotate;
    app->vpssOffline = vpssOffline;

    return 0;
}

static long long get_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Аргумент потока: pack {app_ctx_t*, sensor index} */
typedef struct {
    app_ctx_t *app;
    int sensor_idx;
} thread_arg_t;

/* Forward declarations (vpss_init/deinit defined later) */
static int get_frame(app_ctx_t *app, int sensor_idx, VIDEO_FRAME_INFO_S *vf, int timeout_ms);
static int release_frame(app_ctx_t *app, int sensor_idx, VIDEO_FRAME_INFO_S *vf);

/* Поток захвата одного сенсора (для save/free — одновременный захват) */
static void *grab_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    app_ctx_t *app = targ->app;
    sensor_ctx_t *s = &app->sensors[targ->sensor_idx];
    int ret;
    VIDEO_FRAME_INFO_S stViFrame;
    memset(&stViFrame, 0, sizeof(stViFrame));

    /* Ждём остальные потоки, чтобы вызвать GetChnFrame одновременно */
    pthread_barrier_wait(&app->barrier);

    ret = get_frame(app, targ->sensor_idx, &stViFrame, s->timeoutMs);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "[sensor %d] GetChnFrame failed: %#x\n", s->devId, ret);
        s->got_frame = 0;
        return NULL;
    }

    s->got_frame = 1;
    s->frame_width = stViFrame.stVFrame.u32Width;
    s->frame_height = stViFrame.stVFrame.u32Height;
    s->pts_us = (long long)stViFrame.stVFrame.u64PTS;
    s->data_len = RK_MPI_MB_GetLength(stViFrame.stVFrame.pMbBlk);
    s->data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);

    if (s->verbose) {
        fprintf(stderr, "[sensor %d] frame: %dx%d pts=%lldus len=%d\n",
                s->devId, s->frame_width, s->frame_height, s->pts_us, s->data_len);
    }

    /* Сохранение в файл (имя включает PTS) — только для ACTION_SAVE */
    if (app->action == ACTION_SAVE) {
        snprintf(s->saved_file, sizeof(s->saved_file), "%s%d_%dx%d_pts%lld_nv12.raw",
                 app->outputPrefix, s->devId, s->frame_width, s->frame_height, s->pts_us);
        FILE *fp = fopen(s->saved_file, "wb");
        if (!fp) {
            fprintf(stderr, "[sensor %d] cannot open %s\n", s->devId, s->saved_file);
            s->save_ok = 0;
        } else {
            int expected = s->frame_width * s->frame_height * 3 / 2;
            int to_write = (s->data_len > 0) ? s->data_len : expected;
            size_t written = fwrite(s->data, 1, to_write, fp);
            fclose(fp);
            s->save_ok = 1;
            if (s->verbose)
                fprintf(stderr, "[sensor %d] saved %zu bytes to %s (PTS=%lldus)\n",
                        s->devId, written, s->saved_file, s->pts_us);
        }
    }

    /* Освобождение кадра */
    ret = release_frame(app, targ->sensor_idx, &stViFrame);
    if (ret != RK_SUCCESS)
        fprintf(stderr, "[sensor %d] ReleaseChnFrame failed: %#x\n", s->devId, ret);

    return NULL;
}

/*
 * rga_scale_rotate_to_mmz — scale+rotate: src (WxH NV12) → dst (dst_w x dst_h BGRA8888)
 * rotate 90 + scale в одной операции. Без crop — пропорции 16:9 → 9:16 совпадают.
 * src — rockit dmabuf, dst — MMZ буфер (MB_BLK).
 */
static rga_buffer_t pat_dummy(void) {
    rga_buffer_t p;
    memset(&p, 0, sizeof(p));
    return p;
}

static int rga_process_to_mmz(MB_BLK src_mb, int src_w, int src_h,
                               int src_vw, int src_vh,
                               int dst_w, int dst_h, int verbose,
                               MB_BLK dst_blk, int rotate_90) {
    int src_fd = RK_MPI_MB_Handle2Fd(src_mb);
    int dst_fd = RK_MPI_MB_Handle2Fd(dst_blk);
    if (src_fd < 0 || dst_fd < 0) {
        fprintf(stderr, "[rga] Handle2Fd failed (src=%d dst=%d)\n", src_fd, dst_fd);
        return -1;
    }

    rga_buffer_t src = wrapbuffer_fd_t(src_fd, src_w, src_h,
                                       src_vw, src_vh, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_fd_t(dst_fd, dst_w, dst_h, dst_w, dst_h,
                                       RK_FORMAT_BGRA_8888);

    im_rect srect = { .width = src_w, .height = src_h };
    im_rect drect = { .width = dst_w, .height = dst_h };
    im_rect prect = {0};

    int usage = (rotate_90 ? IM_HAL_TRANSFORM_ROT_90 : 0) | IM_SYNC;
    IM_STATUS st = improcess(src, dst, pat_dummy(), srect, drect, prect, usage);
    if (st != IM_STATUS_SUCCESS) {
        fprintf(stderr, "[rga] improcess failed: %d (src=%dx%d → %dx%d rot=%d)\n",
                (int)st, src_w, src_h, dst_w, dst_h, rotate_90);
        return -1;
    }

    if (verbose)
        printf("  [rga] %s: %dx%d(vir %dx%d) → %dx%d rot=%d\n",
               rotate_90 ? "scale+rotate" : "scale",
               src_w, src_h, src_vw, src_vh, dst_w, dst_h, rotate_90);
    return 0;
}

static int rga_scale_rotate_to_mmz(MB_BLK src_mb, int src_w, int src_h,
                                    int src_vw, int src_vh,
                                    int dst_w, int dst_h, int verbose,
                                    MB_BLK dst_blk) {
    return rga_process_to_mmz(src_mb, src_w, src_h, src_vw, src_vh,
                              dst_w, dst_h, verbose, dst_blk, 1);
}

static int rga_scale_to_mmz(MB_BLK src_mb, int src_w, int src_h,
                             int src_vw, int src_vh,
                             int dst_w, int dst_h, int verbose,
                             MB_BLK dst_blk) {
    return rga_process_to_mmz(src_mb, src_w, src_h, src_vw, src_vh,
                              dst_w, dst_h, verbose, dst_blk, 0);
}

/* Отправить MMZ буфер в VO (дисплей) */
static int send_mmz_to_vo(MB_BLK blk, int w, int h,
                          int vo_layer, int vo_chn, long long pts) {
    VIDEO_FRAME_INFO_S vf;
    memset(&vf, 0, sizeof(vf));
    vf.stVFrame.pMbBlk        = blk;
    vf.stVFrame.u32Width      = w;
    vf.stVFrame.u32Height     = h;
    vf.stVFrame.u32VirWidth   = w;
    vf.stVFrame.u32VirHeight  = h;
    vf.stVFrame.enPixelFormat = RK_FMT_BGRA8888;
    vf.stVFrame.u64PTS        = pts;

    RK_S32 ret = RK_MPI_VO_SendFrame(vo_layer, vo_chn, &vf, 1000);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "[vo] VO_SendFrame failed: %#x\n", ret);
        return -1;
    }
    return 0;
}

/*
 * draw_rect — нарисовать рамку через RGA (hardware, без CPU).
 * cam_idx=0 → зелёный, cam_idx=1 → красный.
 * Рамка толщиной thickness пикселей, отступ inset от края.
 * Использует imfill_t (C API) — 4 заполненных прямоугольника (стороны рамки).
 * Цвет в формате 0xAABBGGRR (imfill_t для BGRA8888).
 */
static int draw_rect(MB_BLK blk, int w, int h, int inset, int thickness, int cam_idx) {
    int dst_fd = RK_MPI_MB_Handle2Fd(blk);
    if (dst_fd < 0) {
        fprintf(stderr, "[rect] Handle2Fd failed\n");
        return -1;
    }
    rga_buffer_t dst = wrapbuffer_fd_t(dst_fd, w, h, w, h, RK_FORMAT_BGRA_8888);

    if (thickness < 1) thickness = 1;
    int x0 = inset, y0 = inset;
    int rw = w - inset * 2, rh = h - inset * 2;
    if (rw < thickness * 2 || rh < thickness * 2) return -1;

    /* Для BGRA8888: пробуем 0xAABBGGRR (alpha в старшем байте).
     * cam0=green: A=255,B=0,G=255,R=0 → 0xFF00FF00
     * cam1=red:   A=255,B=0,G=0,R=255 → 0xFF0000FF */
    uint32_t color = (cam_idx == 0) ? 0xFF00FF00 : 0xFF0000FF;

    /* 4 стороны рамки как заполненные прямоугольники */
    im_rect sides[4] = {
        { x0, y0, rw, thickness },                          /* top */
        { x0, y0 + rh - thickness, rw, thickness },        /* bottom */
        { x0, y0, thickness, rh },                        /* left */
        { x0 + rw - thickness, y0, thickness, rh },       /* right */
    };

    for (int i = 0; i < 4; i++) {
        IM_STATUS st = imfill_t(dst, sides[i], (int)color, 1);
        if (st != IM_STATUS_SUCCESS) {
            fprintf(stderr, "[rect] imfill_t[%d] failed: %d\n", i, (int)st);
            return -1;
        }
    }
    return 0;
}

/*
 * VPSS init: создать по одной группе на сенсор.
 * При --vpss-scale + ACTION_VO: VPSS scale до pre-rotate размера (displayH × displayW = 1280×720),
 *   RGA потом rotate 90 → displayW × displayH (720×1280 portrait).
 * Иначе: passthrough (sensor size, без scale).
 * GetChnFrame делается из VPSS вместо VI.
 */
static int vpss_init(app_ctx_t *app) {
    for (int i = 0; i < NUM_SENSORS; i++) {
        int grp = VPSS_GRP_BASE + i;
        /* При --vpss-rotate + ACTION_VO: VPSS rotate 90° без scale.
         * Тест: оставляем sensor size (1920×1080), VPSS должен крутить.
         * RGA потом scale → display (720×1280).
         * При --vpss-scale + ACTION_VO: VPSS scale до pre-rotate (1280×720), RGA rotate.
         * Иначе: passthrough (sensor size). */
        int chn_w = app->sensors[i].width;
        int chn_h = app->sensors[i].height;
        if (app->vpssScale && app->action == ACTION_VO && app->voDispW > 0) {
            chn_w = app->voDispH;  /* 1280 (landscape pre-rotate) */
            chn_h = app->voDispW;  /* 720  (landscape pre-rotate) */
        }
        /* Offline + rotate: VPSS output is rotated (W↔H swapped).
         * If rotate 90° on 1920×1080 input → output 1080×1920.
         * But VPSS may handle swap internally — test both ways. */
        if (app->vpssOffline && app->vpssRotate) {
            /* Don't swap — let VPSS handle it internally via SetGrpRotation */
        }

        VPSS_GRP_ATTR_S gattr;
        memset(&gattr, 0, sizeof(gattr));
        /* maxW/maxH must accommodate both input AND output (rotated) dimensions */
        gattr.u32MaxW = app->sensors[i].width > chn_w ? app->sensors[i].width : chn_w;
        gattr.u32MaxH = app->sensors[i].height > chn_h ? app->sensors[i].height : chn_h;
        gattr.enPixelFormat = RK_FMT_YUV420SP;
        gattr.stFrameRate.s32SrcFrameRate = -1;
        gattr.stFrameRate.s32DstFrameRate = -1;
        gattr.enCompressMode = COMPRESS_MODE_NONE;
        gattr.enVProcDev = VIDEO_PROC_DEV_VPSS;  /* also set via SetVProcDev after StartGrp */

        int ret = RK_MPI_VPSS_CreateGrp(grp, &gattr);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: CreateGrp[%d] failed: %#x\n", grp, ret);
            return -1;
        }

        VPSS_CHN_ATTR_S cattr;
        memset(&cattr, 0, sizeof(cattr));
        cattr.enChnMode = VPSS_CHN_MODE_USER;
        cattr.enDynamicRange = DYNAMIC_RANGE_SDR8;
        /* При --vpss-rotate: VO+RGA конвертирует NV12→BGRA, VPSS отдаёт NV12.
         * Иначе: NV12 (RGA потом конвертирует). */
        cattr.enPixelFormat = RK_FMT_YUV420SP;
        cattr.stFrameRate.s32SrcFrameRate = -1;
        cattr.stFrameRate.s32DstFrameRate = -1;
        cattr.u32Width = chn_w;
        cattr.u32Height = chn_h;
        cattr.enCompressMode = COMPRESS_MODE_NONE;
        cattr.u32FrameBufCnt = 4;
        cattr.u32Depth = 2;

        ret = RK_MPI_VPSS_SetChnAttr(grp, VPSS_CHN_ID, &cattr);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: SetChnAttr[%d] failed: %#x\n", grp, ret);
            return -1;
        }
        ret = RK_MPI_VPSS_EnableChn(grp, VPSS_CHN_ID);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: EnableChn[%d] failed: %#x\n", grp, ret);
            return -1;
        }

        ret = RK_MPI_VPSS_StartGrp(grp);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: StartGrp[%d] failed: %#x\n", grp, ret);
            return -1;
        }

        /* SetVProcDev AFTER StartGrp (per SDK example test_mod_vpss.cpp) */
        ret = RK_MPI_VPSS_SetVProcDev(grp, VIDEO_PROC_DEV_VPSS);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: SetVProcDev[%d] failed: %#x\n", grp, ret);
            return -1;
        }

        /* VPSS rotate 90° — AFTER StartGrp (per SDK example).
         * In offline mode rotate works via GDC.
         * Use SetGrpRotation (input rotate) + SetChnRotation (output rotate). */
        if (app->vpssRotate) {
            ret = RK_MPI_VPSS_SetGrpRotation(grp, ROTATION_90);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "vpss: SetGrpRotation[%d] failed: %#x\n", grp, ret);
                return -1;
            }
            if (app->verbose)
                printf("vpss: grp[%d] GrpRotation=90 enabled\n", grp);
            ret = RK_MPI_VPSS_SetChnRotation(grp, VPSS_CHN_ID, ROTATION_90);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "vpss: SetChnRotation[%d] failed: %#x\n", grp, ret);
                return -1;
            }
            if (app->verbose)
                printf("vpss: grp[%d] ChnRotation=90 enabled\n", grp);
        }

        if (app->vpssOffline) {
            /* Offline mode: NO bind with VI. Frames fed via SendFrame.
             * Rotate works in offline mode via GDC. */
            if (app->verbose)
                printf("vpss: grp[%d] OFFLINE mode (no VI bind, SendFrame), out %dx%d\n",
                        grp, chn_w, chn_h);
        } else {
            /* Online mode: Bind VI[pipeId, chnId] → VPSS[grp, 0] */
            MPP_CHN_S vi_chn, vpss_in;
            vi_chn.enModId = RK_ID_VI;
            vi_chn.s32DevId = app->sensors[i].devId;
            vi_chn.s32ChnId = app->sensors[i].channelId;
            vpss_in.enModId = RK_ID_VPSS;
            vpss_in.s32DevId = grp;
            vpss_in.s32ChnId = 0;
            ret = RK_MPI_SYS_Bind(&vi_chn, &vpss_in);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "vpss: Bind VI[%d] -> VPSS[%d] failed: %#x\n",
                        app->sensors[i].devId, grp, ret);
                return -1;
            }
            if (app->verbose)
                printf("vpss: grp[%d] OK, VI[%d,%d] -> VPSS[%d,0] bound (out %dx%d)\n",
                        grp, app->sensors[i].devId, app->sensors[i].channelId,
                        grp, chn_w, chn_h);
        }
    }
    return 0;
}

static void vpss_deinit(app_ctx_t *app) {
    for (int i = 0; i < NUM_SENSORS; i++) {
        int grp = VPSS_GRP_BASE + i;
        if (!app->vpssOffline) {
            MPP_CHN_S vi_chn, vpss_in;
            vi_chn.enModId = RK_ID_VI;
            vi_chn.s32DevId = app->sensors[i].devId;
            vi_chn.s32ChnId = app->sensors[i].channelId;
            vpss_in.enModId = RK_ID_VPSS;
            vpss_in.s32DevId = grp;
            vpss_in.s32ChnId = 0;
            RK_MPI_SYS_UnBind(&vi_chn, &vpss_in);
        }
        RK_MPI_VPSS_DisableChn(grp, VPSS_CHN_ID);
        RK_MPI_VPSS_StopGrp(grp);
        RK_MPI_VPSS_DestroyGrp(grp);
    }
}

/* GetChnFrame: из VPSS если --vpss, иначе из VI.
 * При --vpss-offline: VI_GetChnFrame → VPSS_SendFrame → VPSS_GetChnFrame.
 * VI кадр держится в app->viHeldFrame до release_frame. */
static int get_frame(app_ctx_t *app, int sensor_idx, VIDEO_FRAME_INFO_S *vf, int timeout_ms) {
    if (app->vpssOffline) {
        int grp = VPSS_GRP_BASE + sensor_idx;
        VIDEO_FRAME_INFO_S *viF = &app->viHeldFrame[sensor_idx];
        memset(viF, 0, sizeof(*viF));
        int ret = RK_MPI_VI_GetChnFrame(app->sensors[sensor_idx].pipeId,
                                         app->sensors[sensor_idx].channelId,
                                         viF, timeout_ms);
        if (ret != RK_SUCCESS) {
            if (app->verbose) fprintf(stderr, "  [offline] VI_GetChnFrame cam%d failed: %#x\n", sensor_idx, ret);
            return ret;
        }
        /* Flush cache before SendFrame (per SDK example test_mod_vpss.cpp:328) */
        RK_MPI_SYS_MmzFlushCache(viF->stVFrame.pMbBlk, RK_FALSE);
        ret = RK_MPI_VPSS_SendFrame(grp, 0, viF, timeout_ms);
        if (ret != RK_SUCCESS) {
            if (app->verbose) fprintf(stderr, "  [offline] VPSS_SendFrame grp%d failed: %#x\n", grp, ret);
            RK_MPI_VI_ReleaseChnFrame(app->sensors[sensor_idx].pipeId,
                                      app->sensors[sensor_idx].channelId, viF);
            return ret;
        }
        ret = RK_MPI_VPSS_GetChnFrame(grp, VPSS_CHN_ID, vf, timeout_ms);
        if (ret != RK_SUCCESS) {
            if (app->verbose) fprintf(stderr, "  [offline] VPSS_GetChnFrame grp%d chn%d failed: %#x\n", grp, VPSS_CHN_ID, ret);
            RK_MPI_VI_ReleaseChnFrame(app->sensors[sensor_idx].pipeId,
                                      app->sensors[sensor_idx].channelId, viF);
            return ret;
        }
        return RK_SUCCESS;
    }
    if (app->useVpss) {
        int grp = VPSS_GRP_BASE + sensor_idx;
        return RK_MPI_VPSS_GetChnFrame(grp, VPSS_CHN_ID, vf, timeout_ms);
    }
    return RK_MPI_VI_GetChnFrame(app->sensors[sensor_idx].pipeId,
                                  app->sensors[sensor_idx].channelId,
                                  vf, timeout_ms);
}

static int release_frame(app_ctx_t *app, int sensor_idx, VIDEO_FRAME_INFO_S *vf) {
    if (app->vpssOffline) {
        int grp = VPSS_GRP_BASE + sensor_idx;
        int ret = RK_MPI_VPSS_ReleaseChnFrame(grp, VPSS_CHN_ID, vf);
        RK_MPI_VI_ReleaseChnFrame(app->sensors[sensor_idx].pipeId,
                                   app->sensors[sensor_idx].channelId,
                                   &app->viHeldFrame[sensor_idx]);
        return ret;
    }
    if (app->useVpss) {
        int grp = VPSS_GRP_BASE + sensor_idx;
        return RK_MPI_VPSS_ReleaseChnFrame(grp, VPSS_CHN_ID, vf);
    }
    return RK_MPI_VI_ReleaseChnFrame(app->sensors[sensor_idx].pipeId,
                                      app->sensors[sensor_idx].channelId, vf);
}

/* Инициализация VO (DSI display 720x1280, layer 1, VO_INTF_MIPI) */
static int vo_init(app_ctx_t *app) {
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

    RK_S32 ret = RK_MPI_VO_BindLayer(app->voLayer, app->voDevId, VO_LAYER_MODE_GRAPHIC);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_BindLayer: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetPubAttr(app->voDevId, &VoPubAttr);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetPubAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_Enable(app->voDevId);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_Enable: %#x\n", ret); return -1; }

    /* Get real display dimensions from driver */
    ret = RK_MPI_VO_GetPubAttr(app->voDevId, &VoPubAttr);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_GetPubAttr: %#x\n", ret); return -1; }
    if (VoPubAttr.stSyncInfo.u16Hact == 0 || VoPubAttr.stSyncInfo.u16Vact == 0) {
        VoPubAttr.stSyncInfo.u16Hact = 720;
        VoPubAttr.stSyncInfo.u16Vact = 1280;
    }
    app->voDispW = VoPubAttr.stSyncInfo.u16Hact;
    app->voDispH = VoPubAttr.stSyncInfo.u16Vact;
    printf("VO: display %dx%d (from driver)\n", app->voDispW, app->voDispH);

    stLayerAttr.stDispRect.s32X = 0;
    stLayerAttr.stDispRect.s32Y = 0;
    stLayerAttr.stDispRect.u32Width = app->voDispW;
    stLayerAttr.stDispRect.u32Height = app->voDispH;
    stLayerAttr.stImageSize.u32Width = app->voDispW;
    stLayerAttr.stImageSize.u32Height = app->voDispH;
    stLayerAttr.u32DispFrmRt = 30;
    stLayerAttr.enPixFormat = RK_FMT_BGRA8888;
    VideoCSC.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
    VideoCSC.u32Contrast = 50;
    VideoCSC.u32Hue = 50;
    VideoCSC.u32Luma = 50;
    VideoCSC.u32Satuature = 50;

    ret = RK_MPI_VO_SetLayerAttr(app->voLayer, &stLayerAttr);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetLayerAttr: %#x\n", ret); return -1; }
    stLayerAttr.bBypassFrame = RK_FALSE;
    ret = RK_MPI_VO_SetLayerAttr(app->voLayer, &stLayerAttr);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetLayerAttr bypass: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableLayer(app->voLayer);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_EnableLayer: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_SetLayerCSC(app->voLayer, &VideoCSC);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetLayerCSC: %#x\n", ret); return -1; }

    VoChnAttr.bDeflicker = RK_FALSE;
    VoChnAttr.u32Priority = 1;
    VoChnAttr.stRect.s32X = 0;
    VoChnAttr.stRect.s32Y = 0;
    VoChnAttr.stRect.u32Width = app->voDispW;
    VoChnAttr.stRect.u32Height = app->voDispH;
    ret = RK_MPI_VO_SetChnAttr(app->voLayer, app->voChn, &VoChnAttr);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_SetChnAttr: %#x\n", ret); return -1; }
    ret = RK_MPI_VO_EnableChn(app->voLayer, app->voChn);
    if (ret != RK_SUCCESS) { fprintf(stderr, "VO_EnableChn: %#x\n", ret); return -1; }

    printf("VO: layer=%d dev=%d chn=%d enabled (%dx%d DSI)\n",
           app->voLayer, app->voDevId, app->voChn, app->voDispW, app->voDispH);
    return 0;
}

static void vo_deinit(app_ctx_t *app) {
    RK_MPI_VO_DisableChn(app->voLayer, app->voChn);
    RK_MPI_VO_DisableLayer(app->voLayer);
    RK_MPI_VO_UnBindLayer(app->voLayer, app->voDevId);
    RK_MPI_VO_Disable(app->voDevId);
}

int main(int argc, char **argv) {
    app_ctx_t app;
    int ret, i, frame;

    if (parse_args(&app, argc, argv) != 0)
        return 1;

    const char *action_str = (app.action == ACTION_VO) ? "vo" :
                             (app.action == ACTION_FREE) ? "free" : "save";
    printf("vi_grab_dual: sensor0=%dx%d, sensor1=%dx%d, chn=%d, frames=%d, action=%s\n",
           app.sensors[0].width, app.sensors[0].height,
           app.sensors[1].width, app.sensors[1].height,
           app.sensors[0].channelId, app.frameCount, action_str);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* 1. Инициализация MPI */
    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_SYS_Init failed: %#x\n", ret);
        return 1;
    }
    if (app.verbose) printf("RK_MPI_SYS_Init OK\n");

    /* 2. Настройка VI устройств (как в rkipc dual vi_dev_init) */
    for (i = 0; i < NUM_SENSORS; i++) {
        VI_DEV_ATTR_S stDevAttr;
        VI_DEV_BIND_PIPE_S stBindPipe;
        memset(&stDevAttr, 0, sizeof(stDevAttr));
        memset(&stBindPipe, 0, sizeof(stBindPipe));

        ret = RK_MPI_VI_GetDevAttr(i, &stDevAttr);
        if (ret == RK_ERR_VI_NOT_CONFIG) {
            ret = RK_MPI_VI_SetDevAttr(i, &stDevAttr);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "dev %d: RK_MPI_VI_SetDevAttr failed: %#x\n", i, ret);
                goto cleanup_sys;
            }
            if (app.verbose) printf("dev %d: SetDevAttr OK\n", i);
        } else {
            if (app.verbose) printf("dev %d: already configured\n", i);
        }

        ret = RK_MPI_VI_GetDevIsEnable(i);
        if (ret != RK_SUCCESS) {
            ret = RK_MPI_VI_EnableDev(i);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "dev %d: RK_MPI_VI_EnableDev failed: %#x\n", i, ret);
                goto cleanup_dev;
            }
            if (app.verbose) printf("dev %d: EnableDev OK\n", i);

            stBindPipe.u32Num = 1;
            stBindPipe.PipeId[0] = i;
            stBindPipe.bUserStartPipe[0] = RK_TRUE;
            ret = RK_MPI_VI_SetDevBindPipe(i, &stBindPipe);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "dev %d: SetDevBindPipe failed: %#x\n", i, ret);
                goto cleanup_dev;
            }
            if (app.verbose) printf("dev %d: SetDevBindPipe OK\n", i);
        } else {
            if (app.verbose) printf("dev %d: already enabled\n", i);
        }
    }

    /* 3. Настройка VI каналов для каждого сенсора */
    for (i = 0; i < NUM_SENSORS; i++) {
        VI_CHN_ATTR_S vi_chn_attr;
        memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
        vi_chn_attr.stIspOpt.u32BufCount = 3;
        vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
        vi_chn_attr.stIspOpt.stMaxSize.u32Width = app.sensors[i].width;
        vi_chn_attr.stIspOpt.stMaxSize.u32Height = app.sensors[i].height;
        vi_chn_attr.stSize.u32Width = app.sensors[i].width;
        vi_chn_attr.stSize.u32Height = app.sensors[i].height;
        vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
        vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
        vi_chn_attr.u32Depth = 1;

        ret = RK_MPI_VI_SetChnAttr(app.sensors[i].pipeId, app.sensors[i].channelId, &vi_chn_attr);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "sensor %d: SetChnAttr failed: %#x\n", i, ret);
            goto cleanup_dev;
        }
        ret = RK_MPI_VI_EnableChnExt(app.sensors[i].pipeId, app.sensors[i].channelId);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "sensor %d: EnableChnExt failed: %#x\n", i, ret);
            goto cleanup_chn;
        }
        if (app.verbose) printf("sensor %d: chn %d EnableChnExt OK (%dx%d)\n",
                                i, app.sensors[i].channelId,
                                app.sensors[i].width, app.sensors[i].height);
    }

    /* group mode: все каналы должны быть готовы до StartPipe */
    for (i = 0; i < NUM_SENSORS; i++) {
        ret = RK_MPI_VI_StartPipe(app.sensors[i].pipeId);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "pipe %d: StartPipe failed: %#x\n", i, ret);
            goto cleanup_chn;
        }
        if (app.verbose) printf("pipe %d: StartPipe OK\n", i);
    }

    /* 3b. VPSS init (only for --vpss) */
    int vpss_inited = 0;
    int vo_inited = 0;

    /* 4a. VO init (only for --action vo) — ДО VPSS, чтобы знать display size */
    if (app.action == ACTION_VO) {
        if (vo_init(&app) != 0) {
            fprintf(stderr, "VO init failed\n");
            goto cleanup_chn;
        }
        vo_inited = 1;
    }

    if (app.useVpss) {
        if (vpss_init(&app) != 0) {
            fprintf(stderr, "VPSS init failed\n");
            goto cleanup_vo;
        }
        vpss_inited = 1;
        if (app.action == ACTION_VO && app.vpssRotate)
            printf("Pipeline: VI×2 → VPSS×2 (rotate 90° passthrough %dx%d) → RGA scale → VO %dx%d\n",
                   app.sensors[0].width, app.sensors[0].height,
                   app.voDispW, app.voDispH);
        else if (app.action == ACTION_VO && app.vpssScale)
            printf("Pipeline: VI×2 → VPSS×2 (scale→%dx%d) → RGA rotate → VO %dx%d\n",
                   app.voDispH, app.voDispW, app.voDispW, app.voDispH);
        else
            printf("Pipeline: VI×2 → VPSS×2 (passthrough %dx%d) → %s\n",
                   app.sensors[0].width, app.sensors[0].height,
                   (app.action == ACTION_VO) ? "RGA scale+rotate → VO" : "save/free");
    } else if (app.verbose) {
        printf("Pipeline: VI×2 (direct, no VPSS) → %s\n",
               (app.action == ACTION_VO) ? "RGA scale+rotate → VO" : "save/free");
    }

    /* 4b. Захват кадров */
    if (app.action == ACTION_VO) {
        /* ---- VO mode: поочерёдный вывод камер на дисплей ---- */
        if (!app.useVpss)
            printf("Pipeline: VI×2 (independent) → RGA scale+rotate → VO (display %dx%d)\n",
                   app.voDispW, app.voDispH);
        printf("Switching cameras every %d sec\n", app.switchInterval);

        /* Allocate double-buffered MMZ (VO holds one, we fill other) */
        int psize = app.voDispW * app.voDispH * 4;  /* BGRA8888 */
        for (int b = 0; b < 2; b++) {
            RK_S32 rc = RK_MPI_MMZ_Alloc(&app.panBlk[b], psize, 0);
            if (rc != RK_SUCCESS) {
                fprintf(stderr, "MMZ_Alloc pan[%d] failed: %#x\n", b, rc);
                goto cleanup_vo;
            }
        }
        app.panBufSize = psize;
        app.panIdx = 0;

        int cur_cam = 0;
        long long last_switch = get_now_ms();
        int switch_ms = app.switchInterval * 1000;
        int total = app.frameCount;
        int saved = 0;

        for (frame = 0; frame < total && !g_exit; frame++) {
            /* Переключение камеры по времени */
            long long now = get_now_ms();
            if (now - last_switch >= switch_ms) {
                cur_cam ^= 1;
                last_switch = now;
                printf("Frame %d: switch → cam%d\n", frame, cur_cam);
            }

            VIDEO_FRAME_INFO_S stViFrame;
            memset(&stViFrame, 0, sizeof(stViFrame));
            long long t_start = get_now_ms();
            ret = get_frame(&app, cur_cam, &stViFrame, app.sensors[cur_cam].timeoutMs);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "Frame %d: cam%d GetChnFrame failed: %#x\n", frame, cur_cam, ret);
                continue;
            }
            long long t_grab = get_now_ms() - t_start;
            int w = stViFrame.stVFrame.u32Width;
            int h = stViFrame.stVFrame.u32Height;
            int vw = stViFrame.stVFrame.u32VirWidth;
            int vh = stViFrame.stVFrame.u32VirHeight;
            long long pts_us = (long long)stViFrame.stVFrame.u64PTS;

            /* RGA scale+rotate → MMZ.
             * VPSS rotate НЕ работает на RV1126B (доказано тестом — API
             * возвращает success но rotate не применяется, даже в offline mode).
             * Поэтому RGA всегда делает и scale, и rotate. */
            MB_BLK cur_blk = app.panBlk[app.panIdx];
            app.panIdx ^= 1;
            ret = rga_scale_rotate_to_mmz(stViFrame.stVFrame.pMbBlk, w, h,
                                          vw, vh,
                                          app.voDispW, app.voDispH,
                                          app.verbose, cur_blk);
            if (ret) {
                fprintf(stderr, "  rga failed\n");
                release_frame(&app, cur_cam, &stViFrame);
                continue;
            }

            /* Опциональная рамка цвета камеры (--rect) */
            if (app.rectEnabled) {
                draw_rect(cur_blk, app.voDispW, app.voDispH,
                          app.rectInset, app.rectThick, cur_cam);
            }

            /* Отправить в VO */
            send_mmz_to_vo(cur_blk, app.voDispW, app.voDispH,
                           app.voLayer, app.voChn, pts_us);

            release_frame(&app, cur_cam, &stViFrame);

            if (app.verbose && (saved % 30 == 0))
                printf("Frame %d [cam%d]: %dx%d pts=%lldus grab=%lldms → VO %dx%d\n",
                       frame, cur_cam, w, h, pts_us, t_grab, app.voDispW, app.voDispH);
            saved++;
        }
        printf("Done: processed=%d/%d (action=vo)\n", saved, total);
    } else {
        /* ---- save/free mode: одновременный захват с двух сенсоров ---- */
        pthread_barrier_init(&app.barrier, NULL, NUM_SENSORS + 1);

        for (frame = 0; frame < app.frameCount && !g_exit; frame++) {
            pthread_t threads[NUM_SENSORS];
            thread_arg_t targs[NUM_SENSORS];
            long long t_start = get_now_ms();

            for (i = 0; i < NUM_SENSORS; i++) {
                app.sensors[i].got_frame = 0;
                app.sensors[i].save_ok = 0;
                targs[i].app = &app;
                targs[i].sensor_idx = i;
                pthread_create(&threads[i], NULL, grab_thread, &targs[i]);
            }

            pthread_barrier_wait(&app.barrier);

            for (i = 0; i < NUM_SENSORS; i++)
                pthread_join(threads[i], NULL);

            long long t_total = get_now_ms() - t_start;

            if (app.sensors[0].got_frame && app.sensors[1].got_frame) {
                long long pts_diff = app.sensors[0].pts_us - app.sensors[1].pts_us;
                if (pts_diff < 0) pts_diff = -pts_diff;
                printf("Frame %d: s0=%dx%d s1=%dx%d PTS_diff=%lldus (%.2fms) total=%lldms\n",
                       frame,
                       app.sensors[0].frame_width, app.sensors[0].frame_height,
                       app.sensors[1].frame_width, app.sensors[1].frame_height,
                       pts_diff, pts_diff / 1000.0, t_total);
                if (app.action == ACTION_SAVE) {
                    if (app.sensors[0].save_ok)
                        printf("  → %s\n", app.sensors[0].saved_file);
                    if (app.sensors[1].save_ok)
                        printf("  → %s\n", app.sensors[1].saved_file);
                }
            } else {
                printf("Frame %d: FAILED (s0=%d s1=%d)\n", frame,
                       app.sensors[0].got_frame, app.sensors[1].got_frame);
            }
        }

        pthread_barrier_destroy(&app.barrier);
        printf("Done: processed=%d/%d (action=%s)\n",
               app.frameCount, app.frameCount, action_str);
    }

cleanup_vo:
    for (int b = 0; b < 2; b++) {
        if (app.panBlk[b]) { RK_MPI_MMZ_Free(app.panBlk[b]); app.panBlk[b] = NULL; }
    }
    if (vo_inited) vo_deinit(&app);

cleanup_vpss:
    if (vpss_inited) vpss_deinit(&app);

cleanup_chn:
    for (i = 0; i < NUM_SENSORS; i++)
        RK_MPI_VI_StopPipe(app.sensors[i].pipeId);
    for (i = 0; i < NUM_SENSORS; i++)
        RK_MPI_VI_DisableChn(app.sensors[i].pipeId, app.sensors[i].channelId);

cleanup_dev:
    for (i = 0; i < NUM_SENSORS; i++)
        RK_MPI_VI_DisableDev(i);

cleanup_sys:
    RK_MPI_SYS_Exit();
    if (app.verbose) printf("MPI exit\n");

    return 0;
}
