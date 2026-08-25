/* iva_license_modes.c — как API использует license: содержимое или файл?
 *
 * 4 теста:
 *   1. license=NULL, файла key.lic нет
 *   2. license=NULL, пустой key.lic есть (0 байт)
 *   3. license=пустое содержимое (memSize=0, memAddr=NULL) + файл есть
 *   4. license=мусор (128 байт "AAAA...") + файл есть
 *   5. license=мусор, файла НЕТ
 *
 * Кросс-компиляция:
 *   zig cc -target aarch64-linux-gnu -O2 -o iva_license_modes iva_license_modes.c \
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

static const char* retcode_str(RockIvaRetCode r) {
    switch (r) {
        case ROCKIVA_RET_SUCCESS:        return "SUCCESS(0)";
        case ROCKIVA_RET_FAIL:           return "FAIL(-1)";
        case ROCKIVA_RET_LICENSE_ERROR:  return "LIC_ERR(-4)";
        case ROCKIVA_RET_UNSUPPORTED:    return "UNSUPP(-5)";
        default: { static char b[16]; snprintf(b, sizeof(b), "OTHER(%d)", r); return b; }
    }
}

static void dummy_det_cb(const RockIvaFaceDetResult* r,
                          const RockIvaExecuteStatus s, void* u) {}
static void dummy_analyse_cb(const RockIvaFaceCapResults* r,
                              const RockIvaExecuteStatus s, void* u) {}

static int test_init(const char* name, void* lic_addr, size_t lic_size,
                     int detModel, int face_recog) {
    RockIvaHandle handle = NULL;
    RockIvaInitParam initParam;
    memset(&initParam, 0, sizeof(initParam));
    initParam.logLevel = ROCKIVA_LOG_ERROR;
    snprintf(initParam.logPath, ROCKIVA_PATH_LENGTH, LOG_PATH);
    snprintf(initParam.modelPath, ROCKIVA_PATH_LENGTH, MODEL_PATH);
    initParam.license.memAddr = lic_addr;
    initParam.license.memSize = lic_size;
    initParam.channelId = 0;
    initParam.imageInfo.width = 1920;
    initParam.imageInfo.height = 1080;
    initParam.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    initParam.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
    initParam.detModel = (RockIvaDetModel)detModel;
    initParam.trackerVersion = 0;

    RockIvaRetCode r1 = ROCKIVA_Init(&handle, ROCKIVA_MODE_PICTURE, &initParam, NULL);
    printf("[%s] ROCKIVA_Init = %s\n", name, retcode_str(r1));

    if (r1 == ROCKIVA_RET_SUCCESS && handle && face_recog) {
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
        printf("[%s] ROCKIVA_FACE_Init = %s\n", name, retcode_str(r2));
        ROCKIVA_FACE_Release(handle);
    }
    if (handle) ROCKIVA_Release(handle);
    printf("\n");
    return (r1 == ROCKIVA_RET_SUCCESS) ? 0 : 1;
}

int main(void) {
    printf("=== ROCKIVA License Modes Test ===\n\n");

    /* Удаляем key.lic */
    unlink(LICENSE_PATH);
    printf("--- Тест 1: license=NULL, файла НЕТ, detModel=NONE ---\n");
    test_init("NULL-no-file-NONE", NULL, 0, ROCKIVA_DET_MODEL_NONE, 1);

    printf("--- Тест 2: license=NULL, файла НЕТ, detModel=PFP ---\n");
    test_init("NULL-no-file-PFP", NULL, 0, ROCKIVA_DET_MODEL_PFP, 1);

    /* Создаём пустой key.lic */
    FILE* f = fopen(LICENSE_PATH, "wb");
    if (f) fclose(f);
    printf("--- Тест 3: license=NULL, пустой файл ЕСТЬ, detModel=NONE ---\n");
    test_init("NULL-empty-file-NONE", NULL, 0, ROCKIVA_DET_MODEL_NONE, 1);

    printf("--- Тест 4: license=NULL, пустой файл ЕСТЬ, detModel=PFP ---\n");
    test_init("NULL-empty-file-PFP", NULL, 0, ROCKIVA_DET_MODEL_PFP, 1);

    /* Тест 5: передаём пустое содержимое (memSize=0) */
    printf("--- Тест 5: license=empty mem (size=0), файл есть, detModel=NONE ---\n");
    test_init("empty-mem-NONE", NULL, 0, ROCKIVA_DET_MODEL_NONE, 1);

    /* Тест 6: мусор в license (128 байт 'A') */
    char garbage[128];
    memset(garbage, 'A', sizeof(garbage));
    printf("--- Тест 6: license=garbage(128 'A'), файл есть, detModel=NONE ---\n");
    test_init("garbage-NONE", garbage, sizeof(garbage), ROCKIVA_DET_MODEL_NONE, 1);

    printf("--- Тест 7: license=garbage(128 'A'), файл есть, detModel=PFP ---\n");
    test_init("garbage-PFP", garbage, sizeof(garbage), ROCKIVA_DET_MODEL_PFP, 1);

    /* Тест 8: мусор, файла НЕТ */
    unlink(LICENSE_PATH);
    printf("--- Тест 8: license=garbage(128 'A'), файла НЕТ, detModel=NONE ---\n");
    test_init("garbage-no-file-NONE", garbage, sizeof(garbage), ROCKIVA_DET_MODEL_NONE, 1);

    /* Тест 9: ROCKX_LICENCE_KEY env var */
    printf("--- Тест 9: ROCKX_LICENCE_KEY=garbage, файла НЕТ, detModel=NONE ---\n");
    setenv("ROCKX_LICENCE_KEY", garbage, 1);
    test_init("env-garbage-NONE", NULL, 0, ROCKIVA_DET_MODEL_NONE, 1);
    unsetenv("ROCKX_LICENCE_KEY");

    /* Тест 10: путь к файлу в license.memAddr (строка) */
    printf("--- Тест 10: license=path string, файла НЕТ, detModel=NONE ---\n");
    test_init("path-str-NONE", (void*)LICENSE_PATH, strlen(LICENSE_PATH), ROCKIVA_DET_MODEL_NONE, 1);

    printf("=== ИТОГ ===\n");
    printf("Если тест с garbage прошёл OK — API не проверяет содержимое license!\n");
    printf("Если тест с env прошёл OK — lib читает ROCKX_LICENCE_KEY!\n");
    return 0;
}
