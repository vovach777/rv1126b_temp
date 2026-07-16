/*
 * vi_grab_avs.c — CLI для RV1126B: аппаратное сшивание двух камер в один мега-кадр
 *
 * Использует AVS (Auto Video Stitching) — аппаратный блок rockit.
 * bSyncPipe=1 — аппаратная синхронизация: AVS ждёт оба сенсора, потом склеивает.
 * NOBLEND_HOR — кадры горизонтально рядом без blend/калибровки.
 *
 * Пайплайн: VI dev0/1 → VI pipe0/1 (MAINPATH) → AVS grp0 → AVS chn0 → мега-кадр
 *
 * Основано на rkipc dual_ipc:
 *   - rkipc_vi_dev_init()     — VI device init
 *   - rkipc_multi_vi_init()   — VI channel + StartPipe (group mode)
 *   - rkipc_avs_init()        — AVS group/channel init
 *   - rkipc_bind_init()       — VI → AVS bind
 *
 * Использование:
 *   # Мега-кадр 3840x1080 (2×1920x1080 горизонтально)
 *   ./vi_grab_avs -w 1920 -h 1080
 *   → mega_3840x1080_pts12345678_nv12.raw
 *
 *   # 10 мега-кадров
 *   ./vi_grab_avs -w 1920 -h 1080 -n 10
 *
 *   # Вертикально (1920x2160)
 *   ./vi_grab_avs -w 1920 -h 1080 -m ver
 *
 *   # С калибровочным файлом (для BLEND режима)
 *   ./vi_grab_avs -w 1920 -h 1080 -m blend -calib /oem/usr/share/avs_calib/calib_file.xml
 *
 * Параметры:
 *   -w, --width     ширина одного сенсора (обязательно)
 *   -h, --height    высота одного сенсора (обязательно)
 *   -m, --mode      режим AVS: hor, ver, blend (по умолчанию hor)
 *   -c, --channel   VI channel id (по умолчанию 0 = MAINPATH)
 *   -o, --output    префикс файла (по умолчанию "mega")
 *   -n, --count     сколько мега-кадров (по умолчанию 1)
 *   -t, --timeout   таймаут GetChnFrame в мс (по умолчанию 2000)
 *   --calib         путь к калибровочному XML (для blend)
 *   --no-sync       отключить bSyncPipe (не рекомендуется)
 *   -v, --verbose   подробный вывод
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
#include "rk_comm_avs.h"
#include "rk_comm_mb.h"
#include "rk_comm_video.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_avs.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_cal.h"

#define DEFAULT_TIMEOUT_MS  2000
#define DEFAULT_CHANNEL_ID  0    /* RKISP_MAINPATH */
#define DEFAULT_FRAME_COUNT 1
#define NUM_SENSORS         2
#define AVS_GRP_ID          0
#define AVS_CHN_ID          0

