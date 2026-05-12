#ifndef __ROM_LOADER_H
#define __ROM_LOADER_H

#include <stddef.h>
#include <stdbool.h>

#define ROM_MAX_NAME   48
#define ROM_MAX_COUNT  16

typedef struct {
    char name[ROM_MAX_NAME];       /* 纯文件名，例如 "tobu.gb" */
    size_t size;                   /* 文件字节数 */
} rom_entry_t;

/* 兼容旧名字：早期 SPIFFS 版本在这里挂载；现在 ROM 从 SD 卡读取，
 * 实际挂载由 main.c 的 sdcard_mount() 完成。本函数仅做一次性日志。
 * 返回 SD 卡是否已挂载。*/
bool rom_loader_init(void);

/* 扫描 /sdcard/roms/ 下的 .gb / .gbc 文件。
 * 结果写入 list（调用方分配），实际数量写入 *count（<=ROM_MAX_COUNT）。
 * 返回 true 表示扫描成功（即使数量为 0 也返回 true）；SD 未挂载返回 false。 */
bool rom_loader_scan(rom_entry_t *list, int *count);

/* 将 ROM 加载到 PSRAM。调用者用完后必须调 rom_loader_free()。
 * 返回非 NULL 指向 ROM 数据，失败返回 NULL。*size 写入字节数。 */
void *rom_loader_read(const char *name, size_t *size);

/* 释放 rom_loader_read 返回的缓冲 */
void rom_loader_free(void *buf);

#endif
