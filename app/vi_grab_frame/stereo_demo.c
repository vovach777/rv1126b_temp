/*
 * stereo_demo.c — CLI для RV1126B: стерео-камера с аппаратной синхронизацией
 *
 * Полный пайплайн: 2× GC2093 → camgroup (sync 3A) → AVS (sync+balance) → VPSS (fan-out)
 *
 * Все настройки в коде/CLI — НЕ нужно патчить rkipc или системные ini-файлы.
 *
 * Пайплайн:
 *   Cam0 → ISP0 ─┐
 *                 ├→ AVS (NOBLEND_HOR, bSyncPipe) → VPSS_GRP → CHN0 (crop left)  → файл
 *   Cam1 → ISP1 ─┘                                          → CHN1 (crop right) → файл
 *                                                           → CHN2 (full stitch) → файл
 *                                                           → VPSS CHN0/1/2 (crop + full)
 *
 * Что делает программа:
 *   1. camgroup_init()  — rk_aiq_uapi2_camgroup_create (синхронизация AE/AWB)
 *   2. vi_init()         — VI device + channels + StartPipe (group mode)
 *   3. avs_init()        — AVS group (NOBLEND_HOR, bSyncPipe, LDCH)
 *   4. vpss_init()       — VPSS group + 3 канала (crop left/right/full)
 *   5. bind_init()       — VI → AVS → VPSS
 *   6. main loop         — GetChnFrame с каждого канала, сохранение в файл
 *
 * Использование:
 *   # Базовый запуск (2× 1920×1080, crop на две камеры)
 *   ./stereo_demo -w 1920 -h 1080
 *
 *   # 10 кадров, сохранить каждую камеру отдельно
 *   ./stereo_demo -w 1920 -h 1080 -n 10 --save-cam0 --save-cam1
 *
 *   # Сохранить полный стitch
 *   ./stereo_demo -w 1920 -h 1080 -n 10 --save-full
 *
 *   # Snapshot (полный stitch)
 *   ./stereo_demo -w 1920 -h 1080 --save-full -n 1
 *
 *   # Без camgroup (только AVS sync, без 3A sync)
 *   ./stereo_demo -w 1920 -h 1080 --no-camgroup
 *
 * Параметры:
 *   -w, --width       ширина одного сенсора (обязательно)
 *   -h, --height      высота одного сенсора (обязательно)
 *   -n, --count       сколько кадров сохранить (по умолчанию 1)
 *   -s, --skip        отбросить первые N кадров (прогрев, по умолчанию 5)
 *   --iq-dir          путь к IQ-файлам (по умолчанию /oem/usr/share/iqfiles)
 *   --no-camgroup     отключить camgroup (только AVS sync)
 *   --no-sync         отключить bSyncPipe (не рекомендуется)
 *   --no-ldch         отключить LDCH (коррекция дисторсии)
 *   --save-cam0       сохранять CHN0 (левая камера) в cam0_XXX.raw
 *   --save-cam1       сохранять CHN1 (правая камера) в cam1_XXX.raw
 *   --save-full       сохранять CHN2 (полный stitch) в full_XXX.raw
 *   -o, --output      префикс файла (по умолчанию "stereo")
 *   -t, --timeout     таймаут GetChnFrame в мс (по умолчанию 2000)
 *   -v, --verbose     подробный вывод
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/time.h>

/* rockit MPI */
#include "rk_defines.h"
#include "rk_debug.h"
#include "rk_common.h"
#include "rk_comm_vi.h"
#include "rk_comm_avs.h"
#include "rk_comm_vpss.h"
#include "rk_comm_mb.h"
#include "rk_comm_video.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_avs.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_cal.h"

/* rkaiq (3A + camgroup) — заголовки из external/camera_engine_rkaiq/rkaiq/include/uAPI2/ */
#include "rk_aiq_user_api2_sysctl.h"
#include "rk_aiq_user_api2_camgroup.h"

#define DEFAULT_TIMEOUT_MS   2000
#define DEFAULT_SKIP_FRAMES  5
#define DEFAULT_FRAME_COUNT  1
#define NUM_SENSORS          2
#define AVS_GRP_ID           0
#define AVS_CHN_ID           0
#define VPSS_GRP_ID          0
#define VPSS_CHN_CAM0        0
#define VPSS_CHN_CAM1        1
#define VPSS_CHN_FULL        2
#define DEFAULT_IQ_DIR       "/oem/usr/share/iqfiles"

