/*
 * dma_alloc.h — выделение DMA буферов через /dev/dma_heap/ (zero-copy для RGA/VO/NPU)
 *
 * На RV1126B есть:
 *   /dev/dma_heap/system          — cached DMA (CPU-кэшируемый)
 *   /dev/dma_heap/system-uncached — uncached DMA (быстрее для RGA, не нужно sync)
 *
 * DMA буфер = dmabuf fd + mmap virtual addr.
 * RGA может читать/писать через importbuffer_fd / wrapbuffer_fd_t (zero-copy, IOMMU).
 * rockit VO может принять через RK_MPI_SYS_CreateMB + MB_EXT_CONFIG_S.s32Fd.
 * rknn может принять через rknn_set_io_mem с dmabuf fd.
 */
#ifndef DMA_ALLOC_H
#define DMA_ALLOC_H

#include <stddef.h>

#define DMA_HEAP_UNCACHE_PATH        "/dev/dma_heap/system-uncached"
#define DMA_HEAP_PATH                "/dev/dma_heap/system"

/* Выделить DMA буфер. Возвращает 0 при успехе.
 * path  — "/dev/dma_heap/system-uncached" или "/dev/dma_heap/system"
 * size  — размер буфера в байтах
 * fd    — выходной dmabuf fd (для RGA/VO/NPU)
 * va    — выходной mmap virtual address (для CPU доступа: fwrite и т.д.)
 */
int dma_buf_alloc(const char *path, size_t size, int *fd, void **va);

/* Освободить DMA буфер (munmap + close fd) */
void dma_buf_free(size_t size, int *fd, void *va);

/* Синхронизация CPU↔device (для cached DMA буферов, для uncached не нужно) */
int dma_sync_device_to_cpu(int fd);
int dma_sync_cpu_to_device(int fd);

#endif /* DMA_ALLOC_H */
