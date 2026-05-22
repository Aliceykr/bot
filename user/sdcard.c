#include "sdcard.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"

#define TAG             "SDCARD"

/* ============ 硬件引脚 ============ */
/* GPIO 0 是 Boot 按键脚，上电时别按住即可（空闲态高电平正好符合 CS）*/
#define PIN_SD_CS       0
#define PIN_SD_MOSI     8
#define PIN_SD_SCK      18
#define PIN_SD_MISO     21

/* LCD 用 SPI2_HOST（由 lcd.c 初始化），SD 卡走 SPI3_HOST 避免冲突 */
#define SD_HOST_ID      SPI3_HOST

/* FATFS 挂载配置 */
#define SD_MAX_FILES    5          /* 同时打开的文件句柄上限 */
#define SD_ALLOC_UNIT   16 * 1024  /* 新卡格式化时的 cluster size（已格式化卡不用）*/

/* ============ 模块内部状态 ============
 * 所有对 s_card / s_mounted / s_bus_inited / s_drive 的读写都必须持 s_mtx，
 * 防止多任务（LVGL、后台下载、日志）并发调用 mount/unmount/is_mounted 导致
 * use-after-free 或状态撕裂。
 *
 * 约定：
 *   - s_mtx 创建于首次进入 sdcard_mount()；
 *   - 持锁期间只做挂载/卸载相关的 bus / VFS 调用，不持锁执行任何长时间 IO；
 *   - fopen/fread 在应用层不需要持 s_mtx（FATFS 内部有卷级锁）。 */
static sdmmc_card_t     *s_card       = NULL;
static bool              s_mounted    = false;
static bool              s_bus_inited = false;
static SemaphoreHandle_t s_mtx        = NULL;

/* 挂载后记录 esp_vfs_fat_sdspi_mount 实际分配的 FATFS 驱动号，
 * 用于 f_getfree() 查询剩余空间时正确索引。
 * -1 表示未挂载 / 未知 */
static int               s_drive      = -1;

/* ================================================================
 * 惰性创建 mutex：首次调用时创建，之后复用
 * 进入 app_main 的任务在没有并发前先 mount 一次，这个时机是安全的
 * ================================================================ */
static bool ensure_mutex(void)
{
    if (s_mtx) return true;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) {
        ESP_LOGE(TAG, "mutex 创建失败");
        return false;
    }
    return true;
}

/* ================================================================
 * 从 esp_vfs_fat_sdspi_mount 生成的挂载点反推 FATFS 驱动号
 * 挂载点形如 "/sdcard"，对应 FATFS 驱动号字符串形如 "0:"、"1:" 等。
 * 这里用 esp_vfs_fat 内部记账不方便，改为 f_getfree 遍历全部驱动号，
 * 找到第一个能成功返回的就是我们的卷。对 SD 卡通常是 0 号，但
 * 若用户先挂了 SPIFFS 的 FatFs 卷（极少见）就会是 1 号。 */
static int probe_fatfs_drive_number(void)
{
    for (int i = 0; i < FF_VOLUMES; ++i) {
        char drv[4] = { (char)('0' + i), ':', 0, 0 };
        FATFS *fs = NULL;
        DWORD  free_clusters = 0;
        FRESULT fr = f_getfree(drv, &free_clusters, &fs);
        if (fr == FR_OK && fs != NULL) {
            return i;
        }
    }
    return -1;
}

/* ================================================================
 * 公开接口
 * ================================================================ */