typedef struct {
    int width;
    int height;
    int frameCount;
    int skipFrames;
    int timeoutMs;
    int verbose;
    int bSyncPipe;
    int enableLdch;
    int enableCamgroup;
    int saveCam0;
    int saveCam1;
    int saveFull;
    char iqDir[256];
    char outputPrefix[64];
    int noVpss;  /* get frame directly from AVS (no VPSS) */
} StereoCtx;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -w <width> -h <height> [options]\n"
        "\n"
        "Required:\n"
        "  -w, --width <W>      sensor width (e.g. 1920)\n"
        "  -h, --height <H>     sensor height (e.g. 1080)\n"
        "\n"
        "Options:\n"
        "  -n, --count <N>      frames to save (default: 1)\n"
        "  -s, --skip <N>       discard first N frames for warmup (default: 5)\n"
        "  --iq-dir <path>      IQ files directory (default: %s)\n"
        "  --no-camgroup        disable camgroup (only AVS sync, no 3A sync)\n"
        "  --no-sync            disable bSyncPipe (not recommended)\n"
        "  --no-ldch            disable LDCH (lens distortion correction)\n"
        "  --save-cam0          save CHN0 (left camera) to cam0_XXX.raw\n"
        "  --save-cam1          save CHN1 (right camera) to cam1_XXX.raw\n"
        "  --save-full          save CHN2 (full stitch) to full_XXX.raw\n"
        "  --no-vpss            get frame directly from AVS (skip VPSS, like vi_grab_avs)\n"
        "  -o, --output <pref>  output file prefix (default: stereo)\n"
        "  -t, --timeout <ms>   GetChnFrame timeout (default: %d)\n"
        "  -v, --verbose        verbose output\n"
        "\n"
        "Examples:\n"
        "  %s -w 1920 -h 1080 --save-cam0 --save-cam1 -n 10\n"
        "  %s -w 1920 -h 1080 --save-full -n 10\n",
        prog, DEFAULT_IQ_DIR, DEFAULT_TIMEOUT_MS, prog, prog);
}

