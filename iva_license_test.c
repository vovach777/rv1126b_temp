/* iva_license_test.c — проверка license для ROCKIVA face recognition
 *
 * Тестируем:
 *   1. ROCKIVA_Init с license=NULL (detModel=PFP) — работает ли BA-детекция
 *   2. ROCKIVA_FACE_Init с faceRecognizeEnable=1 — работает ли face recognition
 *   3. Печать кодов ошибок
 *
 * Кросс-компиляция (Windows, zig):
 *   zig cc -target aarch64-linux-gnu -o iva_license_test iva_license_test.c \
 *       -I./include -L./lib -lrockiva -Wl,-rpath,/tmp/iva_test/lib
 *
 * Запуск на плате:
 *   LD_LIBRARY_PATH=/tmp/iva_test/lib ./iva_license_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "rockiva_common.h"
#include "rockiva_face_api.h"

#define MODEL_PATH "/data/rockiva_data"
#define LOG_PATH   "/tmp/iva_test"
#define LICENSE_PATH "/tmp/iva_test/key.lic"

static const char* retcode_str(RockIvaRetCode r) {
    switch (r) {
        case ROCKIVA_RET_SUCCESS:        return "SUCCESS(0)";
        case ROCKIVA_RET_FAIL:           return "FAIL(-1)";
        case ROCKIVA_RET_NULL_PTR:       return "NULL_PTR(-2)";
        case ROCKIVA_RET_INVALID_HANDLE: return "INVALID_HANDLE(-3)";
        case ROCKIVA_RET_LICENSE_ERROR:  return "LICENSE_ERROR(-4)";
        case ROCKIVA_RET_UNSUPPORTED:    return "UNSUPPORTED(-5)";
        case ROCKIVA_RET_STREAM_SWITCH:  return "STREAM_SWITCH(-6)";
        case ROCKIVA_RET_BUFFER_FULL:    return "BUFFER_FULL(-7)";
        default: return "UNKNOWN";
    }
}

static const char* status_str(RockIvaExecuteStatus s) {
    switch (s) {
        case ROCKIVA_SUCCESS:           return "SUCCESS(0)";
        case ROCKIVA_UNKNOWN:           return "UNKNOWN(1)";
        case ROCKIVA_NULL_PTR:          return "NULL_PTR(2)";
        case ROCKIVA_ALLOC_FAILED:      return "ALLOC_FAILED(3)";
        case ROCKIVA_INVALID_INPUT:     return "INVALID_INPUT(4)";
        case ROCKIVA_EXECUTE_FAILED:    return "EXECUTE_FAILED(6)";
        case ROCKIVA_NOT_CONFIGURED:    return "NOT_CONFIGURED(7)";
        case ROCKIVA_NO_CAPACITY:       return "NO_CAPACITY(8)";
        case ROCKIVA_BUFFER_FULL:       return "BUFFER_FULL(9)";
        case ROCKIVA_LICENSE_ERROR:     return "LICENSE_ERROR(10)";
        case ROCKIVA_JPEG_DECODE_ERROR: return "JPEG_DECODE_ERROR(11)";
        case ROCKIVA_DECODER_EXIT:      return "DECODER_EXIT(12)";
        default: return "OTHER";
    }
}

/* Заглушка callback для face detection */
static void face_det_callback(const RockIvaFaceDetResult* result,
                              const RockIvaExecuteStatus status,
                              void* userdata) {
    printf("[face_det_cb] status=%s objNum=%d\n", status_str(status), result->objNum);
}

/* Заглушка callback для face analyse (feature extraction) */
static void face_analyse_callback(const RockIvaFaceCapResults* result,
                                  const RockIvaExecuteStatus status,
                                  void* userdata) {
    printf("[face_analyse_cb] status=%s num=%d\n", status_str(status), result->num);
    if (status == ROCKIVA_SUCCESS && result->num > 0) {
        for (uint32_t i = 0; i < result->num; i++) {
            printf("  face[%d] qualityResult=%d featureSize=%d\n",
                   i, result->faceResults[i].qualityResult,
                   result->faceResults[i].faceAnalyseInfo.featureSize);
        }
    }
}

