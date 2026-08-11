/* iva_reid_test.c — тест Person Re-ID без license
 *
 * Person Re-ID — распознавание людей по силуэту (не по лицу)
 * Проверяем: ROCKIVA_REID_Init, ExtractImageFeature, FeatureCompare
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rockiva_common.h"
#include "rockiva_reid_api.h"
#include "rockiva_image.h"

#define MODEL_PATH "/data/rockiva_data"
#define LOG_PATH   "/tmp/iva_test"
#define LICENSE_PATH "/tmp/iva_test/key.lic"

int main(void) {
    printf("=== Person Re-ID Test (no license) ===\n\n");

    /* Создаём пустой key.lic */
    FILE* f = fopen(LICENSE_PATH, "wb");
    if (f) fclose(f);

    /* Warmup — как для PFP */
    printf("[1] Warmup: ROCKIVA_Init(VIDEO, PFP)...\n");
    {
        RockIvaHandle h = NULL;
        RockIvaInitParam p;
        memset(&p, 0, sizeof(p));
        p.logLevel = ROCKIVA_LOG_ERROR;
        snprintf(p.logPath, 128, LOG_PATH);
        snprintf(p.modelPath, 128, MODEL_PATH);
        p.imageInfo.width = 1920;
        p.imageInfo.height = 1080;
        p.imageInfo.format = ROCKIVA_IMAGE_FORMAT_RGB888;
        p.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
        p.detModel = ROCKIVA_DET_MODEL_PFP;
        RockIvaRetCode r = ROCKIVA_Init(&h, ROCKIVA_MODE_VIDEO, &p, NULL);
        printf("    warmup = %d (ожидаем FAIL)\n", r);
        if (r == 0 && h) ROCKIVA_Release(h);
    }

    /* Real init */
    printf("\n[2] ROCKIVA_Init(PICTURE, PFP)...\n");
    RockIvaHandle handle = NULL;
    RockIvaInitParam initParam;
    memset(&initParam, 0, sizeof(initParam));
    initParam.logLevel = ROCKIVA_LOG_INFO;
    snprintf(initParam.logPath, 128, LOG_PATH);
    snprintf(initParam.modelPath, 128, MODEL_PATH);
    initParam.imageInfo.width = 1920;
    initParam.imageInfo.height = 1080;
    initParam.imageInfo.format = ROCKIVA_IMAGE_FORMAT_RGB888;
    initParam.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
    initParam.detModel = ROCKIVA_DET_MODEL_PFP;

    RockIvaRetCode r1 = ROCKIVA_Init(&handle, ROCKIVA_MODE_PICTURE, &initParam, NULL);
    printf("    ROCKIVA_Init = %d\n", r1);
    if (r1 != ROCKIVA_RET_SUCCESS) {
        printf("FAILED\n");
        return 1;
    }

    /* REID_Init */
    printf("\n[3] ROCKIVA_REID_Init...\n");
    RockIvaReIDTaskParams reidParams;
    memset(&reidParams, 0, sizeof(reidParams));
    reidParams.personReIDRule.enable = 1;
    reidParams.personReIDRule.mode = 0;  /* image-based retrieval */

    RockIvaReIDResultCallback callback;
    memset(&callback, 0, sizeof(callback));

    RockIvaRetCode r2 = ROCKIVA_REID_Init(handle, &reidParams, callback);
    printf("    REID_Init = %d\n", r2);
    if (r2 != ROCKIVA_RET_SUCCESS) {
        printf("FAILED — REID требует license?\n");
        ROCKIVA_Release(handle);
        return 1;
    }

    /* ExtractImageFeature — пробуем с тестовым изображением */
    printf("\n[4] ROCKIVA_REID_ExtractImageFeature (test image)...\n");
    RockIvaImage image;
    memset(&image, 0, sizeof(image));
    image.info.width = 1920;
    image.info.height = 1080;
    image.info.format = ROCKIVA_IMAGE_FORMAT_RGB888;
    size_t img_size = 1920 * 1080 * 3;
    image.dataAddr = (uint8_t*)malloc(img_size);
    image.size = img_size;
    memset(image.dataAddr, 128, img_size);  /* серый */

    RockIvaObjectReidentityFeature feature;
    memset(&feature, 0, sizeof(feature));

    RockIvaRetCode r3 = ROCKIVA_REID_ExtractImageFeature(handle, &image, 0, &feature);
    printf("    ExtractImageFeature = %d\n", r3);
    if (r3 == ROCKIVA_RET_SUCCESS) {
        printf("    feature.size = %d\n", feature.size);
        printf("    ✅ Person Re-ID feature extraction РАБОТАЕТ без license!\n");
    } else {
        printf("    ❌ FAILED — требует license\n");
    }

    /* FeatureCompare */
    if (r3 == ROCKIVA_RET_SUCCESS && feature.size > 0) {
        printf("\n[5] ROCKIVA_REID_FeatureCompare (self compare)...\n");
        float score = 0;
        RockIvaRetCode r4 = ROCKIVA_REID_FeatureCompare(feature.feature, feature.feature, &score);
        printf("    FeatureCompare = %d, score = %f\n", r4, score);
    }

    free(image.dataAddr);
    ROCKIVA_REID_Release(handle);
    ROCKIVA_Release(handle);
    return 0;
}