static int parse_args(StereoCtx *ctx, int argc, char **argv) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->frameCount = DEFAULT_FRAME_COUNT;
    ctx->skipFrames = DEFAULT_SKIP_FRAMES;
    ctx->timeoutMs = DEFAULT_TIMEOUT_MS;
    ctx->bSyncPipe = 1;
    ctx->enableLdch = 1;
    ctx->enableCamgroup = 1;
    strcpy(ctx->iqDir, DEFAULT_IQ_DIR);
    strcpy(ctx->outputPrefix, "stereo");

    static struct option long_opts[] = {
        {"width",       required_argument, 0, 'w'},
        {"height",      required_argument, 0, 'h'},
        {"count",       required_argument, 0, 'n'},
        {"skip",        required_argument, 0, 's'},
        {"iq-dir",      required_argument, 0, 1001},
        {"no-camgroup", no_argument,       0, 1004},
        {"no-sync",     no_argument,       0, 1005},
        {"no-ldch",     no_argument,       0, 1006},
        {"save-cam0",   no_argument,       0, 1007},
        {"save-cam1",   no_argument,       0, 1008},
        {"save-full",   no_argument,       0, 1009},
        {"no-vpss",     no_argument,       0, 1011},
        {"output",      required_argument, 0, 'o'},
        {"timeout",     required_argument, 0, 't'},
        {"verbose",     no_argument,       0, 'v'},
        {"help",        no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    int c, opt_idx;
    while ((c = getopt_long(argc, argv, "w:h:n:s:o:t:v?", long_opts, &opt_idx)) != -1) {
        switch (c) {
            case 'w': ctx->width = atoi(optarg); break;
            case 'h': ctx->height = atoi(optarg); break;
            case 'n': ctx->frameCount = atoi(optarg); break;
            case 's': ctx->skipFrames = atoi(optarg); break;
            case 'o': strncpy(ctx->outputPrefix, optarg, sizeof(ctx->outputPrefix)-1); break;
            case 't': ctx->timeoutMs = atoi(optarg); break;
            case 'v': ctx->verbose = 1; break;
            case 1001: strncpy(ctx->iqDir, optarg, sizeof(ctx->iqDir)-1); break;
            case 1004: ctx->enableCamgroup = 0; break;
            case 1005: ctx->bSyncPipe = 0; break;
            case 1006: ctx->enableLdch = 0; break;
            case 1007: ctx->saveCam0 = 1; break;
            case 1008: ctx->saveCam1 = 1; break;
            case 1009: ctx->saveFull = 1; break;
            case 1011: ctx->noVpss = 1; break;
            case '?': usage(argv[0]); return -1;
            default:  usage(argv[0]); return -1;
        }
    }

    if (ctx->width <= 0 || ctx->height <= 0) {
        fprintf(stderr, "Error: --width and --height are required\n");
        usage(argv[0]);
        return -1;
    }
    return 0;
}

/* ─── 1. camgroup_init — синхронизация 3A между камерами ─── */

static rk_aiq_camgroup_ctx_t *g_camgroup_ctx = NULL;

static int camgroup_init(StereoCtx *ctx) {
    if (!ctx->enableCamgroup) {
        if (ctx->verbose) printf("camgroup: disabled (--no-camgroup)\n");
        return 0;
    }

    rk_aiq_camgroup_instance_cfg_t cfg;
    rk_aiq_static_info_t aiq_static_info;
    char sensor_names[NUM_SENSORS][128];
    memset(&cfg, 0, sizeof(cfg));
    memset(&aiq_static_info, 0, sizeof(aiq_static_info));

    cfg.sns_num = NUM_SENSORS;
    cfg.config_file_dir = ctx->iqDir;

    /* Enumerate sensors by physical ID */
    for (int i = 0; i < NUM_SENSORS; i++) {
        rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(i, &aiq_static_info);
        if (!aiq_static_info.sensor_info.sensor_name[0]) {
            fprintf(stderr, "camgroup: sensor %d not found\n", i);
            return -1;
        }
        strncpy(sensor_names[i], aiq_static_info.sensor_info.sensor_name,
                sizeof(sensor_names[i]) - 1);
        cfg.sns_ent_nm_array[i] = sensor_names[i];
        rk_aiq_uapi2_sysctl_preInit_scene(sensor_names[i], "normal", "day");
        if (ctx->verbose) printf("camgroup: sensor[%d] = %s\n", i, sensor_names[i]);
    }

    g_camgroup_ctx = rk_aiq_uapi2_camgroup_create(&cfg);
    if (!g_camgroup_ctx) {
        fprintf(stderr, "camgroup: rk_aiq_uapi2_camgroup_create failed\n");
        return -1;
    }
    if (ctx->verbose) printf("camgroup: create OK\n");

    int ret = rk_aiq_uapi2_camgroup_prepare(g_camgroup_ctx, RK_AIQ_WORKING_MODE_NORMAL);
    if (ret) {
        fprintf(stderr, "camgroup: prepare failed: %d\n", ret);
        return -1;
    }
    if (ctx->verbose) printf("camgroup: prepare OK\n");

    ret = rk_aiq_uapi2_camgroup_start(g_camgroup_ctx);
    if (ret) {
        fprintf(stderr, "camgroup: start failed: %d\n", ret);
        return -1;
    }
    if (ctx->verbose) printf("camgroup: start OK (3A synchronized)\n");

    return 0;
}

static void camgroup_deinit(void) {
    if (g_camgroup_ctx) {
        rk_aiq_uapi2_camgroup_stop(g_camgroup_ctx);
        rk_aiq_uapi2_camgroup_destroy(g_camgroup_ctx);
        g_camgroup_ctx = NULL;
    }
}

/* ─── 2. vi_init — VI device + channels + StartPipe ─── */

static int vi_init(StereoCtx *ctx) {
    int i, ret;

    /* VI device init для каждого сенсора */
    for (i = 0; i < NUM_SENSORS; i++) {
        VI_DEV_ATTR_S stDevAttr;
        VI_DEV_BIND_PIPE_S stBindPipe;
        memset(&stDevAttr, 0, sizeof(stDevAttr));
        memset(&stBindPipe, 0, sizeof(stBindPipe));

        ret = RK_MPI_VI_GetDevAttr(i, &stDevAttr);
        if (ret == RK_ERR_VI_NOT_CONFIG) {
            ret = RK_MPI_VI_SetDevAttr(i, &stDevAttr);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "vi: dev %d SetDevAttr failed: %#x\n", i, ret);
                return -1;
            }
            if (ctx->verbose) printf("vi: dev %d SetDevAttr OK\n", i);
        }

        ret = RK_MPI_VI_GetDevIsEnable(i);
        if (ret != RK_SUCCESS) {
            ret = RK_MPI_VI_EnableDev(i);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "vi: dev %d EnableDev failed: %#x\n", i, ret);
                return -1;
            }
            if (ctx->verbose) printf("vi: dev %d EnableDev OK\n", i);

            stBindPipe.u32Num = 1;
            stBindPipe.PipeId[0] = i;
            stBindPipe.bUserStartPipe[0] = RK_TRUE;
            ret = RK_MPI_VI_SetDevBindPipe(i, &stBindPipe);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "vi: dev %d SetDevBindPipe failed: %#x\n", i, ret);
                return -1;
            }
        }
    }

    /* VI channels — MAINPATH для каждого сенсора */
    for (i = 0; i < NUM_SENSORS; i++) {
        VI_CHN_ATTR_S vi_chn_attr;
        memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
        vi_chn_attr.stIspOpt.u32BufCount = 3;
        vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
        vi_chn_attr.stSize.u32Width = ctx->width;
        vi_chn_attr.stSize.u32Height = ctx->height;
        vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
        vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
        vi_chn_attr.u32Depth = 0;
        vi_chn_attr.stFrameRate.s32SrcFrameRate = -1;
        vi_chn_attr.stFrameRate.s32DstFrameRate = -1;

        ret = RK_MPI_VI_SetChnAttr(i, 0, &vi_chn_attr);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vi: sensor %d SetChnAttr failed: %#x\n", i, ret);
            return -1;
        }

        ret = RK_MPI_VI_EnableChnExt(i, 0);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vi: sensor %d EnableChnExt failed: %#x\n", i, ret);
            return -1;
        }
        if (ctx->verbose) printf("vi: sensor %d chn 0 EnableChnExt OK\n", i);
    }

    /* group mode: все каналы должны быть готовы до StartPipe */
    for (i = 0; i < NUM_SENSORS; i++) {
        ret = RK_MPI_VI_StartPipe(i);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vi: pipe %d StartPipe failed: %#x\n", i, ret);
            return -1;
        }
        if (ctx->verbose) printf("vi: pipe %d StartPipe OK\n", i);
    }

    return 0;
}