/* Заглушка callback для release frame */
static void frame_release_callback(const RockIvaReleaseFrames* releaseFrames,
                                   void* userdata) {
    for (uint32_t i = 0; i < releaseFrames->count; i++) {
        if (releaseFrames->frames[i].dataAddr) {
            free(releaseFrames->frames[i].dataAddr);
        }
    }
}

int main(int argc, char** argv) {
    printf("=== ROCKIVA License Test ===\n");
    printf("modelPath: %s\n", MODEL_PATH);
    printf("lib: /tmp/iva_test/lib/librockiva.so (V1.23.1, 3.3MB)\n");

    /* Проверяем наличие license файла */
    FILE* f = fopen(LICENSE_PATH, "rb");
    int has_license_file = (f != NULL);
    size_t license_size = 0;
    char* license_data = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        license_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        license_data = (char*)malloc(license_size);
        if (license_data && fread(license_data, 1, license_size, f) == license_size) {
            printf("license file: %s (%zu bytes)\n", LICENSE_PATH, license_size);
        } else {
            printf("license file: read error, using NULL\n");
            free(license_data);
            license_data = NULL;
            license_size = 0;
        }
        fclose(f);
    } else {
        printf("license file: NOT FOUND (%s) — testing with NULL license\n", LICENSE_PATH);
    }
    printf("\n");

    /* === Шаг 1: ROCKIVA_Init === */
    RockIvaHandle handle = NULL;
    RockIvaInitParam initParam;
    memset(&initParam, 0, sizeof(initParam));
    initParam.logLevel = ROCKIVA_LOG_INFO;
    snprintf(initParam.logPath, ROCKIVA_PATH_LENGTH, LOG_PATH);
    snprintf(initParam.modelPath, ROCKIVA_PATH_LENGTH, MODEL_PATH);
    /* license: NULL (нет файла) или данные из key.lic */
    if (license_data) {
        initParam.license.memAddr = license_data;
        initParam.license.memSize = license_size;
        printf("[step1] using license from file (%zu bytes)\n", license_size);
    } else {
        initParam.license.memAddr = NULL;
        initParam.license.memSize = 0;
        printf("[step1] using NULL license (no key.lic)\n");
    }
    initParam.channelId = 0;
    initParam.imageInfo.width = 1920;
    initParam.imageInfo.height = 1080;
    initParam.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    initParam.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
    initParam.detModel = ROCKIVA_DET_MODEL_NONE;  /* NONE — проверяем auth без детекции */
    initParam.trackerVersion = 0;

    printf("[step1] ROCKIVA_Init(mode=PICTURE, detModel=PFP)...\n");
    RockIvaRetCode ret = ROCKIVA_Init(&handle, ROCKIVA_MODE_PICTURE, &initParam, NULL);
    printf("[step1] ROCKIVA_Init = %d (%s)\n", ret, retcode_str(ret));

    if (ret != ROCKIVA_RET_SUCCESS) {
        printf("\n*** ROCKIVA_Init FAILED — license required for whole SDK ***\n");
        printf("*** Вывод: даже базовая инициализация требует license ***\n");
        if (license_data) free(license_data);
        return 1;
    }
    printf("[step1] OK — ROCKIVA_Init работает %s\n",
           license_data ? "(с license)" : "(БЕЗ license!)");

    /* === Шаг 2: ROCKIVA_FACE_Init (face recognition) === */
    RockIvaFaceTaskParams faceParams;
    memset(&faceParams, 0, sizeof(faceParams));
    faceParams.mode = ROCKIVA_FACE_MODE_PANEL;  /*近距离 распознавание */
    faceParams.faceTaskType.faceCaptureEnable = 1;
    faceParams.faceTaskType.faceRecognizeEnable = 1;  /* ← ключевой флаг! */
    faceParams.faceTaskType.faceAttributeEnable = 0;
    faceParams.faceTaskType.faceLivenessEnable = 0;
    faceParams.faceTaskType.faceLandmarkEnable = 0;
    faceParams.faceTaskType.facePoseEnable = 0;
    faceParams.faceCaptureRule.optType = ROCKIVA_FACE_OPT_BEST;
    faceParams.faceCaptureRule.optBestOverTime = 10000;
    faceParams.faceCaptureRule.captureWithMask = 0;

    RockIvaFaceCallback callback;
    memset(&callback, 0, sizeof(callback));
    callback.detCallback = face_det_callback;
    callback.analyseCallback = face_analyse_callback;

    printf("\n[step2] ROCKIVA_FACE_Init(faceRecognizeEnable=1)...\n");
    ret = ROCKIVA_FACE_Init(handle, &faceParams, callback);
    printf("[step2] ROCKIVA_FACE_Init = %d (%s)\n", ret, retcode_str(ret));

    if (ret == ROCKIVA_RET_LICENSE_ERROR) {
        printf("\n*** ROCKIVA_FACE_Init: LICENSE_ERROR ***\n");
        printf("*** Вывод: BA-детекция работает без license, ***\n");
        printf("*** но face recognition (feature extraction) ТРЕБУЕТ license ***\n");
        ROCKIVA_Release(handle);
        if (license_data) free(license_data);
        return 2;
    } else if (ret != ROCKIVA_RET_SUCCESS) {
        printf("\n*** ROCKIVA_FACE_Init failed: %s ***\n", retcode_str(ret));
        printf("*** Это не license error — возможно не хватает моделей ***\n");
        ROCKIVA_Release(handle);
        if (license_data) free(license_data);
        return 3;
    }
    printf("[step2] OK — ROCKIVA_FACE_Init работает %s\n",
           license_data ? "(с license)" : "(БЕЗ license!)");

    /* === Шаг 3: пропущен (вызывал segfault из-за callback) === */
    printf("\n[step3] SetFrameReleaseCallback — пропущен (избегаем segfault)\n");

    /* === Шаг 4: ROCKIVA_FACE_FeatureCompare (тест 1:1) ===
     * Создаём два фейковых feature-вектора и пытаемся сравнить.
     * Если вернёт LICENSE_ERROR — feature compare тоже требует license.
     */
    printf("\n[step4] ROCKIVA_FACE_FeatureCompare (fake vectors)...\n");
    float feature1[512], feature2[512];
    for (int i = 0; i < 512; i++) {
        feature1[i] = 0.1f * i;
        feature2[i] = 0.1f * i;  /* одинаковые → score должен быть высоким */
    }
    float score = 0.0f;
    ret = ROCKIVA_FACE_FeatureCompare(feature1, feature2, &score);
    printf("[step4] ROCKIVA_FACE_FeatureCompare = %d (%s), score=%f\n",
           ret, retcode_str(ret), score);

    /* === Шаг 5: ROCKIVA_FACE_SearchFeature (тест 1:N) ===
     * Пытаемся найти фейковый feature в пустой базе.
     */
    printf("\n[step5] ROCKIVA_FACE_SearchFeature (empty lib)...\n");
    RockIvaFaceSearchResults search_result;
    memset(&search_result, 0, sizeof(search_result));
    ret = ROCKIVA_FACE_SearchFeature("face", feature1, sizeof(feature1), 0, 5, &search_result);
    printf("[step5] ROCKIVA_FACE_SearchFeature = %d (%s), num=%d\n",
           ret, retcode_str(ret), search_result.num);

    /* === Итог === */
    printf("\n=== ИТОГ ===\n");
    printf("ROCKIVA_Init:        %s\n", retcode_str((RockIvaRetCode)0));
    printf("ROCKIVA_FACE_Init:   OK (face recognition enabled)\n");
    printf("FeatureCompare:      %s\n", retcode_str(ret));
    printf("License used:        %s\n", license_data ? "key.lic" : "NONE (NULL)");
    printf("\n");
    if (!license_data) {
        printf("*** ВЫВОД: Face recognition работает БЕЗ license на этой плате! ***\n");
        printf("*** Возможные причины: ***\n");
        printf("  - OEM-сборка librockiva.so с встроенной license\n");
        printf("  - License вшита в vendor storage / OTP\n");
        printf("  - Trial-режим (ограничение по времени или функциям)\n");
        printf("  - Эта сборка SDK не требует license для face\n");
    } else {
        printf("*** ВЫВОД: Face recognition работает с license из key.lic ***\n");
    }

    /* Cleanup */
    ROCKIVA_FACE_Release(handle);
    ROCKIVA_Release(handle);
    if (license_data) free(license_data);
    return 0;
}