typedef struct {
    int width;
    int height;
    int mode;          /* AVS_MODE_E */
    int channelId;
    int frameCount;
    int timeoutMs;
    int verbose;
    int bSyncPipe;
    char calibFile[256];
    char outputPrefix[128];
} app_ctx_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -w <W> -h <H> [options]\n"
        "\n"
        "Hardware stitch two sensors into one mega-frame via AVS.\n"
        "\n"
        "Options:\n"
        "  -w, --width <W>     single sensor width (required)\n"
        "  -h, --height <H>    single sensor height (required)\n"
        "  -m, --mode <MODE>   AVS mode: hor, ver, blend (default: hor)\n"
        "  -c, --channel <ID>  VI channel id (default: %d = MAINPATH)\n"
        "  -o, --output <PFX>  output file prefix (default: \"mega\")\n"
        "  -n, --count <N>     number of mega-frames (default: %d)\n"
        "  -t, --timeout <MS>  GetChnFrame timeout in ms (default: %d)\n"
        "  --calib <FILE>      calibration XML path (for blend mode)\n"
        "  --no-sync           disable bSyncPipe (not recommended)\n"
        "  -v, --verbose       verbose output\n"
        "  --help              show this help\n"
        "\n"
        "AVS modes:\n"
        "  hor    = NOBLEND_HOR  — side-by-side, 2W x H  (default)\n"
        "  ver    = NOBLEND_VER  — stacked,      W x 2H\n"
        "  blend  = BLEND        — LUT stitch (needs --calib)\n"
        "\n"
        "Output: <prefix>_<W>x<H>_pts<PTS>_nv12.raw\n"
        "\n"
        "Examples:\n"
        "  %s -w 1920 -h 1080\n"
        "  %s -w 1920 -h 1080 -m ver -n 10\n"
        "  %s -w 1920 -h 1080 -m blend --calib /oem/usr/share/avs_calib/calib_file.xml\n",
        prog, DEFAULT_CHANNEL_ID, DEFAULT_FRAME_COUNT, DEFAULT_TIMEOUT_MS,
        prog, prog, prog);
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
        {"timeout", required_argument, 0, 't'},
        {"calib",   required_argument, 0, 1000},
        {"no-sync", no_argument,       0, 1001},
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
    ctx->timeoutMs = DEFAULT_TIMEOUT_MS;
    ctx->verbose = 0;
    ctx->bSyncPipe = 1;
    ctx->calibFile[0] = '\0';
    strcpy(ctx->outputPrefix, "mega");

    int opt;
    while ((opt = getopt_long(argc, argv, "w:h:m:c:o:n:t:v", long_opts, NULL)) != -1) {
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
            case 't': ctx->timeoutMs = atoi(optarg); break;
            case 1000: strncpy(ctx->calibFile, optarg, sizeof(ctx->calibFile) - 1); break;
            case 1001: ctx->bSyncPipe = 0; break;
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

/* Вычислить размер мега-кадра */
static void calc_mega_size(app_ctx_t *ctx, int *mega_w, int *mega_h) {
    if (ctx->mode == AVS_MODE_NOBLEND_HOR) {
        *mega_w = ctx->width * NUM_SENSORS;
        *mega_h = ctx->height;
    } else if (ctx->mode == AVS_MODE_NOBLEND_VER) {
        *mega_w = ctx->width;
        *mega_h = ctx->height * NUM_SENSORS;
    } else {
        /* blend — размер зависит от калибровки, предполагаем 2W x H */
        *mega_w = ctx->width * NUM_SENSORS;
        *mega_h = ctx->height;
    }
}

int main(int argc, char **argv) {
    app_ctx_t ctx;
    int ret, i, frame;

    if (parse_args(&ctx, argc, argv) != 0)
        return 1;

    int mega_w, mega_h;
    calc_mega_size(&ctx, &mega_w, &mega_h);

    const char *mode_str = ctx.mode == AVS_MODE_NOBLEND_HOR ? "NOBLEND_HOR" :
                           ctx.mode == AVS_MODE_NOBLEND_VER ? "NOBLEND_VER" :
                           "BLEND";

    printf("vi_grab_avs: %dx%d per sensor, mode=%s, sync=%d, mega=%dx%d, frames=%d\n",
           ctx.width, ctx.height, mode_str, ctx.bSyncPipe, mega_w, mega_h, ctx.frameCount);

    /* 1. Инициализация MPI */
    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_SYS_Init failed: %#x\n", ret);
        return 1;
    }
    if (ctx.verbose) printf("RK_MPI_SYS_Init OK\n");

    /* 2. VI device init для каждого сенсора (как rkipc_vi_dev_init) */
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
            if (ctx.verbose) printf("dev %d: SetDevAttr OK\n", i);
        } else {
            if (ctx.verbose) printf("dev %d: already configured\n", i);
        }

        ret = RK_MPI_VI_GetDevIsEnable(i);
        if (ret != RK_SUCCESS) {
            ret = RK_MPI_VI_EnableDev(i);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "dev %d: EnableDev failed: %#x\n", i, ret);
                goto cleanup_dev;
            }
            if (ctx.verbose) printf("dev %d: EnableDev OK\n", i);

            stBindPipe.u32Num = 1;
            stBindPipe.PipeId[0] = i;
            stBindPipe.bUserStartPipe[0] = RK_TRUE;
            ret = RK_MPI_VI_SetDevBindPipe(i, &stBindPipe);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "dev %d: SetDevBindPipe failed: %#x\n", i, ret);
                goto cleanup_dev;
            }
            if (ctx.verbose) printf("dev %d: SetDevBindPipe OK\n", i);
        } else {
            if (ctx.verbose) printf("dev %d: already enabled\n", i);
        }
    }

    /* 3. VI каналы — EnableChnExt + StartPipe (group mode, как rkipc_multi_vi_init) */
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
        if (ctx.verbose) printf("sensor %d: chn %d EnableChnExt OK\n", i, ctx.channelId);
    }

    /* group mode: все каналы должны быть готовы до StartPipe */
    for (i = 0; i < NUM_SENSORS; i++) {
        ret = RK_MPI_VI_StartPipe(i);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "pipe %d: StartPipe failed: %#x\n", i, ret);
            goto cleanup_chn;
        }
        if (ctx.verbose) printf("pipe %d: StartPipe OK\n", i);
    }

    /* 4. AVS init (как rkipc_avs_init) */
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

    /* Калибровка: для NOBLEND не нужна, но AVS может требовать.
       Если файл указан — используем. Если нет и NOBLEND — пробуем без. */
    if (ctx.calibFile[0] != '\0') {
        stAvsGrpAttr.stInAttr.enParamSource = AVS_PARAM_SOURCE_CALIB;
        stAvsGrpAttr.stInAttr.stCalib.pCalibFilePath = ctx.calibFile;
        if (ctx.verbose) printf("AVS: using calib file: %s\n", ctx.calibFile);
    } else {
        /* NOBLEND без калибровки — пробуем LUT с пустой таблицей */
        stAvsGrpAttr.stInAttr.enParamSource = AVS_PARAM_SOURCE_LUT;
        stAvsGrpAttr.stInAttr.stLUT.enAccuracy = AVS_LUT_ACCURACY_HIGH;
        stAvsGrpAttr.stInAttr.stLUT.enFuseWidth = AVS_FUSE_WIDTH_MEDIUM;
        stAvsGrpAttr.stInAttr.stLUT.stLutStep.enStepX = AVS_LUT_STEP_MEDIUM;
        stAvsGrpAttr.stInAttr.stLUT.stLutStep.enStepY = AVS_LUT_STEP_MEDIUM;
        /* pVirAddr[0..1] = NULL — AVS должен использовать identity/no-blend */
        if (ctx.verbose) printf("AVS: NOBLEND without calib (empty LUT)\n");
    }

    ret = RK_MPI_AVS_SetModParam(&stAvsModParam);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_AVS_SetModParam failed: %#x\n", ret);
        goto cleanup_pipe;
    }
    if (ctx.verbose) printf("RK_MPI_AVS_SetModParam OK\n");

    ret = RK_MPI_AVS_CreateGrp(AVS_GRP_ID, &stAvsGrpAttr);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_AVS_CreateGrp failed: %#x\n", ret);
        fprintf(stderr, "  (for NOBLEND without calib, try: --calib /path/to/dummy.xml)\n");
        goto cleanup_pipe;
    }
    if (ctx.verbose) printf("RK_MPI_AVS_CreateGrp OK (mode=%d, sync=%d, pipes=%d)\n",
                            ctx.mode, ctx.bSyncPipe, NUM_SENSORS);

    /* LDCH (Lens Distortion Correction) — нужно для AVS даже в NOBLEND.
       Следуем паттерну rkipc: GetFinalLutBufferSize → CreateMB → GetFinalLut. */
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
            if (ret != RK_SUCCESS || pic_cal[i].u32MBSize == 0) {
                fprintf(stderr, "AVS: GetFinalLutBufferSize[%d] failed: %#x, size=%d\n",
                        i, ret, pic_cal[i].u32MBSize);
                continue;
            }
            if (ctx.verbose) printf("AVS: LUT buf[%d] size=%d\n", i, pic_cal[i].u32MBSize);

            ldch_data[i] = malloc(pic_cal[i].u32MBSize);
            memset(&stMbExtConfig, 0, sizeof(stMbExtConfig));
            stMbExtConfig.pu8VirAddr = (RK_U8 *)ldch_data[i];
            stMbExtConfig.u64Size = pic_cal[i].u32MBSize;
            ret = RK_MPI_SYS_CreateMB(&pstFinalLut.pLdchBlk[i], &stMbExtConfig);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "AVS: CreateMB[%d] failed: %#x\n", i, ret);
                free(ldch_data[i]);
                continue;
            }
        }

        ret = RK_MPI_AVS_GetFinalLut(AVS_GRP_ID, &pstFinalLut);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "AVS: GetFinalLut failed: %#x (continuing anyway)\n", ret);
        } else if (ctx.verbose) {
            printf("AVS: GetFinalLut OK\n");
        }

        for (i = 0; i < NUM_SENSORS; i++) {
            if (pstFinalLut.pLdchBlk[i])
                RK_MPI_SYS_Free(pstFinalLut.pLdchBlk[i]);
            if (ldch_data[i])
                free(ldch_data[i]);
        }
    }

    /* 5. AVS channel — выход мега-кадра */
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
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_AVS_SetChnAttr failed: %#x\n", ret);
        goto cleanup_avs_grp;
    }
    ret = RK_MPI_AVS_EnableChn(AVS_GRP_ID, AVS_CHN_ID);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_AVS_EnableChn failed: %#x\n", ret);
        goto cleanup_avs_grp;
    }
    if (ctx.verbose) printf("AVS chn %d: enabled (%dx%d)\n", AVS_CHN_ID, mega_w, mega_h);

    /* 6. Bind VI → AVS (в rkipc dual_ipc это закомментировано, но нам нужно) */
    for (i = 0; i < NUM_SENSORS; i++) {
        MPP_CHN_S vi_chn, avs_in_chn;
        vi_chn.enModId = RK_ID_VI;
        vi_chn.s32DevId = i;
        vi_chn.s32ChnId = ctx.channelId;
        avs_in_chn.enModId = RK_ID_AVS;
        avs_in_chn.s32DevId = AVS_GRP_ID;
        avs_in_chn.s32ChnId = i;   /* AVS pipe id = sensor id */

        ret = RK_MPI_SYS_Bind(&vi_chn, &avs_in_chn);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "Bind VI[%d,%d] → AVS[%d,%d] failed: %#x\n",
                    vi_chn.s32DevId, vi_chn.s32ChnId,
                    avs_in_chn.s32DevId, avs_in_chn.s32ChnId, ret);
            goto cleanup_avs_chn;
        }
        if (ctx.verbose) printf("Bind VI[%d,%d] → AVS[%d,%d] OK\n",
                                vi_chn.s32DevId, vi_chn.s32ChnId,
                                avs_in_chn.s32DevId, avs_in_chn.s32ChnId);
    }

    /* 7. Start AVS group */
    ret = RK_MPI_AVS_StartGrp(AVS_GRP_ID);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_AVS_StartGrp failed: %#x\n", ret);
        goto cleanup_bind;
    }
    if (ctx.verbose) printf("AVS group started\n");

    /* 8. Захват мега-кадров */
    printf("Waiting for mega-frames (sync=%d, this may take a few seconds)...\n",
           ctx.bSyncPipe);

    for (frame = 0; frame < ctx.frameCount; frame++) {
        VIDEO_FRAME_INFO_S stMegaFrame;
        memset(&stMegaFrame, 0, sizeof(stMegaFrame));

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
        void *data = RK_MPI_MB_Handle2VirAddr(stMegaFrame.stVFrame.pMbBlk);
        int data_len = RK_MPI_MB_GetLength(stMegaFrame.stVFrame.pMbBlk);

        if (ctx.verbose) {
            printf("Frame %d: %dx%d pts=%lldus (%lldms) len=%d grab=%lldms\n",
                   frame, w, h, pts_us, pts_us / 1000, data_len, t_grab);
        }

        /* Сохранение в файл */
        char fname[300];
        snprintf(fname, sizeof(fname), "%s_%dx%d_pts%lld_nv12.raw",
                 ctx.outputPrefix, w, h, pts_us);
        FILE *fp = fopen(fname, "wb");
        if (!fp) {
            fprintf(stderr, "Cannot open %s\n", fname);
        } else {
            int expected = w * h * 3 / 2;
            int to_write = (data_len > 0) ? data_len : expected;
            size_t written = fwrite(data, 1, to_write, fp);
            fclose(fp);
            printf("Frame %d: %dx%d pts=%lldus grab=%lldms → %s (%zu bytes)\n",
                   frame, w, h, pts_us, t_grab, fname, written);
        }

        RK_MPI_AVS_ReleaseChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stMegaFrame);
    }

    /* 9. Cleanup */
