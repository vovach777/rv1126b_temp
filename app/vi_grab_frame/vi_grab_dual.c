/*
 * vi_grab_dual.c — CLI-программа для RV1126B: одновременный захват с двух сенсоров
 *
 * Получает кадры с двух камер через MPI с минимальной задержкой между ними.
 * Использует два потока, каждый вызывает RK_MPI_VI_GetChnFrame одновременно
 * (синхронизация через pthread_barrier).
 *
 * Основано на rkipc dual_ipc (app/rkipc/src/rv1126b_dual_ipc/video/video.c)
 *
 * Использование:
 *   # Одновременный захват с двух сенсоров (1920x1080 каждый)
 *   ./vi_grab_dual -w 1920 -h 1080
 *   → sensor0_1920x1080_nv12.raw
 *   → sensor1_1920x1080_nv12.raw
 *
 *   # Разные разрешения для каждого сенсора
 *   ./vi_grab_dual -w 1920 -h 1080 -W 640 -H 360
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/time.h>

#include "rk_defines.h"
#include "rk_debug.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

#define DEFAULT_TIMEOUT_MS  1000
#define DEFAULT_CHANNEL_ID  0
#define DEFAULT_FRAME_COUNT 1
#define NUM_SENSORS         2

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
} app_ctx_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -w <W0> -h <H0> [options]\n"
        "\n"
        "Grab frames from TWO sensors simultaneously with minimum latency.\n"
        "\n"
        "Options:\n"
        "  -w, --width0 <W>    sensor 0 width (required)\n"
        "  -h, --height0 <H>   sensor 0 height (required)\n"
        "  -W, --width1 <W>    sensor 1 width (default: = width0)\n"
        "  -H, --height1 <H>   sensor 1 height (default: = height0)\n"
        "  -c, --channel <ID>  VI channel id (default: %d)\n"
        "  -o, --output <PFX>  output file prefix (default: \"sensor\")\n"
        "  -n, --count <N>     number of frame pairs (default: %d)\n"
        "  -t, --timeout <MS>  GetChnFrame timeout in ms (default: %d)\n"
        "  -v, --verbose       verbose output\n"
        "  --help              show this help\n"
        "\n"
        "Output files: <prefix>0_<W>x<H>_nv12.raw, <prefix>1_<W>x<H>_nv12.raw\n"
        "\n"
        "Examples:\n"
        "  %s -w 1920 -h 1080\n"
        "  %s -w 1920 -h 1080 -W 640 -H 360 -n 10\n",
        prog, DEFAULT_CHANNEL_ID, DEFAULT_FRAME_COUNT, DEFAULT_TIMEOUT_MS,
        prog, prog);
}

static int parse_args(app_ctx_t *app, int argc, char **argv) {
    static struct option long_opts[] = {
        {"width0",   required_argument, 0, 'w'},
        {"height0",  required_argument, 0, 'h'},
        {"width1",   required_argument, 0, 'W'},
        {"height1",  required_argument, 0, 'H'},
        {"channel",  required_argument, 0, 'c'},
        {"output",   required_argument, 0, 'o'},
        {"count",    required_argument, 0, 'n'},
        {"timeout",  required_argument, 0, 't'},
        {"verbose",  no_argument,       0, 'v'},
        {"help",     no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    int w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    int channel = DEFAULT_CHANNEL_ID;
    int count = DEFAULT_FRAME_COUNT;
    int timeout = DEFAULT_TIMEOUT_MS;
    int verbose = 0;
    char prefix[128] = "sensor";

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

/* Поток захвата одного сенсора */
static void *grab_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    app_ctx_t *app = targ->app;
    sensor_ctx_t *s = &app->sensors[targ->sensor_idx];
    int ret;
    VIDEO_FRAME_INFO_S stViFrame;
    memset(&stViFrame, 0, sizeof(stViFrame));

    /* Ждём остальные потоки, чтобы вызвать GetChnFrame одновременно */
    pthread_barrier_wait(&app->barrier);

    ret = RK_MPI_VI_GetChnFrame(s->pipeId, s->channelId, &stViFrame, s->timeoutMs);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "[sensor %d] RK_MPI_VI_GetChnFrame failed: %#x\n", s->devId, ret);
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

    /* Сохранение в файл */
    snprintf(s->saved_file, sizeof(s->saved_file), "%s%d_%dx%d_nv12.raw",
             app->outputPrefix, s->devId, s->frame_width, s->frame_height);
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
            fprintf(stderr, "[sensor %d] saved %zu bytes to %s\n",
                    s->devId, written, s->saved_file);
    }

    /* Освобождение кадра */
    ret = RK_MPI_VI_ReleaseChnFrame(s->pipeId, s->channelId, &stViFrame);
    if (ret != RK_SUCCESS)
        fprintf(stderr, "[sensor %d] ReleaseChnFrame failed: %#x\n", s->devId, ret);

    return NULL;
}

