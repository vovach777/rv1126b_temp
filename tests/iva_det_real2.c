/* iva_det_real2.c — пробуем PFP_V3 с правильным warmup и реальным изображением
 *
 * PFP_V3 использует object_detection_v3_pfp_640x384.data (3.5MB) — полноценная модель
 * Warmup: VIDEO PFP_V3 → FAIL, затем PICTURE PFP_V3 → SUCCESS
 * Также пробуем загрузить JPEG файл напрямую (format=JPEG)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rockiva_common.h"
#include "rockiva_det_api.h"

#define MODEL_PATH "/data/rockiva_data"
#define LOG_PATH   "/tmp/iva_test"
#define WIDTH 1920
#define HEIGHT 1080

static int det_callback_count = 0;
static int last_obj_num = -1;

static void det_callback(const RockIvaDetectResult* result,
                          const RockIvaExecuteStatus status, void* userdata) {
    det_callback_count++;
    last_obj_num = result->objNum;
    printf("[det_cb #%d] status=%d objNum=%d\n",
           det_callback_count, status, result->objNum);
    for (uint32_t i = 0; i < result->objNum && i < 5; i++) {
        printf("  obj[%d] type=%d score=%d rect=[(%d,%d)-(%d,%d)]\n",
               i, result->objInfo[i].type, result->objInfo[i].score,
               result->objInfo[i].rect.topLeft.x, result->objInfo[i].rect.topLeft.y,
               result->objInfo[i].rect.bottomRight.x, result->objInfo[i].rect.bottomRight.y);
    }
}

static RockIvaRetCode do_warmup(RockIvaDetModel model) {
    RockIvaHandle h = NULL;
    RockIvaInitParam p;
    memset(&p, 0, sizeof(p));
    p.logLevel = ROCKIVA_LOG_ERROR;
    snprintf(p.logPath, 128, LOG_PATH);
    snprintf(p.modelPath, 128, MODEL_PATH);
    p.imageInfo.width = WIDTH;
    p.imageInfo.height = HEIGHT;
    p.imageInfo.format = ROCKIVA_IMAGE_FORMAT_BGR888;
    p.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
    p.detModel = model;
    RockIvaRetCode r = ROCKIVA_Init(&h, ROCKIVA_MODE_VIDEO, &p, NULL);
    printf("  warmup(VIDEO, model=%d) = %d (ожидаем FAIL)\n", model, r);
    if (r == 0 && h) ROCKIVA_Release(h);
    return r;
}

static int test_model(RockIvaDetModel model, const char* name) {
    printf("\n=== Testing %s (model=%d) ===\n", name, model);

    /* Warmup */
    do_warmup(model);

    /* Real init */
    RockIvaHandle handle = NULL;
    RockIvaInitParam initParam;
    memset(&initParam, 0, sizeof(initParam));
    initParam.logLevel = ROCKIVA_LOG_ERROR;
    snprintf(initParam.logPath, 128, LOG_PATH);
    snprintf(initParam.modelPath, 128, MODEL_PATH);
    initParam.imageInfo.width = WIDTH;
    initParam.imageInfo.height = HEIGHT;
    initParam.imageInfo.format = ROCKIVA_IMAGE_FORMAT_BGR888;
    initParam.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
    initParam.detModel = model;

    RockIvaRetCode r1 = ROCKIVA_Init(&handle, ROCKIVA_MODE_PICTURE, &initParam, NULL);
    printf("  ROCKIVA_Init(PICTURE) = %d\n", r1);
    if (r1 != ROCKIVA_RET_SUCCESS) {
        printf("  FAILED\n");
        return 0;
    }

    /* DETECT_Init */
    RockIvaDetTaskParams detParams;
    memset(&detParams, 0, sizeof(detParams));
    RockIvaRetCode r2 = ROCKIVA_DETECT_Init(handle, &detParams, det_callback);
    printf("  DETECT_Init = %d\n", r2);
    if (r2 != ROCKIVA_RET_SUCCESS) {
        ROCKIVA_Release(handle);
        return 0;
    }

    /* PushFrame — пробуем BGR888 с CPU buffer (как rkipc) */
    size_t frame_size = WIDTH * HEIGHT * 3;  /* BGR888 = 3 bytes/pixel */
    unsigned char* frame_data = (unsigned char*)malloc(frame_size);
    memset(frame_data, 128, frame_size);  /* серый */

    RockIvaImage image;
    memset(&image, 0, sizeof(image));
    image.info.width = WIDTH;
    image.info.height = HEIGHT;
    image.info.format = ROCKIVA_IMAGE_FORMAT_BGR888;
    image.dataAddr = frame_data;
    image.size = frame_size;

    det_callback_count = 0;
    last_obj_num = -1;
    for (int i = 0; i < 3; i++) {
        RockIvaRetCode r3 = ROCKIVA_PushFrame(handle, &image, NULL);
        printf("  PushFrame[%d] = %d\n", i, r3);
        usleep(300000);
    }
    sleep(1);

    printf("  callbacks=%d last_obj_num=%d\n", det_callback_count, last_obj_num);

    ROCKIVA_DETECT_Release(handle);
    ROCKIVA_Release(handle);
    free(frame_data);
    return det_callback_count > 0 && last_obj_num >= 0;
}

int main(void) {
    printf("=== PFP / PFP_V3 Detection Test ===\n");

    /* Проверяем какие модели есть */
    printf("Models in /data/rockiva_data/:\n");
    system("ls /data/rockiva_data/ | grep -E 'object_detection|pfp'");

    /* Тест 1: PFP (uses object_detection_pfp_for_recog.data) */
    test_model(ROCKIVA_DET_MODEL_PFP, "PFP");

    /* Тест 2: PFP_V3 (uses object_detection_v3_pfp_*.data) */
    test_model(ROCKIVA_DET_MODEL_PFP_V3, "PFP_V3");

    printf("\n=== ИТОГ ===\n");
    return 0;
}
