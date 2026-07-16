/*
 * vi_grab_frame.c — минимальная CLI-программа для RV1126B
 *
 * Получает один кадр с камеры через MPI (RK_MPI_VI_GetChnFrame)
 * и сохраняет его в файл как raw NV12.
 *
 * Основано на паттерне из rkipc (app/rkipc/src/rv1126b_ipc/video/video.c)
 * и test_mpi_vi.cpp (external/rockit/mpi/example/mod/).
 *
 * Сборка (на Linux-машине с toolchain aarch64):
 *   export CROSS_COMPILE=aarch64-linux-gnu-
 *   mkdir build && cd build
 *   cmake .. && make
 *
 * Использование на плате:
 *   ./vi_grab_frame -w 1920 -h 1080 -o frame_nv12.raw
 *   ./vi_grab_frame -w 2560 -h 1440 -c 4 -o frame_2560x1440_nv12.raw
 *
 * Параметры:
 *   -w, --width    ширина кадра (обязательно)
 *   -h, --height   высота кадра (обязательно)
 *   -d, --dev      VI device id (по умолчанию 0)
 *   -p, --pipe     VI pipe id (по умолчанию = dev)
 *   -c, --channel  VI channel id (по умолчанию 0)
 *   -o, --output   имя выходного файла (по умолчанию <w>x<h>_nv12.raw)
 *   -n, --count    сколько кадров захватить (по умолчанию 1)
 *   -t, --timeout  таймаут GetChnFrame в мс (по умолчанию 1000)
 *   -v, --verbose  подробный вывод
 *
 * Пример:
 *   # Захват одного кадра 1920x1080 в frame_1920x1080_nv12.raw
 *   ./vi_grab_frame -w 1920 -h 1080
 *
 *   # Захват 10 кадров с канала 4 (как в rkipc для NPU)
 *   ./vi_grab_frame -w 2560 -h 1440 -c 4 -n 10
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/time.h>

#include "rk_defines.h"
#include "rk_debug.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

#define DEFAULT_TIMEOUT_MS 1000
#define DEFAULT_DEV_ID     0
#define DEFAULT_CHANNEL_ID 0
#define DEFAULT_FRAME_COUNT 1

typedef struct {
    int width;
    int height;
    int devId;
    int pipeId;
    int channelId;
    int frameCount;
    int timeoutMs;
    int verbose;
    char outputFile[256];
} vi_grab_ctx_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -w <width> -h <height> [options]\n"
        "\n"
        "Options:\n"
        "  -w, --width <W>     frame width (required)\n"
        "  -h, --height <H>    frame height (required)\n"
        "  -d, --dev <ID>      VI device id (default: %d)\n"
        "  -p, --pipe <ID>     VI pipe id (default: = dev)\n"
        "  -c, --channel <ID>  VI channel id (default: %d)\n"
        "  -o, --output <FILE> output file (default: <W>x<H>_nv12.raw)\n"
        "  -n, --count <N>     number of frames to grab (default: %d)\n"
        "  -t, --timeout <MS>  GetChnFrame timeout in ms (default: %d)\n"
        "  -v, --verbose       verbose output\n"
        "  --help              show this help\n"
        "\n"
        "Examples:\n"
        "  %s -w 1920 -h 1080\n"
        "  %s -w 2560 -h 1440 -c 4 -n 10\n"
        "  %s -w 640 -h 360 -c 4 -o small.raw\n",
        prog, DEFAULT_DEV_ID, DEFAULT_CHANNEL_ID, DEFAULT_FRAME_COUNT,
        DEFAULT_TIMEOUT_MS,
        prog, prog, prog);
}

static int parse_args(vi_grab_ctx_t *ctx, int argc, char **argv) {
    static struct option long_opts[] = {
        {"width",   required_argument, 0, 'w'},
        {"height",  required_argument, 0, 'h'},
        {"dev",     required_argument, 0, 'd'},
        {"pipe",    required_argument, 0, 'p'},
        {"channel", required_argument, 0, 'c'},
        {"output",  required_argument, 0, 'o'},
        {"count",   required_argument, 0, 'n'},
        {"timeout", required_argument, 0, 't'},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    memset(ctx, 0, sizeof(*ctx));
    ctx->width = 0;
    ctx->height = 0;
    ctx->devId = DEFAULT_DEV_ID;
    ctx->pipeId = -1;  /* -1 means "use devId" */
    ctx->channelId = DEFAULT_CHANNEL_ID;
    ctx->frameCount = DEFAULT_FRAME_COUNT;
    ctx->timeoutMs = DEFAULT_TIMEOUT_MS;
    ctx->verbose = 0;
    ctx->outputFile[0] = '\0';

    int opt;
    while ((opt = getopt_long(argc, argv, "w:h:d:p:c:o:n:t:v", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'w': ctx->width = atoi(optarg); break;
            case 'h': ctx->height = atoi(optarg); break;
            case 'd': ctx->devId = atoi(optarg); break;
            case 'p': ctx->pipeId = atoi(optarg); break;
            case 'c': ctx->channelId = atoi(optarg); break;
            case 'o': strncpy(ctx->outputFile, optarg, sizeof(ctx->outputFile) - 1); break;
            case 'n': ctx->frameCount = atoi(optarg); break;
            case 't': ctx->timeoutMs = atoi(optarg); break;
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

    if (ctx->pipeId < 0)
        ctx->pipeId = ctx->devId;

    if (ctx->outputFile[0] == '\0')
        snprintf(ctx->outputFile, sizeof(ctx->outputFile), "%dx%d_nv12.raw",
                 ctx->width, ctx->height);

    return 0;
}

static long long get_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int main(int argc, char **argv) {
    vi_grab_ctx_t ctx;
    int ret;
    int i;

    if (parse_args(&ctx, argc, argv) != 0)
        return 1;

    printf("vi_grab_frame: %dx%d, dev=%d, pipe=%d, chn=%d, frames=%d, out=%s\n",
           ctx.width, ctx.height, ctx.devId, ctx.pipeId, ctx.channelId,
           ctx.frameCount, ctx.outputFile);

    /* 1. Инициализация MPI */
    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_SYS_Init failed: %#x\n", ret);
        return 1;
    }
    if (ctx.verbose) printf("RK_MPI_SYS_Init OK\n");

    /* 2. Настройка VI device (как в rkipc vi_dev_init) */
    VI_DEV_ATTR_S stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    memset(&stBindPipe, 0, sizeof(stBindPipe));

    ret = RK_MPI_VI_GetDevAttr(ctx.devId, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(ctx.devId, &stDevAttr);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "RK_MPI_VI_SetDevAttr failed: %#x\n", ret);
            goto cleanup_sys;
        }
        if (ctx.verbose) printf("RK_MPI_VI_SetDevAttr OK\n");
    } else {
        if (ctx.verbose) printf("VI dev already configured\n");
    }

    ret = RK_MPI_VI_GetDevIsEnable(ctx.devId);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(ctx.devId);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "RK_MPI_VI_EnableDev failed: %#x\n", ret);
            goto cleanup_sys;
        }
        if (ctx.verbose) printf("RK_MPI_VI_EnableDev OK\n");

        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = ctx.pipeId;
        ret = RK_MPI_VI_SetDevBindPipe(ctx.devId, &stBindPipe);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "RK_MPI_VI_SetDevBindPipe failed: %#x\n", ret);
            goto cleanup_dev;
        }
        if (ctx.verbose) printf("RK_MPI_VI_SetDevBindPipe OK\n");
    } else {
        if (ctx.verbose) printf("VI dev already enabled\n");
    }

    /* 3. Настройка VI канала (как в rkipc, канал для NPU/IVS) */
    VI_CHN_ATTR_S vi_chn_attr;
    memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
    vi_chn_attr.stIspOpt.u32BufCount = 3;
    vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    vi_chn_attr.stIspOpt.stMaxSize.u32Width = ctx.width;
    vi_chn_attr.stIspOpt.stMaxSize.u32Height = ctx.height;
    vi_chn_attr.stSize.u32Width = ctx.width;
    vi_chn_attr.stSize.u32Height = ctx.height;
    vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;   /* NV12 */
    vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    vi_chn_attr.u32Depth = 1;                       /* +1 буфер для get/release */

    ret = RK_MPI_VI_SetChnAttr(ctx.pipeId, ctx.channelId, &vi_chn_attr);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_VI_SetChnAttr failed: %#x\n", ret);
        goto cleanup_dev;
    }
    if (ctx.verbose) printf("RK_MPI_VI_SetChnAttr OK (chn=%d)\n", ctx.channelId);

    ret = RK_MPI_VI_EnableChn(ctx.pipeId, ctx.channelId);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_VI_EnableChn failed: %#x\n", ret);
        goto cleanup_dev;
    }
    if (ctx.verbose) printf("RK_MPI_VI_EnableChn OK (chn=%d)\n", ctx.channelId);

    /* 4. Захват кадров */
    long long t_start = get_now_ms();
    long long t_first = 0;

    for (i = 0; i < ctx.frameCount; i++) {
        VIDEO_FRAME_INFO_S stViFrame;
        memset(&stViFrame, 0, sizeof(stViFrame));

        ret = RK_MPI_VI_GetChnFrame(ctx.pipeId, ctx.channelId, &stViFrame, ctx.timeoutMs);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "RK_MPI_VI_GetChnFrame failed (frame %d): %#x\n", i, ret);
            continue;
        }

        if (t_first == 0) {
            t_first = get_now_ms() - t_start;
            printf("First frame: %lld ms\n", t_first);
        }

        void *data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);
        int data_len = RK_MPI_MB_GetLength(stViFrame.stVFrame.pMbBlk);
        int w = stViFrame.stVFrame.u32Width;
        int h = stViFrame.stVFrame.u32Height;
        int seq = stViFrame.stVFrame.u32TimeRef;
        long long pts_us = (long long)stViFrame.stVFrame.u64PTS;

        if (ctx.verbose) {
            printf("Frame %d: %dx%d, seq=%d, pts=%lldus (%lldms), len=%d, data=%p\n",
                   i, w, h, seq, pts_us, pts_us / 1000, data_len, data);
        }

        /* 5. Сохранение в файл (имя включает PTS) */
        if (ctx.frameCount == 1) {
            /* Один кадр — пишем в указанный файл */
            FILE *fp = fopen(ctx.outputFile, "wb");
            if (!fp) {
                fprintf(stderr, "Cannot open %s for writing\n", ctx.outputFile);
            } else {
                /* NV12: Y plane (w*h) + UV plane (w*h/2) = w*h*3/2 */
                int expected = w * h * 3 / 2;
                int to_write = (data_len > 0) ? data_len : expected;
                size_t written = fwrite(data, 1, to_write, fp);
                fclose(fp);
                printf("Saved %zu bytes to %s (%dx%d NV12, PTS=%lldus, expected %d)\n",
                       written, ctx.outputFile, w, h, pts_us, expected);
            }
        } else {
            /* Несколько кадров — каждый в свой файл с PTS в имени */
            char fname[300];
            snprintf(fname, sizeof(fname), "%s_pts%lld_%04d.raw",
                     ctx.outputFile, pts_us, i);
            FILE *fp = fopen(fname, "wb");
            if (!fp) {
                fprintf(stderr, "Cannot open %s for writing\n", fname);
            } else {
                int expected = w * h * 3 / 2;
                int to_write = (data_len > 0) ? data_len : expected;
                size_t written = fwrite(data, 1, to_write, fp);
                fclose(fp);
                if (ctx.verbose)
                    printf("Saved %zu bytes to %s (PTS=%lldus)\n", written, fname, pts_us);
            }
        }

        /* 6. Освобождение кадра */
        ret = RK_MPI_VI_ReleaseChnFrame(ctx.pipeId, ctx.channelId, &stViFrame);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "RK_MPI_VI_ReleaseChnFrame failed: %#x\n", ret);
        }
    }

    long long t_total = get_now_ms() - t_start;
    printf("Done: %d frames in %lld ms\n", i, t_total);

    /* 7. Очистка */
    RK_MPI_VI_DisableChn(ctx.pipeId, ctx.channelId);
    if (ctx.verbose) printf("VI channel disabled\n");

cleanup_dev:
    RK_MPI_VI_DisableDev(ctx.devId);
    if (ctx.verbose) printf("VI device disabled\n");

cleanup_sys:
    RK_MPI_SYS_Exit();
    if (ctx.verbose) printf("MPI system exit\n");

    return 0;
}