static void vi_deinit(void) {
    int i;
    for (i = 0; i < NUM_SENSORS; i++) {
        RK_MPI_VI_StopPipe(i);
        RK_MPI_VI_DisableChnExt(i, 0);
        RK_MPI_VI_DisableDev(i);
    }
}

/* ─── 3. avs_init — AVS group (NOBLEND_HOR, bSyncPipe, LDCH) ─── */

static int avs_init(StereoCtx *ctx) {
    int i, ret;
    int mega_w = ctx->width * NUM_SENSORS;
    int mega_h = ctx->height;

    AVS_MOD_PARAM_S stAvsModParam;
    AVS_GRP_ATTR_S stAvsGrpAttr;
    memset(&stAvsModParam, 0, sizeof(stAvsModParam));
    memset(&stAvsGrpAttr, 0, sizeof(stAvsGrpAttr));

    stAvsModParam.u32WorkingSetSize = 0;
    stAvsModParam.enMBSource = MB_SOURCE_PRIVATE;

    /* NOBLEND_HOR — просто кладёт две камеры рядом без blend */
    stAvsGrpAttr.enMode = AVS_MODE_NOBLEND_HOR;
    stAvsGrpAttr.u32PipeNum = NUM_SENSORS;
    stAvsGrpAttr.bSyncPipe = ctx->bSyncPipe;
    stAvsGrpAttr.stGainAttr.enMode = AVS_GAIN_MODE_AUTO;
    stAvsGrpAttr.stOutAttr.enPrjMode = AVS_PROJECTION_RECTILINEAR;
    stAvsGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stAvsGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stAvsGrpAttr.stInAttr.stSize.u32Width = ctx->width;
    stAvsGrpAttr.stInAttr.stSize.u32Height = ctx->height;
    stAvsGrpAttr.stOutAttr.fDistance = 5;

    /* NOBLEND без калибровки — пустая LUT */
    stAvsGrpAttr.stInAttr.enParamSource = AVS_PARAM_SOURCE_LUT;
    stAvsGrpAttr.stInAttr.stLUT.enAccuracy = AVS_LUT_ACCURACY_HIGH;
    stAvsGrpAttr.stInAttr.stLUT.enFuseWidth = AVS_FUSE_WIDTH_MEDIUM;
    stAvsGrpAttr.stInAttr.stLUT.stLutStep.enStepX = AVS_LUT_STEP_MEDIUM;
    stAvsGrpAttr.stInAttr.stLUT.stLutStep.enStepY = AVS_LUT_STEP_MEDIUM;

    ret = RK_MPI_AVS_SetModParam(&stAvsModParam);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "avs: SetModParam failed: %#x\n", ret);
        return -1;
    }

    ret = RK_MPI_AVS_CreateGrp(AVS_GRP_ID, &stAvsGrpAttr);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "avs: CreateGrp failed: %#x\n", ret);
        return -1;
    }
    if (ctx->verbose)
        printf("avs: CreateGrp OK (mode=NOBLEND_HOR, sync=%d, %dx%d → %dx%d)\n",
               ctx->bSyncPipe, ctx->width, ctx->height, mega_w, mega_h);

    /* LDCH — коррекция дисторсии */
    if (ctx->enableLdch) {
        AVS_FINAL_LUT_S pstFinalLut;
        PIC_BUF_ATTR_S stBufAttr;
        MB_PIC_CAL_S pic_cal[NUM_SENSORS];
        MB_EXT_CONFIG_S stMbExtConfig;
        void *ldch_data[NUM_SENSORS];

        memset(&pstFinalLut, 0, sizeof(pstFinalLut));
        for (i = 0; i < NUM_SENSORS; i++) {
            memset(&stBufAttr, 0, sizeof(stBufAttr));
            memset(&pic_cal[i], 0, sizeof(pic_cal[i]));
            stBufAttr.u32Width = ctx->width;
            stBufAttr.u32Height = ctx->height;
            ret = RK_MPI_CAL_AVS_GetFinalLutBufferSize(&stBufAttr, &pic_cal[i]);
            if (ret != RK_SUCCESS || pic_cal[i].u32MBSize == 0) {
                fprintf(stderr, "avs: LUT buf[%d] failed: %#x\n", i, ret);
                continue;
            }
            if (ctx->verbose) printf("avs: LUT buf[%d] size=%d\n", i, pic_cal[i].u32MBSize);

            ldch_data[i] = malloc(pic_cal[i].u32MBSize);
            memset(&stMbExtConfig, 0, sizeof(stMbExtConfig));
            stMbExtConfig.pu8VirAddr = (RK_U8 *)ldch_data[i];
            stMbExtConfig.u64Size = pic_cal[i].u32MBSize;
            ret = RK_MPI_SYS_CreateMB(&pstFinalLut.pLdchBlk[i], &stMbExtConfig);
            if (ret != RK_SUCCESS) {
                free(ldch_data[i]);
                continue;
            }
        }

        ret = RK_MPI_AVS_GetFinalLut(AVS_GRP_ID, &pstFinalLut);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "avs: GetFinalLut failed: %#x (continuing)\n", ret);
        } else if (ctx->verbose) {
            printf("avs: GetFinalLut OK (LDCH applied)\n");
        }

        for (i = 0; i < NUM_SENSORS; i++) {
            if (pstFinalLut.pLdchBlk[i])
                RK_MPI_SYS_Free(pstFinalLut.pLdchBlk[i]);
            if (ldch_data[i])
                free(ldch_data[i]);
        }
    } else {
        if (ctx->verbose) printf("avs: LDCH disabled (--no-ldch)\n");
    }

    /* AVS channel — выход мега-кадра */
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
        fprintf(stderr, "avs: SetChnAttr failed: %#x\n", ret);
        return -1;
    }
    ret = RK_MPI_AVS_EnableChn(AVS_GRP_ID, AVS_CHN_ID);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "avs: EnableChn failed: %#x\n", ret);
        return -1;
    }
    if (ctx->verbose) printf("avs: chn %d enabled (%dx%d)\n", AVS_CHN_ID, mega_w, mega_h);

    return 0;
}

