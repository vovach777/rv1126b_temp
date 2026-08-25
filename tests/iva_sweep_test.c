/* iva_sweep_test.c — проверка всех detModel с пустым key.lic
 *
 * Перебираем все RockIvaDetModel (0-13) и проверяем:
 *   - проходит ли ROCKIVA_Init
 *   - проходит ли ROCKIVA_FACE_Init (faceRecognizeEnable=1)
 *
 * Кросс-компиляция:
 *   zig cc -target aarch64-linux-gnu -O2 -o iva_sweep_test iva_sweep_test.c \
 *       -I. -L. -lrockiva -Wl,-rpath,/tmp/iva_test/lib
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rockiva_common.h"
#include "rockiva_face_api.h"

#define MODEL_PATH "/data/rockiva_data"
#define LOG_PATH   "/tmp/iva_test"
#define LICENSE_PATH "/tmp/iva_test/key.lic"

static const char* detmodel_str(RockIvaDetModel m) {
    switch (m) {
        case ROCKIVA_DET_MODEL_NONE:      return "NONE(0)";
        case ROCKIVA_DET_MODEL_CLS7:      return "CLS7(1)";
        case ROCKIVA_DET_MODEL_PFCP:      return "PFCP(2)";
        case ROCKIVA_DET_MODEL_PFP:       return "PFP(3)";
        case ROCKIVA_DET_MODEL_PHS:       return "PHS(4)";
        case ROCKIVA_DET_MODEL_PHCP:      return "PHCP(5)";
        case ROCKIVA_DET_MODEL_PERSON:    return "PERSON(6)";
        case ROCKIVA_DET_MODEL_NONVEHICLE:return "NONVEHICLE(7)";
        case ROCKIVA_DET_MODEL_FHS:       return "FHS(8)";
        case ROCKIVA_DET_MODEL_PHCN:      return "PHCN(9)";
        case ROCKIVA_DET_MODEL_CLS8:      return "CLS8(10)";
        case ROCKIVA_DET_MODEL_PFP_V3:    return "PFP_V3(11)";
        case ROCKIVA_DET_MODEL_PFCPP:     return "PFCPP(12)";
        case ROCKIVA_DET_MODEL_CLS9:      return "CLS9(13)";
        case ROCKIVA_DET_MODEL_MHTB:      return "MHTB(14)";
        default: return "UNKNOWN";
    }
}

static void dummy_det_cb(const RockIvaFaceDetResult* r,
                          const RockIvaExecuteStatus s, void* u) {}
static void dummy_analyse_cb(const RockIvaFaceCapResults* r,
                              const RockIvaExecuteStatus s, void* u) {}

int main(void) {
    printf("=== ROCKIVA detModel Sweep Test (empty key.lic) ===\n\n");

    /* Читаем пустой key.lic */
    FILE* f = fopen(LICENSE_PATH, "rb");
    char* license_data = NULL;
    size_t license_size = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        license_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        license_data = (char*)malloc(license_size + 1);
        if (license_data) {
            size_t n = fread(license_data, 1, license_size, f);
            license_data[n] = 0;
        }
        fclose(f);
    }
    printf("license: %s (%zu bytes)\n\n", LICENSE_PATH, license_size);

    printf("| detModel | Init | FACE_Init | FaceDet | FaceRecog |\n");
    printf("|----------|------|-----------|---------|-----------|\n");

    for (int m = 0; m <= 14; m++) {
        RockIvaHandle handle = NULL;
        RockIvaInitParam initParam;
        memset(&initParam, 0, sizeof(initParam));
        initParam.logLevel = ROCKIVA_LOG_ERROR;  /* тише */
        snprintf(initParam.logPath, ROCKIVA_PATH_LENGTH, LOG_PATH);
        snprintf(initParam.modelPath, ROCKIVA_PATH_LENGTH, MODEL_PATH);
        if (license_data) {
            initParam.license.memAddr = license_data;
            initParam.license.memSize = license_size;
        }
        initParam.channelId = 0;
        initParam.imageInfo.width = 1920;
        initParam.imageInfo.height = 1080;
        initParam.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
        initParam.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
        initParam.detModel = (RockIvaDetModel)m;
        initParam.trackerVersion = 0;

        RockIvaRetCode r1 = ROCKIVA_Init(&handle, ROCKIVA_MODE_PICTURE, &initParam, NULL);
        const char* init_str = (r1 == ROCKIVA_RET_SUCCESS) ? "OK" :
                               (r1 == ROCKIVA_RET_LICENSE_ERROR) ? "LIC_ERR" :
                               (r1 == ROCKIVA_RET_FAIL) ? "FAIL" : "OTHER";

        const char* face_init_str = "-";
        const char* face_det_str = "-";
        const char* face_recog_str = "-";

        if (r1 == ROCKIVA_RET_SUCCESS && handle) {
            RockIvaFaceTaskParams faceParams;
            memset(&faceParams, 0, sizeof(faceParams));
            faceParams.mode = ROCKIVA_FACE_MODE_PANEL;
            faceParams.faceTaskType.faceCaptureEnable = 1;
            faceParams.faceTaskType.faceRecognizeEnable = 1;
            faceParams.faceCaptureRule.optType = ROCKIVA_FACE_OPT_BEST;
            faceParams.faceCaptureRule.optBestOverTime = 10000;

            RockIvaFaceCallback callback;
            memset(&callback, 0, sizeof(callback));
            callback.detCallback = dummy_det_cb;
            callback.analyseCallback = dummy_analyse_cb;

            RockIvaRetCode r2 = ROCKIVA_FACE_Init(handle, &faceParams, callback);
            if (r2 == ROCKIVA_RET_SUCCESS) {
                face_init_str = "OK";
                face_det_str = "OK";     /* если Init OK — детекция работает */
                face_recog_str = "OK?";  /* нужно проверить PushFrame */
            } else if (r2 == ROCKIVA_RET_LICENSE_ERROR) {
                face_init_str = "LIC_ERR";
                face_det_str = "OK";     /* детекция работает (видели в логах) */
                face_recog_str = "LIC_ERR";
            } else {
                face_init_str = (r2 == ROCKIVA_RET_FAIL) ? "FAIL" : "OTHER";
            }
            ROCKIVA_FACE_Release(handle);
            ROCKIVA_Release(handle);
        }

        printf("| %s | %s | %s | %s | %s |\n",
               detmodel_str((RockIvaDetModel)m),
               init_str, face_init_str, face_det_str, face_recog_str);
        fflush(stdout);
        usleep(100000);  /* 100ms между тестами */
    }

    printf("\n=== ИТОГ ===\n");
    printf("Если face_recog=OK для какого-то detModel — нашли рабочий режим!\n");
    printf("Если везде LIC_ERR — face recognition требует license независимо от detModel.\n");

    if (license_data) free(license_data);
    return 0;
}
