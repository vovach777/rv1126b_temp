/* iva_det_sweep.c — какие detModel работают без license (только детекция)
 *
 * memAddr=NULL + пустой key.lic → lib читает файл сама
 * Проверяем только ROCKIVA_Init (без FACE_Init — recognition не нужен)
 *
 * Кросс-компиляция:
 *   zig cc -target aarch64-linux-gnu -O2 -o iva_det_sweep iva_det_sweep.c \
 *       -I. -L. -lrockiva -Wl,-rpath,/tmp/iva_test/lib
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rockiva_common.h"
#include "rockiva_face_api.h"
#include "rockiva_det_api.h"
#include "rockiva_object_api.h"

#define MODEL_PATH "/data/rockiva_data"
#define LOG_PATH   "/tmp/iva_test"
#define LICENSE_PATH "/tmp/iva_test/key.lic"

static const char* detmodel_str(RockIvaDetModel m) {
    switch (m) {
        case ROCKIVA_DET_MODEL_NONE:      return "NONE     ";
        case ROCKIVA_DET_MODEL_CLS7:      return "CLS7     ";
        case ROCKIVA_DET_MODEL_PFCP:      return "PFCP     ";
        case ROCKIVA_DET_MODEL_PFP:       return "PFP      ";
        case ROCKIVA_DET_MODEL_PHS:       return "PHS      ";
        case ROCKIVA_DET_MODEL_PHCP:      return "PHCP     ";
        case ROCKIVA_DET_MODEL_PERSON:    return "PERSON   ";
        case ROCKIVA_DET_MODEL_NONVEHICLE:return "NONVEH   ";
        case ROCKIVA_DET_MODEL_FHS:       return "FHS      ";
        case ROCKIVA_DET_MODEL_PHCN:      return "PHCN     ";
        case ROCKIVA_DET_MODEL_CLS8:      return "CLS8     ";
        case ROCKIVA_DET_MODEL_PFP_V3:    return "PFP_V3   ";
        case ROCKIVA_DET_MODEL_PFCPP:     return "PFCPP    ";
        case ROCKIVA_DET_MODEL_CLS9:      return "CLS9     ";
        case ROCKIVA_DET_MODEL_MHTB:      return "MHTB     ";
        default: return "UNKNOWN  ";
    }
}

static const char* detmodel_classes(RockIvaDetModel m) {
    switch (m) {
        case ROCKIVA_DET_MODEL_NONE:      return "нет детекции";
        case ROCKIVA_DET_MODEL_CLS7:      return "person,face,car,plate,bike,pet";
        case ROCKIVA_DET_MODEL_PFCP:      return "person,face,car,pet";
        case ROCKIVA_DET_MODEL_PFP:       return "person,face,pet";
        case ROCKIVA_DET_MODEL_PHS:       return "person,head-shoulders";
        case ROCKIVA_DET_MODEL_PHCP:      return "person,head-shoulders,car,pet";
        case ROCKIVA_DET_MODEL_PERSON:    return "person";
        case ROCKIVA_DET_MODEL_NONVEHICLE:return "e-bike";
        case ROCKIVA_DET_MODEL_FHS:       return "face,head-shoulders";
        case ROCKIVA_DET_MODEL_PHCN:      return "person,head-shoulders,car,bike";
        case ROCKIVA_DET_MODEL_CLS8:      return "CLS7+head";
        case ROCKIVA_DET_MODEL_PFP_V3:    return "person,face,pet (v3)";
        case ROCKIVA_DET_MODEL_PFCPP:     return "person,face,car,pet,parcel";
        case ROCKIVA_DET_MODEL_CLS9:      return "CLS8+parcel";
        case ROCKIVA_DET_MODEL_MHTB:      return "e-bike parts";
        default: return "?";
    }
}

static void dummy_obj_cb(const RockIvaDetectResult* r,
                          const RockIvaExecuteStatus s, void* u) {}

int main(void) {
    printf("=== ROCKIVA detModel Sweep (det only, empty key.lic) ===\n\n");

    printf("| detModel   | классы                          | Init  |\n");
    printf("|------------|---------------------------------|-------|\n");

    /* Warmup: VIDEO PFP FAIL — сбрасывает auth state для последующих PICTURE вызовов */
    {
        RockIvaHandle h = NULL;
        RockIvaInitParam p;
        memset(&p, 0, sizeof(p));
        p.logLevel = ROCKIVA_LOG_ERROR;
        snprintf(p.logPath, ROCKIVA_PATH_LENGTH, LOG_PATH);
        snprintf(p.modelPath, ROCKIVA_PATH_LENGTH, MODEL_PATH);
        p.imageInfo.width = 1920;
        p.imageInfo.height = 1080;
        p.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
        p.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
        p.detModel = ROCKIVA_DET_MODEL_PFP;
        RockIvaRetCode r = ROCKIVA_Init(&h, ROCKIVA_MODE_VIDEO, &p, NULL);
        printf("[warmup VIDEO PFP] = %d (ожидаем FAIL)\n\n", r);
        if (r == 0 && h) ROCKIVA_Release(h);
    }

    for (int m = 0; m <= 14; m++) {
        unlink(LICENSE_PATH);

        RockIvaHandle handle = NULL;
        RockIvaInitParam initParam;
        memset(&initParam, 0, sizeof(initParam));
        initParam.logLevel = ROCKIVA_LOG_ERROR;
        snprintf(initParam.logPath, ROCKIVA_PATH_LENGTH, LOG_PATH);
        snprintf(initParam.modelPath, ROCKIVA_PATH_LENGTH, MODEL_PATH);
        /* KEY: memAddr=NULL → lib читает key.lic сама (пустой файл) */
        initParam.license.memAddr = NULL;
        initParam.license.memSize = 0;
        initParam.channelId = 0;
        initParam.imageInfo.width = 1920;
        initParam.imageInfo.height = 1080;
        initParam.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
        initParam.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
        initParam.detModel = (RockIvaDetModel)m;
        initParam.trackerVersion = 0;

        RockIvaRetCode r1 = ROCKIVA_Init(&handle, ROCKIVA_MODE_PICTURE, &initParam, NULL);
        /* PICTURE mode — обходит auth check (в отличие от VIDEO) */
        const char* status = (r1 == ROCKIVA_RET_SUCCESS) ? "✅ OK" :
                             (r1 == ROCKIVA_RET_LICENSE_ERROR) ? "❌ LIC" :
                             (r1 == ROCKIVA_RET_FAIL) ? "❌ FAIL" : "❌ OTHER";

        printf("| %s | %-31s | %s |\n",
               detmodel_str((RockIvaDetModel)m),
               detmodel_classes((RockIvaDetModel)m),
               status);
        fflush(stdout);

        if (r1 == ROCKIVA_RET_SUCCESS && handle) {
            ROCKIVA_Release(handle);
        }
        usleep(200000);
    }

    printf("\n=== ИТОГ ===\n");
    printf("✅ OK = детекция работает без license (с пустым key.lic)\n");
    printf("❌ LIC/FAIL = требует license\n");
    return 0;
}
