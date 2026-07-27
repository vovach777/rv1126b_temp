/*
 * dma_alloc.c — выделение DMA буферов через /dev/dma_heap/ (zero-copy для RGA/VO/NPU)
 *
 * Перевод dma_alloc.cpp из linux-rga/samples/utils/allocator/ на чистый C.
 * Источник: K:\sdk\external\linux-rga\samples\utils\allocator\dma_alloc.cpp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdint.h>

#include "dma_alloc.h"

typedef unsigned long long __u64;
typedef unsigned int __u32;

struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC      'H'
#define DMA_HEAP_IOCTL_ALLOC    _IOWR(DMA_HEAP_IOC_MAGIC, 0, struct dma_heap_allocation_data)

#define DMA_BUF_SYNC_READ       (1 << 0)
#define DMA_BUF_SYNC_WRITE      (2 << 0)
#define DMA_BUF_SYNC_RW         (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START      (0 << 2)
#define DMA_BUF_SYNC_END        (1 << 2)

struct dma_buf_sync {
    __u64 flags;
};

#define DMA_BUF_BASE            'b'
#define DMA_BUF_IOCTL_SYNC      _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

int dma_sync_device_to_cpu(int fd) {
    struct dma_buf_sync sync = {0};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

int dma_sync_cpu_to_device(int fd) {
    struct dma_buf_sync sync = {0};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

int dma_buf_alloc(const char *path, size_t size, int *fd, void **va) {
    int ret;
    int prot;
    void *mmap_va;
    int dma_heap_fd = -1;
    struct dma_heap_allocation_data buf_data;

    dma_heap_fd = open(path, O_RDWR);
    if (dma_heap_fd < 0) {
        fprintf(stderr, "dma_buf_alloc: open %s failed: %s\n", path, strerror(errno));
        return dma_heap_fd;
    }

    memset(&buf_data, 0, sizeof(buf_data));
    buf_data.len = size;
    buf_data.fd_flags = O_CLOEXEC | O_RDWR;
    ret = ioctl(dma_heap_fd, DMA_HEAP_IOCTL_ALLOC, &buf_data);
    if (ret < 0) {
        fprintf(stderr, "dma_buf_alloc: ioctl ALLOC failed: %s\n", strerror(errno));
        close(dma_heap_fd);
        return ret;
    }

    if (fcntl(buf_data.fd, F_GETFL) & O_RDWR)
        prot = PROT_READ | PROT_WRITE;
    else
        prot = PROT_READ;

    mmap_va = mmap(NULL, buf_data.len, prot, MAP_SHARED, buf_data.fd, 0);
    if (mmap_va == MAP_FAILED) {
        fprintf(stderr, "dma_buf_alloc: mmap failed: %s\n", strerror(errno));
        close(buf_data.fd);
        close(dma_heap_fd);
        return -errno;
    }

    *va = mmap_va;
    *fd = buf_data.fd;

    close(dma_heap_fd);
    return 0;
}

void dma_buf_free(size_t size, int *fd, void *va) {
    if (va && va != MAP_FAILED)
        munmap(va, size);
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
