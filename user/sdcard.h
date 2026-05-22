#ifndef __SDCARD_H
#define __SDCARD_H

#include <stdbool.h>
#include <stdint.h>

/* ================================================================
 * Micro-SD 卡驱动（SPI 模式，挂载 FATFS）
 *
 * 硬件接线：
 *   CS    -> GPIO 0   (板载 RGB LED 脚，CS 对时序不敏感，适合脏脚)
 *   MOSI  -> GPIO 8    (SD 模块 DI / CMD)
 *   SCK   -> GPIO 18   (SD 模块 CLK)
 *   MISO  -> GPIO 21   (SD 模块 DO / DAT0)
 *   VCC   -> 3.3V
 *   GND   -> GND
 *
 * 使用 SPI3_HOST（LCD 已占用 SPI2_HOST）
 *
 * 挂载点：/sdcard
 *   应用层用标准 POSIX API：fopen("/sdcard/music/song.mp3", "rb") 等
 *
 * 卡格式化要求：FAT32 或 exFAT（IDF 默认不开 exFAT，保险用 FAT32）
 *
 * 注意：插卡前不调用 sdcard_mount()，挂载失败返回 false，不会崩溃
 * ================================================================ */

/* 挂载点路径，其他模块可直接拼接 */
#define SDCARD_MOUNT_POINT   "/sdcard"

/**
 * @brief 初始化 SD SPI 总线并尝试挂载文件系统
 *
 * 可重入：重复调用 ok，内部有 mounted 标志；
 * 用 SPI3_HOST，跟 LCD（SPI2）不冲突。
 *
 * @return true 挂载成功；false 未插卡 / 卡损坏 / SPI 初始化失败
 */
bool sdcard_mount(void);

/**
 * @brief 卸载文件系统并释放 SPI 总线
 *
 * 场景：用户主动弹出 SD 卡前调用，避免写缓冲未 flush；
 * 或临时释放 SPI3 资源给别的外设用。
 */
void sdcard_unmount(void);

/**
 * @brief 查询当前是否已挂载
 */
bool sdcard_is_mounted(void);

/**
 * @brief 获取 SD 卡总容量（MB）。未挂载返回 0
 */
uint32_t sdcard_total_mb(void);

/**
 * @brief 获取 SD 卡可用空间（MB）。未挂载返回 0
 */
uint32_t sdcard_free_mb(void);

#endif /* __SDCARD_H */