bool sdcard_mount(void)
{
    if (!ensure_mutex()) return false;

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    if (s_mounted) {
        xSemaphoreGive(s_mtx);
        return true;
    }

    esp_err_t ret;

    /* ---- 1. 初始化 SPI 总线（只做一次） ---- */
    if (!s_bus_inited) {
        /* SD 卡 SPI 模式要求 MISO / CS 线有上拉。
         * 很多裸 SD 卡座/模块没有贴片上拉电阻，靠 MCU 内部弱上拉兜底。
         * 注意：弱上拉 ~45kΩ，如果 SPI 跑 >10MHz 信号沿会圆，
         * 高频下建议板上加 10kΩ 上拉到 3.3V */
        gpio_set_pull_mode(PIN_SD_MISO, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(PIN_SD_CS,   GPIO_PULLUP_ONLY);
        /* MOSI / SCK 也上拉，防止浮空时误采样 */
        gpio_set_pull_mode(PIN_SD_MOSI, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(PIN_SD_SCK,  GPIO_PULLUP_ONLY);

        spi_bus_config_t bus_cfg = {
            .mosi_io_num     = PIN_SD_MOSI,
            .miso_io_num     = PIN_SD_MISO,
            .sclk_io_num     = PIN_SD_SCK,
            .quadwp_io_num   = -1,
            .quadhd_io_num   = -1,
            /* max_transfer_sz：FATFS 单次读写最多一个 cluster（16KB），
             * 留点余量给底层协议开销 */
            .max_transfer_sz = 32 * 1024,
        };
        ret = spi_bus_initialize(SD_HOST_ID, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize 失败: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_mtx);
            return false;
        }
        s_bus_inited = true;
    }

    /* ---- 2. 配置 SD SPI host（SPI3 + CS） ---- */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SD_HOST_ID;
    /* 初始低频握手后协商到这个频率。
     * 硬件验证过 10MHz 稳定工作；提到 20MHz 读速度翻倍（约 2MB/s），
     * 如果出现偶发读错误可降回 10MHz */
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.host_id = SD_HOST_ID;

    /* ---- 3. 挂载文件系统（已格式化卡：失败不要自动格式化，避免误删用户数据） ---- */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = SD_MAX_FILES,
        .allocation_unit_size   = SD_ALLOC_UNIT,
    };

    ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot_cfg,
                                  &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "挂载失败：卡存在但文件系统无法识别（请格式化为 FAT32）");
        } else {
            ESP_LOGE(TAG, "挂载失败: %s（检查接线 / 是否插卡）", esp_err_to_name(ret));
        }
        s_card = NULL;
        /* 挂载失败：回收 SPI 总线，避免 DMA 通道 + ~4KB DRAM 被挂载失败的
         * 状态永久占用。下次 mount 会重新初始化总线。 */
        if (s_bus_inited) {
            spi_bus_free(SD_HOST_ID);
            s_bus_inited = false;
        }
        xSemaphoreGive(s_mtx);
        return false;
    }

    /* 记录 FATFS 实际分配到的驱动号，后续 sdcard_free_mb 用 */
    s_drive   = probe_fatfs_drive_number();
    s_mounted = true;

    ESP_LOGI(TAG, "SD 卡挂载成功: %s (FATFS drv=%d)", SDCARD_MOUNT_POINT, s_drive);
    sdmmc_card_print_info(stdout, s_card);

    xSemaphoreGive(s_mtx);
    return true;
}

void sdcard_unmount(void)
{
    if (!ensure_mutex()) return;

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    if (!s_mounted) {
        xSemaphoreGive(s_mtx);
        return;
    }

    /* 先清 mounted 标志再卸载，阻止并发 is_mounted 返回 true 后访问 s_card */
    s_mounted = false;

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "卸载失败: %s", esp_err_to_name(ret));
    }
    s_card  = NULL;
    s_drive = -1;

    /* 彻底释放 SPI 总线，归还 DMA 通道和 ~4KB DRAM。
     * 用户再调 sdcard_mount() 会重新初始化。 */
    if (s_bus_inited) {
        spi_bus_free(SD_HOST_ID);
        s_bus_inited = false;
    }

    ESP_LOGI(TAG, "SD 卡已卸载");
    xSemaphoreGive(s_mtx);
}

bool sdcard_is_mounted(void)
{
    /* 读取单个 bool，跨任务读写有撕裂风险但最多读到旧值，
     * 调用方依赖这个返回值做 fopen 时，FATFS 内部会再校验，
     * 所以这里不强制加锁（加锁会拖慢高频调用）。 */
    return s_mounted;
}

uint32_t sdcard_total_mb(void)
{
    if (!ensure_mutex()) return 0;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t result = 0;
    if (s_mounted && s_card) {
        /* capacity 单位是 sector（512 字节），用 64 位运算后转 MB。
         * MB 值超过 uint32_t 上限（≈ 4 PB）才会溢出，实际 SD 卡达不到 */
        uint64_t bytes = (uint64_t)s_card->csd.capacity *
                         (uint64_t)s_card->csd.sector_size;
        uint64_t mb    = bytes / (1024ULL * 1024ULL);
        /* 64 位 MB 值截成 32 位前做饱和，保护 >4TB 卡（未来可能存在） */
        if (mb > 0xFFFFFFFFULL) mb = 0xFFFFFFFFULL;
        result = (uint32_t)mb;
    }
    xSemaphoreGive(s_mtx);
    return result;
}

uint32_t sdcard_free_mb(void)
{
    if (!ensure_mutex()) return 0;

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    if (!s_mounted || s_drive < 0) {
        xSemaphoreGive(s_mtx);
        return 0;
    }

    /* 构造驱动号字符串，如 "0:" 或 "1:"。用挂载时探到的实际卷号，
     * 避免硬编码 "0:" 在多 FatFs 卷环境下返回错误的空间信息 */
    char drv[4] = { (char)('0' + s_drive), ':', 0, 0 };
    FATFS *fs;
    DWORD  free_clusters;
    FRESULT fr = f_getfree(drv, &free_clusters, &fs);
    if (fr != FR_OK || fs == NULL) {
        xSemaphoreGive(s_mtx);
        return 0;
    }
    uint64_t total_sectors = (uint64_t)free_clusters * (uint64_t)fs->csize;
    /* FATFS 扇区一律 512 字节 */
    uint64_t mb = (total_sectors * 512ULL) / (1024ULL * 1024ULL);
    if (mb > 0xFFFFFFFFULL) mb = 0xFFFFFFFFULL;

    xSemaphoreGive(s_mtx);
    return (uint32_t)mb;
}