static void avs_deinit(void) {
    RK_MPI_AVS_DisableChn(AVS_GRP_ID, AVS_CHN_ID);
    RK_MPI_AVS_StopGrp(AVS_GRP_ID);
    RK_MPI_AVS_DestroyGrp(AVS_GRP_ID);
}

/* ─── 4. vpss_init — VPSS group + 3 канала (crop left/right/full) ─── */

static int vpss_init(StereoCtx *ctx) {
    int ret;
    int mega_w = ctx->width * NUM_SENSORS;
    int mega_h = ctx->height;

    VPSS_GRP_ATTR_S stVpssGrpAttr;
    memset(&stVpssGrpAttr, 0, sizeof(stVpssGrpAttr));
    stVpssGrpAttr.u32MaxW = mega_w;
    stVpssGrpAttr.u32MaxH = mega_h;
    stVpssGrpAttr.enPixelFormat = RK_FMT_YUV420SP;
    stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stVpssGrpAttr.enCompressMode = COMPRESS_MODE_NONE;

    /* Аппаратный VPSS (не RGA, не GPU) */
    stVpssGrpAttr.enVProcDev = VIDEO_PROC_DEV_VPSS;

    ret = RK_MPI_VPSS_CreateGrp(VPSS_GRP_ID, &stVpssGrpAttr);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "vpss: CreateGrp failed: %#x\n", ret);
        return -1;
    }
    if (ctx->verbose) printf("vpss: CreateGrp OK (maxW=%d, maxH=%d, VPSS hw)\n", mega_w, mega_h);

    /* CHN0: левая камера (cam0) — crop левой половины */
    {
        VPSS_CHN_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        attr.enChnMode = VPSS_CHN_MODE_USER;
        attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
        attr.enPixelFormat = RK_FMT_YUV420SP;
        attr.stFrameRate.s32SrcFrameRate = -1;
        attr.stFrameRate.s32DstFrameRate = -1;
        attr.u32Width = ctx->width;
        attr.u32Height = ctx->height;
        attr.enCompressMode = COMPRESS_MODE_NONE;
        attr.u32FrameBufCnt = 2;
        attr.u32Depth = 1;

        ret = RK_MPI_VPSS_SetChnAttr(VPSS_GRP_ID, VPSS_CHN_CAM0, &attr);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: CHN0 SetChnAttr failed: %#x\n", ret);
            return -1;
        }

        ret = RK_MPI_VPSS_EnableChn(VPSS_GRP_ID, VPSS_CHN_CAM0);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: CHN0 EnableChn failed: %#x\n", ret);
            return -1;
        }

        /* Crop: левая половина входного кадра (set AFTER EnableChn) */
        VPSS_CROP_INFO_S crop;
        memset(&crop, 0, sizeof(crop));
        crop.bEnable = RK_TRUE;
        crop.enCropCoordinate = VPSS_CROP_ABS_COOR;
        crop.stCropRect.s32X = 0;
        crop.stCropRect.s32Y = 0;
        crop.stCropRect.u32Width = ctx->width;
        crop.stCropRect.u32Height = ctx->height;
        ret = RK_MPI_VPSS_SetChnCrop(VPSS_GRP_ID, VPSS_CHN_CAM0, &crop);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: CHN0 SetChnCrop failed: %#x\n", ret);
            return -1;
        }
        if (ctx->verbose) printf("vpss: CHN0 (cam0, crop 0,0,%d,%d) enabled\n",
                                 ctx->width, ctx->height);
    }

    /* CHN1: правая камера (cam1) — crop правой половины */
    {
        VPSS_CHN_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        attr.enChnMode = VPSS_CHN_MODE_USER;
        attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
        attr.enPixelFormat = RK_FMT_YUV420SP;
        attr.stFrameRate.s32SrcFrameRate = -1;
        attr.stFrameRate.s32DstFrameRate = -1;
        attr.u32Width = ctx->width;
        attr.u32Height = ctx->height;
        attr.enCompressMode = COMPRESS_MODE_NONE;
        attr.u32FrameBufCnt = 2;
        attr.u32Depth = 1;

        ret = RK_MPI_VPSS_SetChnAttr(VPSS_GRP_ID, VPSS_CHN_CAM1, &attr);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: CHN1 SetChnAttr failed: %#x\n", ret);
            return -1;
        }

        ret = RK_MPI_VPSS_EnableChn(VPSS_GRP_ID, VPSS_CHN_CAM1);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: CHN1 EnableChn failed: %#x\n", ret);
            return -1;
        }

        /* Crop: правая половина входного кадра (set AFTER EnableChn) */
        VPSS_CROP_INFO_S crop;
        memset(&crop, 0, sizeof(crop));
        crop.bEnable = RK_TRUE;
        crop.enCropCoordinate = VPSS_CROP_ABS_COOR;
        crop.stCropRect.s32X = ctx->width;
        crop.stCropRect.s32Y = 0;
        crop.stCropRect.u32Width = ctx->width;
        crop.stCropRect.u32Height = ctx->height;
        ret = RK_MPI_VPSS_SetChnCrop(VPSS_GRP_ID, VPSS_CHN_CAM1, &crop);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: CHN1 SetChnCrop failed: %#x\n", ret);
            return -1;
        }
        if (ctx->verbose) printf("vpss: CHN1 (cam1, crop %d,0,%d,%d) enabled\n",
                                 ctx->width, ctx->width, ctx->height);
    }

    /* CHN2: полный stitch (без crop) */
    {
        VPSS_CHN_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        attr.enChnMode = VPSS_CHN_MODE_USER;
        attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
        attr.enPixelFormat = RK_FMT_YUV420SP;
        attr.stFrameRate.s32SrcFrameRate = -1;
        attr.stFrameRate.s32DstFrameRate = -1;
        attr.u32Width = mega_w;
        attr.u32Height = mega_h;
        attr.enCompressMode = COMPRESS_MODE_NONE;
        attr.u32FrameBufCnt = 2;
        attr.u32Depth = 1;

        ret = RK_MPI_VPSS_SetChnAttr(VPSS_GRP_ID, VPSS_CHN_FULL, &attr);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: CHN2 SetChnAttr failed: %#x\n", ret);
            return -1;
        }
        /* Без crop — весь кадр */

        ret = RK_MPI_VPSS_EnableChn(VPSS_GRP_ID, VPSS_CHN_FULL);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "vpss: CHN2 EnableChn failed: %#x\n", ret);
            return -1;
        }
        if (ctx->verbose) printf("vpss: CHN2 (full stitch %dx%d) enabled\n", mega_w, mega_h);
    }

    /* SetVProcDev — аппаратный VPSS */
    ret = RK_MPI_VPSS_SetVProcDev(VPSS_GRP_ID, VIDEO_PROC_DEV_VPSS);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "vpss: SetVProcDev failed: %#x\n", ret);
        return -1;
    }

    ret = RK_MPI_VPSS_StartGrp(VPSS_GRP_ID);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "vpss: StartGrp failed: %#x\n", ret);
        return -1;
    }
    if (ctx->verbose) printf("vpss: StartGrp OK\n");

    return 0;
}