cleanup_bind:
    for (i = 0; i < NUM_SENSORS; i++) {
        MPP_CHN_S vi_chn, avs_in_chn;
        vi_chn.enModId = RK_ID_VI;
        vi_chn.s32DevId = i;
        vi_chn.s32ChnId = ctx.channelId;
        avs_in_chn.enModId = RK_ID_AVS;
        avs_in_chn.s32DevId = AVS_GRP_ID;
        avs_in_chn.s32ChnId = i;
        RK_MPI_SYS_UnBind(&vi_chn, &avs_in_chn);
    }

cleanup_avs_chn:
    RK_MPI_AVS_DisableChn(AVS_GRP_ID, AVS_CHN_ID);

cleanup_avs_grp:
    RK_MPI_AVS_StopGrp(AVS_GRP_ID);
    RK_MPI_AVS_DestroyGrp(AVS_GRP_ID);

cleanup_pipe:
    for (i = 0; i < NUM_SENSORS; i++)
        RK_MPI_VI_StopPipe(i);

cleanup_chn:
    for (i = 0; i < NUM_SENSORS; i++)
        RK_MPI_VI_DisableChnExt(i, ctx.channelId);

cleanup_dev:
    for (i = 0; i < NUM_SENSORS; i++)
        RK_MPI_VI_DisableDev(i);

cleanup_sys:
    RK_MPI_SYS_Exit();
    if (ctx.verbose) printf("MPI exit\n");

    return 0;
}
