/* iva_det_real.c — проверка PFP детекции с реальным кадром
 *
 * Warmup трюк + ROCKIVA_DETECT_Init + PushFrame с тестовым NV12 кадром
 * Проверяем — приходит ли callback с результатами детекции
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "rockiva_common.h"
#include "rockiva_det_api.h"

#define MODEL_PATH "/data/rockiva_data"
#define LOG_PATH   "/tmp/iva_test"
#define WIDTH 1920
#define HEIGHT 1080

static int det_callback_count = 0;

static void det_callback(const RockIvaDetectResult* result,
                          const RockIvaExecuteStatus status, void* userdata) {
    det_callback_count++;
    printf("[det_cb #%d] status=%d objNum=%d\n",
           det_callback_count, status, result->objNum);
    for (uint32_t i = 0; i < result->objNum && i < 5; i++) {
        printf("  obj[%d] type=%d score=%d rect=[(%d,%d)-(%d,%d)]\n",
               i, result->objInfo[i].type, result->objInfo[i].score,
               result->objInfo[i].rect.topLeft.x, result->objInfo[i].rect.topLeft.y,
               result->objInfo[i].rect.bottomRight.x, result->objInfo[i].rect.bottomRight.y);
    }
}

int main(void) {
    printf("=== PFP Detection Real Test (warmup) ===\n\n");

    /* === Warmup: VIDEO PFP → FAIL === */
    printf("[1] Warmup: ROCKIVA_Init(VIDEO, PFP)...\n");
    {
        RockIvaHandle h = NULL;
        RockIvaInitParam p;
        memset(&p, 0, sizeof(p));
        p.logLevel = ROCKIVA_LOG_ERROR;
        snprintf(p.logPath, 128, LOG_PATH);
        snprintf(p.modelPath, 128, MODEL_PATH);
        p.imageInfo.width = WIDTH;
        p.imageInfo.height = HEIGHT;
        p.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
        p.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
        p.detModel = ROCKIVA_DET_MODEL_PFP;
        RockIvaRetCode r = ROCKIVA_Init(&h, ROCKIVA_MODE_VIDEO, &p, NULL);
        printf("    warmup = %d (ожидаем FAIL)\n", r);
        if (r == 0 && h) ROCKIVA_Release(h);
    }

    /* === Рабочий вызов: PICTURE PFP === */
    printf("\n[2] ROCKIVA_Init(PICTURE, PFP)...\n");
    RockIvaHandle handle = NULL;
    RockIvaInitParam initParam;
    memset(&initParam, 0, sizeof(initParam));
    initParam.logLevel = ROCKIVA_LOG_INFO;
    snprintf(initParam.logPath, 128, LOG_PATH);
    snprintf(initParam.modelPath, 128, MODEL_PATH);
    initParam.imageInfo.width = WIDTH;
    initParam.imageInfo.height = HEIGHT;
    initParam.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    initParam.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
    initParam.detModel = ROCKIVA_DET_MODEL_PFP;
    initParam.trackerVersion = 0;

    RockIvaRetCode r1 = ROCKIVA_Init(&handle, ROCKIVA_MODE_PICTURE, &initParam, NULL);
    printf("    ROCKIVA_Init = %d\n", r1);
    if (r1 != ROCKIVA_RET_SUCCESS) {
        printf("FAILED — warmup не сработал\n");
        return 1;
    }

    /* === DETECT_Init === */
    printf("\n[3] ROCKIVA_DETECT_Init...\n");
    RockIvaDetTaskParams detParams;
    memset(&detParams, 0, sizeof(detParams));
    RockIvaRetCode r2 = ROCKIVA_DETECT_Init(handle, &detParams, det_callback);
    printf("    DETECT_Init = %d\n", r2);
    if (r2 != ROCKIVA_RET_SUCCESS) {
        printf("FAILED\n");
        ROCKIVA_Release(handle);
        return 1;
    }

    /* === PushFrame — тестовый NV12 кадр (серый) === */
    printf("\n[4] PushFrame (test NV12 gray frame)...\n");
    size_t frame_size = WIDTH * HEIGHT * 3 / 2;  /* NV12 */
    unsigned char* frame_data = (unsigned char*)malloc(frame_size);
    /* Y = 128 (серый), UV = 128 (нейтральный) */
    memset(frame_data, 128, frame_size);

    RockIvaImage image;
    memset(&image, 0, sizeof(image));
    image.info.width = WIDTH;
    image.info.height = HEIGHT;
    image.info.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    image.dataAddr = frame_data;
    image.size = frame_size;

    for (int i = 0; i < 5; i++) {
        RockIvaRetCode r3 = ROCKIVA_PushFrame(handle, &image, NULL);
        printf("    PushFrame[%d] = %d\n", i, r3);
        usleep(200000);  /* 200ms — ждём callback */
    }

    /* Ждём callbacks */
    printf("\n[5] Waiting for callbacks (2 sec)...\n");
    sleep(2);

    printf("\n=== ИТОГ ===\n");
    printf("det_callback_count = %d\n", det_callback_count);
    if (det_callback_count > 0) {
        printf("✅ Детекция РАБОТАЕТ — получили %d callbacks\n", det_callback_count);
    } else {
        printf("⚠️ Callbacks не пришли — возможно серый кадр без объектов\n");
        printf("   (нужен реальный кадр с камеры для проверки)\n");
    }

    ROCKIVA_DETECT_Release(handle);
    ROCKIVA_Release(handle);
    free(frame_data);
    return 0;
}