static void vpss_deinit(void) {
    RK_MPI_VPSS_DisableChn(VPSS_GRP_ID, VPSS_CHN_FULL);
    RK_MPI_VPSS_DisableChn(VPSS_GRP_ID, VPSS_CHN_CAM1);
    RK_MPI_VPSS_DisableChn(VPSS_GRP_ID, VPSS_CHN_CAM0);
    RK_MPI_VPSS_StopGrp(VPSS_GRP_ID);
    RK_MPI_VPSS_DestroyGrp(VPSS_GRP_ID);
}

/* ─── 5. bind_init — VI → AVS → VPSS ─── */

static int bind_init(StereoCtx *ctx) {
    int i, ret;

    /* Bind VI → AVS (для каждого сенсора) */
    for (i = 0; i < NUM_SENSORS; i++) {
        MPP_CHN_S vi_chn, avs_in_chn;
        vi_chn.enModId = RK_ID_VI;
        vi_chn.s32DevId = i;
        vi_chn.s32ChnId = 0;  /* MAINPATH */
        avs_in_chn.enModId = RK_ID_AVS;
        avs_in_chn.s32DevId = AVS_GRP_ID;
        avs_in_chn.s32ChnId = i;  /* AVS pipe id = sensor id */

        ret = RK_MPI_SYS_Bind(&vi_chn, &avs_in_chn);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "bind: VI[%d] -> AVS[%d] failed: %#x\n", i, i, ret);
            return -1;
        }
        if (ctx->verbose) printf("bind: VI[%d,0] -> AVS[%d,%d] OK\n", i, AVS_GRP_ID, i);
    }

    /* Bind AVS → VPSS (skip if --no-vpss) */
    if (!ctx->noVpss) {
        MPP_CHN_S avs_out, vpss_in;
        avs_out.enModId = RK_ID_AVS;
        avs_out.s32DevId = AVS_GRP_ID;
        avs_out.s32ChnId = AVS_CHN_ID;
        vpss_in.enModId = RK_ID_VPSS;
        vpss_in.s32DevId = VPSS_GRP_ID;
        vpss_in.s32ChnId = 0;  /* group input */

        ret = RK_MPI_SYS_Bind(&avs_out, &vpss_in);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "bind: AVS -> VPSS failed: %#x\n", ret);
            return -1;
        }
        if (ctx->verbose) printf("bind: AVS[%d,%d] -> VPSS[%d] OK\n",
                                 AVS_GRP_ID, AVS_CHN_ID, VPSS_GRP_ID);
    }

    return 0;
}

