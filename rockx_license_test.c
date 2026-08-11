/* rockx_license_test.c — проверка license для RockX Face API
 *
 * Старая плата RV1126/RV1109 (armhf) использует librockx.so + librockface.so
 * с empty key.lic (0 байт). Проверяем — работает ли face recognition без license.
 *
 * API: https://github.com/rockchip-linux/rockx-docs
 *
 * Кросс-компиляция (Windows, zig):
 *   zig cc -target arm-linux-musleabihf -o rockx_license_test rockx_license_test.c \
 *       -L. -lrockx -Wl,-rpath,/usr/lib
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

/* Минимальные определения RockX API (из публичной документации) */
typedef enum {
    ROCKX_SUCCESS = 0,
    ROCKX_FAILURE = -1,
    ROCKX_NOT_SUPPORT = -2,
    ROCKX_OUT_OF_MEMORY = -3,
    ROCKX_INVALID_PARAM = -4,
    ROCKX_INVALID_HANDLE = -5,
    ROCKX_INVALID_IMAGE = -6,
    ROCKX_NOT_ACTIVATED = -7,  /* license error */
} rockx_ret_t;

typedef enum {
    ROCKX_MODULE_FACE_DETECTION = 0,
    ROCKX_MODULE_FACE_LANDMARK = 1,
    ROCKX_MODULE_FACE_RECOGNIZE = 2,
    ROCKX_MODULE_FACE_LIVENESS = 3,
    ROCKX_MODULE_OBJECT_DETECTION = 4,
    ROCKX_MODULE_FACE_ATTRIBUTE = 5,
    ROCKX_MODULE_FACE_QUALITY = 6,
    /* ... */
} rockx_module_t;

typedef struct {
    int width;
    int height;
    int format;  /* 0: RGB888, 1: BGR888, 2: GRAY8, 3: YUV420SP_NV12 ... */
    unsigned char* data;
} rockx_image_t;

typedef struct rockx_handle_t {
    void* impl;
} rockx_handle_t;

typedef struct rockx_config_t {
    struct rockx_config_t* next;
    char* key;
    void* value;
    int value_size;
} rockx_config_t;

/* Правильная сигнатура rockx_create (из rockx.h):
 * rockx_ret_t rockx_create(rockx_module_t module,
 *                          rockx_config_t **config,
 *                          rockx_handle_t **handle) */
typedef rockx_ret_t (*rockx_create_t)(rockx_module_t module,
                                       rockx_config_t** config,
                                       rockx_handle_t** handle);
typedef rockx_config_t* (*rockx_create_config_t)(void);
typedef rockx_ret_t (*rockx_add_config_t)(rockx_config_t** config, const char* key, void* value, int value_size);
typedef rockx_ret_t (*rockx_release_config_t)(rockx_config_t* config);

/* Функции librockx.so */
typedef rockx_ret_t (*rockx_release_t)(rockx_handle_t* handle);
typedef rockx_ret_t (*rockx_set_licence_t)(const char* licence);
typedef rockx_ret_t (*rockx_face_detect_t)(rockx_handle_t* handle, rockx_image_t* in_img,
                                            void* out_result);
typedef rockx_ret_t (*rockx_face_recognize_t)(rockx_handle_t* handle, rockx_image_t* in_img,
                                               void* face, void* out_feature);
typedef rockx_ret_t (*rockx_face_feature_similarity_t)(void* feature1, void* feature2,
                                                        float* similarity);

