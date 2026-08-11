/* iva_pfp_test.c — изолированный тест PFP с пустым key.lic
 * Проверяем: memAddr=NULL vs empty_buf vs garbage, файл есть/нет
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

int main(void) {
    printf("=== PFP isolated test ===\n\n");

    /* 1. Файл есть, memAddr=NULL */
    FILE* f = fopen(LICENSE_PATH, "wb");
    if (f) fclose(f);

    for (int mode = 0; mode <= 1; mode++) {
        const char* mode_str = mode ? "PICTURE" : "VIDEO";
        RockIvaHandle handle = NULL;
        RockIvaInitParam p;
        memset(&p, 0, sizeof(p));
        p.logLevel = ROCKIVA_LOG_ERROR;
        snprintf(p.logPath, 128, LOG_PATH);
        snprintf(p.modelPath, 128, MODEL_PATH);
        p.license.memAddr = NULL;
        p.license.memSize = 0;
        p.imageInfo.width = 1920;
        p.imageInfo.height = 1080;
        p.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
        p.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
        p.detModel = ROCKIVA_DET_MODEL_PFP;

        RockIvaRetCode r = ROCKIVA_Init(&handle, mode, &p, NULL);
        printf("[PFP %s memAddr=NULL file=empty] = %d\n", mode_str, r);
        if (r == 0 && handle) ROCKIVA_Release(handle);
    }

    /* 2. memAddr = empty buffer (не NULL) */
    printf("\n");
    for (int mode = 0; mode <= 1; mode++) {
        const char* mode_str = mode ? "PICTURE" : "VIDEO";
        RockIvaHandle handle = NULL;
        RockIvaInitParam p;
        memset(&p, 0, sizeof(p));
        p.logLevel = ROCKIVA_LOG_ERROR;
        snprintf(p.logPath, 128, LOG_PATH);
        snprintf(p.modelPath, 128, MODEL_PATH);
        char empty[1] = {0};
        p.license.memAddr = empty;
        p.license.memSize = 0;
        p.imageInfo.width = 1920;
        p.imageInfo.height = 1080;
        p.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
        p.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
        p.detModel = ROCKIVA_DET_MODEL_PFP;

        RockIvaRetCode r = ROCKIVA_Init(&handle, mode, &p, NULL);
        printf("[PFP %s memAddr=empty_buf size=0] = %d\n", mode_str, r);
        if (r == 0 && handle) ROCKIVA_Release(handle);
    }

    /* 3. memAddr = garbage 128 bytes */
    printf("\n");
    char garbage[128];
    memset(garbage, 'A', sizeof(garbage));
    for (int mode = 0; mode <= 1; mode++) {
        const char* mode_str = mode ? "PICTURE" : "VIDEO";
        RockIvaHandle handle = NULL;
        RockIvaInitParam p;
        memset(&p, 0, sizeof(p));
        p.logLevel = ROCKIVA_LOG_ERROR;
        snprintf(p.logPath, 128, LOG_PATH);
        snprintf(p.modelPath, 128, MODEL_PATH);
        p.license.memAddr = garbage;
        p.license.memSize = sizeof(garbage);
        p.imageInfo.width = 1920;
        p.imageInfo.height = 1080;
        p.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
        p.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
        p.detModel = ROCKIVA_DET_MODEL_PFP;

        RockIvaRetCode r = ROCKIVA_Init(&handle, mode, &p, NULL);
        printf("[PFP %s memAddr=garbage(128) size=128] = %d\n", mode_str, r);
        if (r == 0 && handle) ROCKIVA_Release(handle);
    }

    /* 4. Файла нет, memAddr=NULL */
    printf("\n");
    unlink(LICENSE_PATH);
    for (int mode = 0; mode <= 1; mode++) {
        const char* mode_str = mode ? "PICTURE" : "VIDEO";
        RockIvaHandle handle = NULL;
        RockIvaInitParam p;
        memset(&p, 0, sizeof(p));
        p.logLevel = ROCKIVA_LOG_ERROR;
        snprintf(p.logPath, 128, LOG_PATH);
        snprintf(p.modelPath, 128, MODEL_PATH);
        p.license.memAddr = NULL;
        p.license.memSize = 0;
        p.imageInfo.width = 1920;
        p.imageInfo.height = 1080;
        p.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
        p.cameraType = ROCKIVA_CAMERA_TYPE_ONE;
        p.detModel = ROCKIVA_DET_MODEL_PFP;

        RockIvaRetCode r = ROCKIVA_Init(&handle, mode, &p, NULL);
        printf("[PFP %s memAddr=NULL file=NO] = %d\n", mode_str, r);
        if (r == 0 && handle) ROCKIVA_Release(handle);
    }

    return 0;
}
