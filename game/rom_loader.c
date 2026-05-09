#include "rom_loader.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"

#define TAG "ROM_LOADER"

#define SPIFFS_BASE      "/spiffs"
#define ROM_DIR          "/spiffs/roms"
#define SPIFFS_LABEL     "storage"   /* 对应 partitions.csv 里 spiffs 的 Name */

static bool s_mounted = false;

bool rom_loader_init(void)
{
    if (s_mounted) return true;

    esp_vfs_spiffs_conf_t conf = {
        .base_path       = SPIFFS_BASE,
        .partition_label = NULL,   /* 留 NULL 按类型 spiffs 找第一个分区 */
        .max_files       = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 挂载失败: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: %u / %u bytes used", (unsigned)used, (unsigned)total);
    }

    /* 确保 roms 目录存在（SPIFFS 不支持真正的目录层级，但路径前缀过滤有效）*/
    s_mounted = true;
    return true;
}

static bool has_gb_ext(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 3;
    if (strcasecmp(ext, ".gb") == 0) return true;
    if (len >= 5 && strcasecmp(name + len - 4, ".gbc") == 0) return true;
    return false;
}

bool rom_loader_scan(rom_entry_t *list, int *count)
{
    if (!list || !count) return false;
    *count = 0;

    if (!rom_loader_init()) return false;

    /* SPIFFS 是扁平的，但支持路径前缀查找。opendir ROM_DIR 可能返回所有
     * 以 "/roms/" 开头的文件。为了兼容性，尝试 ROM_DIR 失败则扫 SPIFFS_BASE。*/
    DIR *d = opendir(ROM_DIR);
    const char *scan_path = ROM_DIR;
    if (!d) {
        d = opendir(SPIFFS_BASE);
        scan_path = SPIFFS_BASE;
    }
    if (!d) {
        ESP_LOGW(TAG, "无法打开 %s", scan_path);
        return true;  /* 不是错误，只是没有 ROM */
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && *count < ROM_MAX_COUNT) {
        if (!has_gb_ext(de->d_name)) continue;

        /* 构造完整路径以 stat 取文件大小
         * d_name 理论最大 NAME_MAX=255，scan_path 固定路径 < 32，
         * 故 320 字节缓冲足以容纳最坏情况 */
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", scan_path, de->d_name);
        struct stat st;
        size_t fsize = 0;
        if (stat(full, &st) == 0) fsize = (size_t)st.st_size;

        rom_entry_t *entry = &list[*count];
        strncpy(entry->name, de->d_name, ROM_MAX_NAME - 1);
        entry->name[ROM_MAX_NAME - 1] = '\0';
        entry->size = fsize;
        (*count)++;
    }
    closedir(d);

    ESP_LOGI(TAG, "发现 %d 个 ROM", *count);
    return true;
}

void *rom_loader_read(const char *name, size_t *size)
{
    if (!name || !size) return NULL;
    if (!rom_loader_init()) return NULL;

    /* 依次尝试 ROM_DIR/name 和 SPIFFS_BASE/name
     * 路径缓冲 320 字节足以容纳最长 d_name (255) + 前缀 */
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", ROM_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(path, sizeof(path), "%s/%s", SPIFFS_BASE, name);
        fp = fopen(path, "rb");
    }
    if (!fp) {
        ESP_LOGE(TAG, "打开 ROM 失败: %s", name);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 4 * 1024 * 1024) {
        ESP_LOGE(TAG, "ROM 大小异常: %ld", len);
        fclose(fp);
        return NULL;
    }

    /* 放 PSRAM：ROM 最大 2MB（GBC MBC5），内部 DRAM 不够 */
    void *buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM 分配失败 %ld 字节", len);
        fclose(fp);
        return NULL;
    }

    /* 分块读取（fread 大块在 SPIFFS 上稳定性好过一次性全读）*/
    size_t total = 0;
    const size_t chunk = 4096;
    uint8_t *p = (uint8_t *)buf;
    while (total < (size_t)len) {
        size_t want = ((size_t)len - total) > chunk ? chunk : ((size_t)len - total);
        size_t got = fread(p + total, 1, want, fp);
        if (got == 0) break;
        total += got;
    }
    fclose(fp);

    if (total != (size_t)len) {
        ESP_LOGE(TAG, "读 ROM 不完整: %u / %ld", (unsigned)total, len);
        heap_caps_free(buf);
        return NULL;
    }

    *size = total;
    ESP_LOGI(TAG, "ROM 加载完成 %s (%u bytes)", name, (unsigned)total);
    return buf;
}

void rom_loader_free(void *buf)
{
    if (buf) heap_caps_free(buf);
}
