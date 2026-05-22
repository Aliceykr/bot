#include "rom_loader.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdcard.h"

#define TAG "ROM_LOADER"

/* ROM 现在全部从 SD 卡读取：/sdcard/rom/ 下的 .gb / .gbc 文件
 *
 * 以前的 SPIFFS 方案已废弃，storage 分区从 partitions.csv 删除，app0/app1
 * 各扩到 6.5MB。
 *
 * rom_loader_init 名字保留以兼容 main.c 调用处，但现在仅做一次日志；
 * SD 卡由 main.c 的 sdcard_mount() 统一挂载。*/
#define ROM_DIR          SDCARD_MOUNT_POINT "/rom"

static bool s_logged_once = false;

bool rom_loader_init(void)
{
    if (!s_logged_once) {
        s_logged_once = true;
        if (sdcard_is_mounted()) {
            ESP_LOGI(TAG, "ROM 源: %s", ROM_DIR);
        } else {
            ESP_LOGW(TAG, "SD 卡未挂载，ROM 功能不可用");
        }
    }
    return sdcard_is_mounted();
}

static bool has_gb_ext(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) return false;
    if (strcasecmp(name + len - 3, ".gb") == 0) return true;
    if (len >= 5 && strcasecmp(name + len - 4, ".gbc") == 0) return true;
    return false;
}

bool rom_loader_scan(rom_entry_t *list, int *count)
{
    if (!list || !count) return false;
    *count = 0;

    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "scan: SD 卡未挂载");
        return false;
    }

    DIR *d = opendir(ROM_DIR);
    if (!d) {
        /* 用户可能还没建 rom 目录，这不是错误，返回空列表 */
        ESP_LOGW(TAG, "%s 不存在，请在 SD 卡上创建 rom/ 目录", ROM_DIR);
        return true;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && *count < ROM_MAX_COUNT) {
        if (de->d_type == DT_DIR) continue;
        if (!has_gb_ext(de->d_name)) continue;

        /* 构造完整路径以 stat 取文件大小
         * d_name 最大 NAME_MAX=255，路径前缀 < 32 字节，320 足够 */
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", ROM_DIR, de->d_name);
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
    if (!sdcard_is_mounted()) {
        ESP_LOGE(TAG, "read: SD 卡未挂载");
        return NULL;
    }

    /* name 来自 rom_loader_scan 的目录项，只拼到固定 ROM_DIR 下；
     * 外部若传入带路径分隔符的名字也只会在 SD 卡 ROM 目录下解析。 */
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", ROM_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "打开 ROM 失败: %s", path);
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

    /* 放 PSRAM：ROM 最大可到数 MB，内部 DRAM 要留给 LVGL、WiFi、音频 ring。
     * 模拟器读取 ROM 是随机访问，Walnut-CGB 的 16/32 位回调能把 PSRAM
     * 访问成本压到可接受范围。 */
    void *buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM 分配失败 %ld 字节", len);
        fclose(fp);
        return NULL;
    }

    /* 分块读取：4KB 对 FATFS 是较好的平衡点 */
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
