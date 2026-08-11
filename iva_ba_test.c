/* iva_ba_test.c — тест старой lib (666KB) с ROCKIVA_BA_Init
 *
 * Старая lib не имеет auth check. Используем BA (Behaviour Analysis)
 * для детекции person/face/pet — как rkipc.
 *
 * Кросс-компиляция:
 *   zig cc -target aarch64-linux-gnu -O2 -o iva_ba_test iva_ba_test.c \
 *       -I. -L. -lrockiva -Wl,-rpath,/usr/lib
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "old_inc/rockiva_common.h"
#include "old_inc/rockiva_ba_api.h"

#define MODEL_PATH "/usr/lib/"
#define WIDTH 1920
#define HEIGHT 1080

static int ba_callback_count = 0;

static void ba_callback(const RockIvaBaResult* result,
                         const RockIvaExecuteStatus status, void* userdata) {
    ba_callback_count++;
    printf("[ba_cb #%d] status=%d objNum=%d\n",
           ba_callback_count, status, result->objNum);
    for (uint32_t i = 0; i < result->objNum && i < 5; i++) {
        printf("  obj[%d] type=%d score=%d rect=[(%d,%d)-(%d,%d)]\n",
               i, result->triggerObjects[i].objInfo.type,
               result->triggerObjects[i].objInfo.score,
               result->triggerObjects[i].objInfo.rect.topLeft.x,
               result->triggerObjects[i].objInfo.rect.topLeft.y,
               result->triggerObjects[i].objInfo.rect.bottomRight.x,
               result->triggerObjects[i].objInfo.rect.bottomRight.y);
    }
}

int main(void) {
    printf("=== BA Detection Test (old lib 666KB) ===\n\n");

    /* Используем старую lib из /usr/lib */
    RockIvaHandle handle = NULL;
    RockIvaInitParam initParam;
    memset(&initParam, 0, sizeof(initParam));
    initParam.logLevel = ROCKIVA_LOG_INFO;
    snprintf(initParam.modelPath, 128, MODEL_PATH);
    initParam.coreMask = 0x04;
    initParam.detModel = ROCKIVA_DET_MODEL_PFP;
    initParam.imageInfo.width = WIDTH;
    initParam.imageInfo.height = HEIGHT;
    initParam.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;

    printf("[1] ROCKIVA_Init(VIDEO, PFP) — old lib, no auth...\n");
    RockIvaRetCode r1 = ROCKIVA_Init(&handle, ROCKIVA_MODE_VIDEO, &initParam, NULL);
    printf("    ROCKIVA_Init = %d\n", r1);
    if (r1 != ROCKIVA_RET_SUCCESS) {
        printf("    FAILED\n");
        return 1;
    }

    /* BA_Init — как rkipc */
    printf("\n[2] ROCKIVA_BA_Init...\n");
    RockIvaBaTaskParams baParams;
    memset(&baParams, 0, sizeof(baParams));
    /* Простое правило — детекция person/face/pet во всей области */
    baParams.baRules.areaInBreakRule[0].ruleEnable = 1;
    baParams.baRules.areaInBreakRule[0].sense = 50;
    baParams.baRules.areaInBreakRule[0].alertTime = 1000;
    baParams.baRules.areaInBreakRule[0].event = ROCKIVA_BA_TRIP_EVENT_STAY;
    baParams.baRules.areaInBreakRule[0].ruleID = 0;
    baParams.baRules.areaInBreakRule[0].objType =
        ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON) |
        ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_FACE) |
        ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PET);
    /* Вся область */
    baParams.baRules.areaInBreakRule[0].area.pointNum = 4;
    baParams.baRules.areaInBreakRule[0].area.points[0].x = 0;
    baParams.baRules.areaInBreakRule[0].area.points[0].y = 0;
    baParams.baRules.areaInBreakRule[0].area.points[1].x = 10000;
    baParams.baRules.areaInBreakRule[0].area.points[1].y = 0;
    baParams.baRules.areaInBreakRule[0].area.points[2].x = 10000;
    baParams.baRules.areaInBreakRule[0].area.points[2].y = 10000;
    baParams.baRules.areaInBreakRule[0].area.points[3].x = 0;
    baParams.baRules.areaInBreakRule[0].area.points[3].y = 10000;
    baParams.aiConfig.detectResultMode = 1;  /*上报所有检测目标 */

    RockIvaRetCode r2 = ROCKIVA_BA_Init(handle, &baParams, ba_callback);
    printf("    BA_Init = %d\n", r2);
    if (r2 != ROCKIVA_RET_SUCCESS) {
        printf("    FAILED\n");
        ROCKIVA_Release(handle);
        return 1;
    }

    /* PushFrame — серый NV12 кадр (CPU) */
    printf("\n[3] PushFrame (gray NV12, CPU)...\n");
    size_t frame_size = WIDTH * HEIGHT * 3 / 2;
    unsigned char* frame_data = (unsigned char*)malloc(frame_size);
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
        usleep(300000);
    }
    sleep(1);

    printf("\n=== ИТОГ ===\n");
    printf("ba_callback_count = %d\n", ba_callback_count);
    if (ba_callback_count > 0) {
        printf("✅ BA детекция РАБОТАЕТ (старая lib, без license)\n");
    } else {
        printf("⚠️ Callbacks не пришли\n");
    }

    ROCKIVA_BA_Release(handle);
    ROCKIVA_Release(handle);
    free(frame_data);
    return 0;
}