static void bind_deinit(StereoCtx *ctx) {
    int i;

    /* Unbind AVS → VPSS (only if VPSS was used) */
    if (!ctx->noVpss) {
        MPP_CHN_S avs_out, vpss_in;
        avs_out.enModId = RK_ID_AVS;
        avs_out.s32DevId = AVS_GRP_ID;
        avs_out.s32ChnId = AVS_CHN_ID;
        vpss_in.enModId = RK_ID_VPSS;
        vpss_in.s32DevId = VPSS_GRP_ID;
        vpss_in.s32ChnId = 0;
        RK_MPI_SYS_UnBind(&avs_out, &vpss_in);
    }

    /* Unbind VI → AVS */
    for (i = 0; i < NUM_SENSORS; i++) {
        MPP_CHN_S vi_chn, avs_in_chn;
        vi_chn.enModId = RK_ID_VI;
        vi_chn.s32DevId = i;
        vi_chn.s32ChnId = 0;
        avs_in_chn.enModId = RK_ID_AVS;
        avs_in_chn.s32DevId = AVS_GRP_ID;
        avs_in_chn.s32ChnId = i;
        RK_MPI_SYS_UnBind(&vi_chn, &avs_in_chn);
    }
}

/* ─── 6. save_frame — сохранение кадра в файл ─── */

static int save_frame_to_file(const char *filename,
                              VIDEO_FRAME_INFO_S *pFrame, int verbose) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "save: cannot open %s\n", filename);
        return -1;
    }

    RK_U8 *pData = (RK_U8 *)RK_MPI_MB_Handle2VirAddr(pFrame->stVFrame.pMbBlk);
    if (!pData) {
        fprintf(stderr, "save: Handle2VirAddr failed\n");
        fclose(fp);
        return -1;
    }

    int w = pFrame->stVFrame.u32Width;
    int h = pFrame->stVFrame.u32Height;
    /* NV12: Y plane + interleaved UV plane = w * h * 3 / 2 */
    int size = w * h * 3 / 2;
    fwrite(pData, 1, size, fp);
    fclose(fp);

    if (verbose) printf("save: %s (%dx%d, %d bytes)\n", filename, w, h, size);
    return 0;
}

/* ─── main ─── */

