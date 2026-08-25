/* rga_test.c — тест RGA rotation на RV1126B
 * Проверяем: работает ли rotation через librga im2d API
 * Используем правильный синтаксис improcess (как в vi_grab_dual.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>

#include "im2d.h"
#include "RgaApi.h"
#include "rga.h"

#define SRC_W 1920
#define SRC_H 1080
#define DST_W 1080  /* после rotation 90° */
#define DST_H 1920

static rga_buffer_t pat_dummy(void) {
    rga_buffer_t pat = {0};
    return pat;
}

int main(void) {
    printf("=== RGA Rotation Test (RV1126B) ===\n");
    printf("Test: %dx%d NV12 → rotate 90° → %dx%d NV12\n\n", SRC_W, SRC_H, DST_W, DST_H);

    /* Query RGA capabilities */
    printf("[1] Query RGA...\n");
    const char* ver = querystring(RGA_VERSION);
    printf("    version: %s\n", ver ? ver : "NULL");
    const char* max_in = querystring(RGA_MAX_INPUT);
    printf("    max input: %s\n", max_in ? max_in : "NULL");
    const char* max_out = querystring(RGA_MAX_OUTPUT);
    printf("    max output: %s\n", max_out ? max_out : "NULL");

    /* Allocate DMA buffers via dma-heap */
    printf("\n[2] Allocate DMA buffers...\n");
    int heap_fd = open("/dev/dma_heap/linux,cma", O_RDWR);
    if (heap_fd < 0) {
        heap_fd = open("/dev/dma_heap/system", O_RDWR);
    }
    if (heap_fd < 0) {
        perror("open dma_heap");
        return 1;
    }

    size_t src_size = SRC_W * SRC_H * 3 / 2; /* NV12 */
    size_t dst_size = DST_W * DST_H * 3 / 2;

    struct dma_heap_allocation_data alloc = {0};
    alloc.len = src_size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        perror("alloc src");
        return 1;
    }
    int src_fd = alloc.fd;
    printf("    src: fd=%d size=%zu\n", src_fd, src_size);

    alloc.len = dst_size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    memset(&alloc, 0, sizeof(alloc));
    alloc.len = dst_size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        perror("alloc dst");
        return 1;
    }
    int dst_fd = alloc.fd;
    printf("    dst: fd=%d size=%zu\n", dst_fd, dst_size);

    /* Fill source with pattern */
    uint8_t* src_ptr = mmap(NULL, src_size, PROT_READ|PROT_WRITE, MAP_SHARED, src_fd, 0);
    if (src_ptr == MAP_FAILED) { perror("mmap src"); return 1; }
    for (int y = 0; y < SRC_H; y++)
        for (int x = 0; x < SRC_W; x++)
            src_ptr[y * SRC_W + x] = (x * 255) / SRC_W;
    memset(src_ptr + SRC_W * SRC_H, 128, SRC_W * SRC_H / 2);

    /* Wrap for RGA */
    printf("\n[3] wrapbuffer_fd...\n");
    rga_buffer_t src = wrapbuffer_fd(src_fd, SRC_W, SRC_H, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_fd(dst_fd, DST_W, DST_H, RK_FORMAT_YCbCr_420_SP);
    printf("    src: %dx%d fmt=%d\n", src.width, src.height, src.format);
    printf("    dst: %dx%d fmt=%d\n", dst.width, dst.height, dst.format);

    im_rect srect = { .width = SRC_W, .height = SRC_H };
    im_rect drect = { .width = DST_W, .height = DST_H };
    im_rect prect = {0};

    /* Test 1: rotate 90° */
    printf("\n[4] improcess rotate 90°...\n");
    int usage = IM_HAL_TRANSFORM_ROT_90 | IM_SYNC;
    IM_STATUS st = improcess(src, dst, pat_dummy(), srect, drect, prect, usage);
    printf("    = %d (%s)\n", st, st == IM_STATUS_SUCCESS ? "SUCCESS" : "FAILED");

    /* Test 2: rotate 270° */
    printf("\n[5] improcess rotate 270°...\n");
    usage = IM_HAL_TRANSFORM_ROT_270 | IM_SYNC;
    st = improcess(src, dst, pat_dummy(), srect, drect, prect, usage);
    printf("    = %d (%s)\n", st, st == IM_STATUS_SUCCESS ? "SUCCESS" : "FAILED");

    /* Test 3: scale 1920x1080 → 800x1280 (no rotation) */
    printf("\n[6] scale 1920x1080 → 800x1280 (no rot)...\n");
    rga_buffer_t dst2 = wrapbuffer_fd(dst_fd, 800, 1280, RK_FORMAT_YCbCr_420_SP);
    im_rect drect2 = { .width = 800, .height = 1280 };
    usage = IM_SYNC;
    st = improcess(src, dst2, pat_dummy(), srect, drect2, prect, usage);
    printf("    = %d (%s)\n", st, st == IM_STATUS_SUCCESS ? "SUCCESS" : "FAILED");

    /* Test 4: scale + rotate 90° 1920x1080 → 720x1280 */
    printf("\n[7] scale+rotate 1920x1080 → 720x1280 (rot 90°)...\n");
    rga_buffer_t dst3 = wrapbuffer_fd(dst_fd, 720, 1280, RK_FORMAT_YCbCr_420_SP);
    im_rect drect3 = { .width = 720, .height = 1280 };
    usage = IM_HAL_TRANSFORM_ROT_90 | IM_SYNC;
    st = improcess(src, dst3, pat_dummy(), srect, drect3, prect, usage);
    printf("    = %d (%s)\n", st, st == IM_STATUS_SUCCESS ? "SUCCESS" : "FAILED");

    /* Test 5: crop + scale + rotate — как в оригинале percomedia */
    printf("\n[8] crop(800x1080) + scale + rotate 90° → 800x1280...\n");
    /* crop center: 1920x1080 → crop 800x1080 (portrait area) */
    int crop_x = (SRC_W - 800) / 2;  /* 560 */
    im_rect srect_crop = { .x = crop_x, .y = 0, .width = 800, .height = 1080 };
    im_rect drect_crop = { .width = 800, .height = 1280 };
    usage = IM_HAL_TRANSFORM_ROT_90 | IM_SYNC;
    st = improcess(src, dst2, pat_dummy(), srect_crop, drect_crop, prect, usage);
    printf("    = %d (%s)\n", st, st == IM_STATUS_SUCCESS ? "SUCCESS" : "FAILED");

    munmap(src_ptr, src_size);
    close(src_fd);
    close(dst_fd);
    close(heap_fd);

    printf("\n=== ИТОГ ===\n");
    return 0;
}