int main(int argc, char** argv) {
    printf("=== RockX License Test (RV1126/RV1109 armhf) ===\n");

    void* lib = dlopen("/usr/lib/librockx.so", RTLD_NOW);
    if (!lib) {
        printf("dlopen librockx.so failed: %s\n", dlerror());
        return 1;
    }
    printf("librockx.so loaded\n");

    rockx_create_t rockx_create = (rockx_create_t)dlsym(lib, "rockx_create");
    rockx_create_config_t rockx_create_config = (rockx_create_config_t)dlsym(lib, "rockx_create_config");
    rockx_release_t rockx_release = (rockx_release_t)dlsym(lib, "rockx_release");
    rockx_set_licence_t rockx_set_licence = (rockx_set_licence_t)dlsym(lib, "rockx_set_licence");
    rockx_face_detect_t rockx_face_detect = (rockx_face_detect_t)dlsym(lib, "rockx_face_detect");
    rockx_face_recognize_t rockx_face_recognize = (rockx_face_recognize_t)dlsym(lib, "rockx_face_recognize");
    rockx_face_feature_similarity_t rockx_face_feature_similarity =
        (rockx_face_feature_similarity_t)dlsym(lib, "rockx_face_feature_similarity");

    printf("rockx_create: %p\n", rockx_create);
    printf("rockx_create_config: %p\n", rockx_create_config);
    printf("rockx_set_licence: %p\n", rockx_set_licence);
    printf("rockx_face_detect: %p\n", rockx_face_detect);
    printf("rockx_face_recognize: %p\n", rockx_face_recognize);
    printf("rockx_face_feature_similarity: %p\n", rockx_face_feature_similarity);

    if (!rockx_create) {
        printf("rockx_create not found!\n");
        dlclose(lib);
        return 1;
    }

    /* === Шаг 1: Проверка без license === */
    printf("\n[step1] rockx_create(FACE_DETECTION) без license...\n");
    rockx_handle_t* handle_det = NULL;
    rockx_config_t* cfg = rockx_create_config ? rockx_create_config() : NULL;
    rockx_ret_t ret = rockx_create(ROCKX_MODULE_FACE_DETECTION, &cfg, &handle_det);
    printf("[step1] rockx_create(FACE_DETECTION) = %d, handle=%p\n", ret, handle_det);

    if (ret == ROCKX_NOT_ACTIVATED) {
        printf(">>> NOT_ACTIVATED — face detection требует license <<<\n");
    } else if (ret == ROCKX_SUCCESS) {
        printf(">>> SUCCESS — face detection работает БЕЗ license! <<<\n");
        rockx_release(handle_det);
    } else {
        printf(">>> ret=%d (не license error) <<<\n", ret);
    }

    /* === Шаг 2: face_recognize === */
    printf("\n[step2] rockx_create(FACE_RECOGNIZE) без license...\n");
    rockx_handle_t* handle_rec = NULL;
    cfg = rockx_create_config ? rockx_create_config() : NULL;
    ret = rockx_create(ROCKX_MODULE_FACE_RECOGNIZE, &cfg, &handle_rec);
    printf("[step2] rockx_create(FACE_RECOGNIZE) = %d, handle=%p\n", ret, handle_rec);

    if (ret == ROCKX_NOT_ACTIVATED) {
        printf(">>> NOT_ACTIVATED — face recognition требует license <<<\n");
    } else if (ret == ROCKX_SUCCESS) {
        printf(">>> SUCCESS — face recognition работает БЕЗ license! <<<\n");
        rockx_release(handle_rec);
    } else {
        printf(">>> ret=%d (не license error) <<<\n", ret);
    }

    /* === Шаг 3: face_liveness === */
    printf("\n[step3] rockx_create(FACE_LIVENESS) без license...\n");
    rockx_handle_t* handle_live = NULL;
    cfg = rockx_create_config ? rockx_create_config() : NULL;
    ret = rockx_create(ROCKX_MODULE_FACE_LIVENESS, &cfg, &handle_live);
    printf("[step3] rockx_create(FACE_LIVENESS) = %d, handle=%p\n", ret, handle_live);

    if (ret == ROCKX_NOT_ACTIVATED) {
        printf(">>> NOT_ACTIVATED — face liveness требует license <<<\n");
    } else if (ret == ROCKX_SUCCESS) {
        printf(">>> SUCCESS — face liveness работает БЕЗ license! <<<\n");
        rockx_release(handle_live);
    } else {
        printf(">>> ret=%d (не license error) <<<\n", ret);
    }

    /* === Шаг 4: object_detection === */
    printf("\n[step4] rockx_create(OBJECT_DETECTION) без license...\n");
    rockx_handle_t* handle_obj = NULL;
    cfg = rockx_create_config ? rockx_create_config() : NULL;
    ret = rockx_create(ROCKX_MODULE_OBJECT_DETECTION, &cfg, &handle_obj);
    printf("[step4] rockx_create(OBJECT_DETECTION) = %d, handle=%p\n", ret, handle_obj);

    if (ret == ROCKX_NOT_ACTIVATED) {
        printf(">>> NOT_ACTIVATED — object detection требует license <<<\n");
    } else if (ret == ROCKX_SUCCESS) {
        printf(">>> SUCCESS — object detection работает БЕЗ license! <<<\n");
        rockx_release(handle_obj);
    } else {
        printf(">>> ret=%d (не license error) <<<\n", ret);
    }

    /* === Итог === */
    printf("\n=== ИТОГ ===\n");
    printf("key.lic на плате: /oem/key.lic (0 байт — пустой)\n");
    printf("ROCKX_LICENCE_KEY env: %s\n", getenv("ROCKX_LICENCE_KEY") ? getenv("ROCKX_LICENCE_KEY") : "NOT SET");
    printf("\n");
    printf("Если все модули вернули SUCCESS — RockX работает без license на этой плате.\n");
    printf("Если NOT_ACTIVATED — нужна license.\n");

    dlclose(lib);
    return 0;
}