int main(int argc, char **argv) {
    StereoCtx ctx;
    int ret, frame;

    if (parse_args(&ctx, argc, argv) != 0)
        return 1;

    int mega_w = ctx.width * NUM_SENSORS;
    int mega_h = ctx.height;

    printf("stereo_demo: %dx%d per sensor, mega=%dx%d, sync=%d, camgroup=%d, ldch=%d\n",
           ctx.width, ctx.height, mega_w, mega_h,
           ctx.bSyncPipe, ctx.enableCamgroup, ctx.enableLdch);

    /* 1. RK_MPI_SYS_Init */
    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_SYS_Init failed: %#x\n", ret);
        return 1;
    }
    if (ctx.verbose) printf("RK_MPI_SYS_Init OK\n");

    /* 2. camgroup (3A sync) — ДО VI, т.к. настраивает ISP */
    ret = camgroup_init(&ctx);
    if (ret) goto cleanup_sys;

    /* 3. VI (cameras) */
    ret = vi_init(&ctx);
    if (ret) goto cleanup_camgroup;

    /* 4. AVS (sync + balance + LDCH) */
    ret = avs_init(&ctx);
    if (ret) goto cleanup_vi;

    /* 5. VPSS (fan-out: crop left/right/full) — skip if --no-vpss */
    if (!ctx.noVpss) {
        ret = vpss_init(&ctx);
        if (ret) goto cleanup_avs;
    } else if (ctx.verbose) {
        printf("vpss: skipped (--no-vpss, getting from AVS directly)\n");
    }

    /* 6. Bind: VI → AVS → VPSS (or just VI → AVS if --no-vpss) */
    ret = bind_init(&ctx);
    if (ret) goto cleanup_vpss;

    /* 7. Start AVS group */
    ret = RK_MPI_AVS_StartGrp(AVS_GRP_ID);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "AVS StartGrp failed: %#x\n", ret);
        goto cleanup_bind;
    }
    if (ctx.verbose) printf("AVS group started\n");

    /* 8. Захват кадров */
    printf("Waiting for stereo frames (sync=%d, this may take a few seconds)...\n",
           ctx.bSyncPipe);

    if (ctx.noVpss) {
        /* ── No-VPSS mode: get frame directly from AVS (like vi_grab_avs) ── */
        int total = ctx.skipFrames + ctx.frameCount;
        int saved = 0;
        int mega_w = ctx.width * NUM_SENSORS;
        int mega_h = ctx.height;

        for (frame = 0; frame < total; frame++) {
            VIDEO_FRAME_INFO_S stFrame;
            memset(&stFrame, 0, sizeof(stFrame));

            ret = RK_MPI_AVS_GetChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stFrame, ctx.timeoutMs);
            if (ret != RK_SUCCESS) {
                fprintf(stderr, "AVS GetChnFrame failed: %#x (frame %d)\n", ret, frame);
                continue;
            }

            if (frame < ctx.skipFrames) {
                RK_MPI_AVS_ReleaseChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stFrame);
                if (ctx.verbose) printf("skip frame %d\n", frame);
                continue;
            }

            char filename[256];
            snprintf(filename, sizeof(filename), "%s_mega_%05d_%dx%d.raw",
                     ctx.outputPrefix, saved,
                     stFrame.stVFrame.u32Width, stFrame.stVFrame.u32Height);
            save_frame_to_file(filename, &stFrame, ctx.verbose);
            saved++;

            RK_MPI_AVS_ReleaseChnFrame(AVS_GRP_ID, AVS_CHN_ID, &stFrame);
        }
        printf("Saved %d mega-frame(s) from AVS directly\n", saved);

    } else {
        /* ── Channel mode: GetChnFrame с каждого канала ── */
        int total = ctx.skipFrames + ctx.frameCount;
        int saved = 0;

        for (frame = 0; frame < total; frame++) {
            VIDEO_FRAME_INFO_S stCam0, stCam1, stFull;

            if (ctx.saveCam0) {
                memset(&stCam0, 0, sizeof(stCam0));
                ret = RK_MPI_VPSS_GetChnFrame(VPSS_GRP_ID, VPSS_CHN_CAM0,
                                              &stCam0, ctx.timeoutMs);
                if (ret != RK_SUCCESS) {
                    fprintf(stderr, "GetChnFrame CAM0 failed: %#x (frame %d)\n", ret, frame);
                }
            }
            if (ctx.saveCam1) {
                memset(&stCam1, 0, sizeof(stCam1));
                ret = RK_MPI_VPSS_GetChnFrame(VPSS_GRP_ID, VPSS_CHN_CAM1,
                                              &stCam1, ctx.timeoutMs);
                if (ret != RK_SUCCESS) {
                    fprintf(stderr, "GetChnFrame CAM1 failed: %#x (frame %d)\n", ret, frame);
                }
            }
            if (ctx.saveFull) {
                memset(&stFull, 0, sizeof(stFull));
                ret = RK_MPI_VPSS_GetChnFrame(VPSS_GRP_ID, VPSS_CHN_FULL,
                                              &stFull, ctx.timeoutMs);
                if (ret != RK_SUCCESS) {
                    fprintf(stderr, "GetChnFrame FULL failed: %#x (frame %d)\n", ret, frame);
                }
            }

            if (frame < ctx.skipFrames) {
                if (ctx.saveCam0 && stCam0.stVFrame.pMbBlk)
                    RK_MPI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_CAM0, &stCam0);
                if (ctx.saveCam1 && stCam1.stVFrame.pMbBlk)
                    RK_MPI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_CAM1, &stCam1);
                if (ctx.saveFull && stFull.stVFrame.pMbBlk)
                    RK_MPI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_FULL, &stFull);
                if (ctx.verbose) printf("skip frame %d\n", frame);
                continue;
            }

            char filename[256];
            if (ctx.saveCam0 && stCam0.stVFrame.pMbBlk) {
                snprintf(filename, sizeof(filename), "%s_cam0_%05d_%dx%d.raw",
                         ctx.outputPrefix, saved,
                         stCam0.stVFrame.u32Width, stCam0.stVFrame.u32Height);
                save_frame_to_file(filename, &stCam0, ctx.verbose);
                RK_MPI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_CAM0, &stCam0);
            }
            if (ctx.saveCam1 && stCam1.stVFrame.pMbBlk) {
                snprintf(filename, sizeof(filename), "%s_cam1_%05d_%dx%d.raw",
                         ctx.outputPrefix, saved,
                         stCam1.stVFrame.u32Width, stCam1.stVFrame.u32Height);
                save_frame_to_file(filename, &stCam1, ctx.verbose);
                RK_MPI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_CAM1, &stCam1);
            }
            if (ctx.saveFull && stFull.stVFrame.pMbBlk) {
                snprintf(filename, sizeof(filename), "%s_full_%05d_%dx%d.raw",
                         ctx.outputPrefix, saved,
                         stFull.stVFrame.u32Width, stFull.stVFrame.u32Height);
                save_frame_to_file(filename, &stFull, ctx.verbose);
                RK_MPI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_FULL, &stFull);
            }
            saved++;
        }
        printf("Saved %d frame set(s) via GetChnFrame\n", saved);
    }

    /* Cleanup */
cleanup_bind:
    bind_deinit(&ctx);
cleanup_vpss:
    if (!ctx.noVpss) vpss_deinit();
cleanup_avs:
    avs_deinit();
cleanup_vi:
    vi_deinit();
cleanup_camgroup:
    camgroup_deinit();
cleanup_sys:
    RK_MPI_SYS_Exit();

    if (ctx.verbose) printf("stereo_demo: cleanup done\n");
    return 0;
}