int main(int argc, char **argv) {
    app_ctx_t app;
    int ret, i, frame;

    if (parse_args(&app, argc, argv) != 0)
        return 1;

    printf("vi_grab_dual: sensor0=%dx%d, sensor1=%dx%d, chn=%d, frames=%d\n",
           app.sensors[0].width, app.sensors[0].height,
           app.sensors[1].width, app.sensors[1].height,
           app.sensors[0].channelId, app.frameCount);

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
        ret = RK_MPI_VI_EnableChn(app.sensors[i].pipeId, app.sensors[i].channelId);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "sensor %d: EnableChn failed: %#x\n", i, ret);
            goto cleanup_chn;
        }
        if (app.verbose) printf("sensor %d: chn %d enabled (%dx%d)\n",
                                i, app.sensors[i].channelId,
                                app.sensors[i].width, app.sensors[i].height);
    }

    /* 4. Захват пар кадров */
    pthread_barrier_init(&app.barrier, NULL, NUM_SENSORS + 1);

    for (frame = 0; frame < app.frameCount; frame++) {
        pthread_t threads[NUM_SENSORS];
        thread_arg_t targs[NUM_SENSORS];
        long long t_start = get_now_ms();

        /* Запускаем потоки */
        for (i = 0; i < NUM_SENSORS; i++) {
            app.sensors[i].got_frame = 0;
            app.sensors[i].save_ok = 0;
            targs[i].app = &app;
            targs[i].sensor_idx = i;
            pthread_create(&threads[i], NULL, grab_thread, &targs[i]);
        }

        /* Синхронизация: все потоки стартуют GetChnFrame одновременно */
        pthread_barrier_wait(&app.barrier);

        /* Ждём завершения */
        for (i = 0; i < NUM_SENSORS; i++)
            pthread_join(threads[i], NULL);

        long long t_total = get_now_ms() - t_start;

        /* Отчёт */
        if (app.sensors[0].got_frame && app.sensors[1].got_frame) {
            long long pts_diff = app.sensors[0].pts_us - app.sensors[1].pts_us;
            if (pts_diff < 0) pts_diff = -pts_diff;
            printf("Frame %d: s0=%dx%d s1=%dx%d PTS_diff=%lldus (%.2fms) total=%lldms\n",
                   frame,
                   app.sensors[0].frame_width, app.sensors[0].frame_height,
                   app.sensors[1].frame_width, app.sensors[1].frame_height,
                   pts_diff, pts_diff / 1000.0, t_total);
            if (app.sensors[0].save_ok)
                printf("  → %s\n", app.sensors[0].saved_file);
            if (app.sensors[1].save_ok)
                printf("  → %s\n", app.sensors[1].saved_file);
        } else {
            printf("Frame %d: FAILED (s0=%d s1=%d)\n", frame,
                   app.sensors[0].got_frame, app.sensors[1].got_frame);
        }
    }

    pthread_barrier_destroy(&app.barrier);

    /* 5. Очистка */
cleanup_chn:
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
