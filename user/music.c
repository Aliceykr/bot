#include "music.h"
#include "speaker.h"
#include "sdcard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define TAG "MUSIC"

/* ================================================================
 * MP3 支持开关
 *
 * 用 chmorgan/esp-libhelix-mp3（Espressif Component Registry）。
 * 选它的原因：解码器工作 RAM 最小（~5KB，可放 PSRAM），避免挤压 DRAM。
 * minimp3 要 ~16KB 栈，esp_audio_codec 要 ~20KB，都太大。
 *
 * 依赖 main/idf_component.yml 里的 chmorgan/esp-libhelix-mp3 ^1.0.1。
 * 默认启用；要禁用可在 CMakeLists 加 -DMUSIC_ENABLE_MP3=0 并从
 * idf_component.yml 删除依赖。
 * ================================================================ */
#ifndef MUSIC_ENABLE_MP3
#define MUSIC_ENABLE_MP3 1
#endif

#if MUSIC_ENABLE_MP3
#include "mp3dec.h"
#endif

#define MUSIC_DIR   SDCARD_MOUNT_POINT "/music"

/* 每次 fread 的块大小（必须偶数，WAV 样本对齐要用）
 * 必须放内部 DMA-capable DRAM（见下方 file_buf 分配），
 * 因此保守用 4KB 减少内部 DRAM 压力 */
#define FREAD_CHUNK   4096

/* ================================================================
 * 扫描 / 路径工具
 * ================================================================ */
static music_fmt_t fmt_from_name(const char *name)
{
    size_t len = strlen(name);
    if (len >= 4 && strcasecmp(name + len - 4, ".wav") == 0) return MUSIC_FMT_WAV;
    if (len >= 4 && strcasecmp(name + len - 4, ".mp3") == 0) return MUSIC_FMT_MP3;
    return MUSIC_FMT_UNKNOWN;
}

bool music_scan(music_entry_t *list, int *count)
{
    if (!list || !count) return false;
    *count = 0;

    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "SD 卡未挂载");
        return false;
    }

    DIR *d = opendir(MUSIC_DIR);
    if (!d) {
        ESP_LOGW(TAG, "%s 不存在，请在 SD 卡上创建 music/ 目录", MUSIC_DIR);
        return true;   /* 空列表，不算错 */
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && *count < MUSIC_MAX_COUNT) {
        if (de->d_type == DT_DIR) continue;
        music_fmt_t fmt = fmt_from_name(de->d_name);
        if (fmt == MUSIC_FMT_UNKNOWN) continue;

#if !MUSIC_ENABLE_MP3
        /* 不支持 MP3 时跳过，避免用户选中后解码失败 */
        if (fmt == MUSIC_FMT_MP3) continue;
#endif

        char full[320];
        snprintf(full, sizeof(full), "%s/%s", MUSIC_DIR, de->d_name);
        struct stat st;
        size_t fsize = 0;
        if (stat(full, &st) == 0) fsize = (size_t)st.st_size;

        music_entry_t *entry = &list[*count];
        strncpy(entry->name, de->d_name, MUSIC_MAX_NAME - 1);
        entry->name[MUSIC_MAX_NAME - 1] = '\0';
        entry->size = fsize;
        entry->fmt  = fmt;
        (*count)++;
    }
    closedir(d);

    ESP_LOGI(TAG, "发现 %d 首音乐", *count);
    return true;
}

void music_full_path(const char *name, char *out, size_t cap)
{
    if (!name || !out || cap == 0) return;
    snprintf(out, cap, "%s/%s", MUSIC_DIR, name);
}

/* ================================================================
 * 播放器状态
 * ================================================================ */
static volatile music_state_t s_state           = MUSIC_STATE_IDLE;
static volatile bool          s_stop_request    = false;
static volatile bool          s_pause_request   = false;
static SemaphoreHandle_t      s_mtx             = NULL;
static SemaphoreHandle_t      s_done_sem        = NULL;
static TaskHandle_t           s_task            = NULL;
static char                   s_current_name[96] = "";

static inline void mp_lock(void)   { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void mp_unlock(void) { if (s_mtx) xSemaphoreGive(s_mtx); }

/* 解码任务公用的 PCM 推送 + 暂停/停止控制。
 * 返回 false 表示被 stop 请求打断，调用方应立即返回。*/
static bool push_pcm_mono_blocking(const int16_t *pcm, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = bytes;
    while (left > 0) {
        if (s_stop_request) return false;
        while (s_pause_request && !s_stop_request) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_stop_request) return false;

        int written = speaker_play((const int16_t *)p, left);
        if (written > 0) {
            p    += written;
            left -= written;
        } else {
            /* ring 满：等 spk_tx_task 消费 */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return true;
}

/* ================================================================
 * WAV 解码器
 * ================================================================ */
static bool wav_parse_header(FILE *fp, uint32_t *sample_rate,
                             uint16_t *channels, uint16_t *bits,
                             uint32_t *data_bytes_out)
{
    uint8_t riff[12];
    if (fread(riff, 1, sizeof(riff), fp) != sizeof(riff)) return false;
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "不是 RIFF/WAVE");
        return false;
    }

    bool got_fmt = false;
    while (1) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, fp) != 8) return false;
        uint32_t size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                        ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint32_t to_read = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, to_read, fp) != to_read) return false;
            if (size > to_read) fseek(fp, (long)(size - to_read), SEEK_CUR);

            uint16_t fmt_code = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
            *channels    = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
            *sample_rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                           ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            *bits        = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
            if (fmt_code != 0x0001 && fmt_code != 0xFFFE) {
                ESP_LOGE(TAG, "不支持的编码 fmt=0x%04x", fmt_code);
                return false;
            }
            got_fmt = true;
        } else if (memcmp(hdr, "data", 4) == 0) {
            *data_bytes_out = size;
            break;
        } else {
            fseek(fp, (long)size, SEEK_CUR);
        }
    }

    if (!got_fmt)         { ESP_LOGE(TAG, "未找到 fmt chunk"); return false; }
    if (*bits != 16)      { ESP_LOGE(TAG, "只支持 16bit WAV"); return false; }
    if (*channels != 1 && *channels != 2) {
        ESP_LOGE(TAG, "不支持声道数 %u", *channels);
        return false;
    }
    return true;
}

static bool decode_wav_stream(FILE *fp)
{
    uint32_t sample_rate = 0, data_bytes = 0;
    uint16_t channels = 0, bits = 0;
    if (!wav_parse_header(fp, &sample_rate, &channels, &bits, &data_bytes)) {
        return false;
    }
    ESP_LOGI(TAG, "WAV: %u Hz, %u ch, %u bit, data=%u bytes",
             (unsigned)sample_rate, channels, bits, (unsigned)data_bytes);

    /* 钳制到 speaker 支持范围 */
    if (sample_rate < 8000)  sample_rate = 8000;
    if (sample_rate > 48000) sample_rate = 48000;
    if (!speaker_set_sample_rate(sample_rate)) {
        ESP_LOGE(TAG, "切换采样率失败");
        return false;
    }

    uint8_t *file_buf = heap_caps_malloc(FREAD_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    int16_t *mono_buf = heap_caps_malloc(FREAD_CHUNK / 2, MALLOC_CAP_SPIRAM);
    if (!file_buf || !mono_buf) {
        ESP_LOGE(TAG, "缓冲分配失败 (file_buf DMA-DRAM=%p, mono_buf PSRAM=%p)",
                 file_buf, mono_buf);
        if (file_buf) heap_caps_free(file_buf);
        if (mono_buf) heap_caps_free(mono_buf);
        return false;
    }

    uint32_t total_read = 0;
    bool ok = true;
    while (total_read < data_bytes) {
        if (s_stop_request) break;

        uint32_t remain = data_bytes - total_read;
        uint32_t want   = remain < FREAD_CHUNK ? remain : FREAD_CHUNK;
        uint32_t align  = (channels == 2) ? 4 : 2;
        want -= want % align;
        if (want == 0) break;

        size_t got = fread(file_buf, 1, want, fp);
        if (got == 0) break;
        total_read += got;

        int16_t *src = (int16_t *)file_buf;
        size_t samples_in = got / sizeof(int16_t);
        size_t mono_count;
        if (channels == 1) {
            memcpy(mono_buf, src, got);
            mono_count = samples_in;
        } else {
            mono_count = samples_in / 2;
            for (size_t i = 0; i < mono_count; ++i) {
                int32_t sum = (int32_t)src[i * 2] + (int32_t)src[i * 2 + 1];
                mono_buf[i] = (int16_t)(sum / 2);
            }
        }

        if (!push_pcm_mono_blocking(mono_buf, mono_count * sizeof(int16_t))) {
            ok = false;
            break;
        }
    }

    heap_caps_free(file_buf);
    heap_caps_free(mono_buf);
    return ok;
}

/* ================================================================
 * MP3 解码器（helix-mp3）
 *
 * 文件读-解-推 循环：
 *   1. 维护一个 INPUT_CHUNK=2KB 的环形输入缓冲 in_buf[]，有 in_left 字节
 *   2. 缺数据时 fread INPUT_CHUNK - in_left 字节填满
 *   3. MP3FindSyncWord 找 ID3/padding 后第一个同步字节
 *   4. MP3Decode 吃掉 1 帧字节数，产出 ≤2304 个 PCM sample / 声道
 *   5. stereo 降 mono 后 push_pcm_mono_blocking
 *   6. 第一帧解码成功后用 MP3GetLastFrameInfo 拿采样率切 speaker
 *
 * 关键细节：
 *   - 使用 chmorgan/esp-libhelix-mp3 的 C API，线程安全（纯函数 + handle）
 *   - PCM 输出缓冲每帧 2304 * 2 = 4608 个 int16，stereo 时是 9216 字节
 *     放 PSRAM 避免占内部 DRAM
 *   - Helix 内部 context 约 5KB，从 PSRAM 分配进一步减内部 DRAM 压力
 *     （MP3InitDecoder 内部会用 calloc，这个 calloc 直接走的是 esp malloc，
 *      esp-libhelix-mp3 的 CMakeLists 里如果定义 HELIX_FEATURE_USE_PSRAM
 *      会优先 PSRAM；若没定义就落在 DRAM，~5KB 可接受）
 * ================================================================ */
#if MUSIC_ENABLE_MP3

#define MP3_INPUT_CHUNK       (2 * 1024)    /* 输入环形缓冲：每次 fread 填满 */
#define MP3_MAX_PCM_SAMPLES   2304           /* helix 单帧最大 */

static bool decode_mp3_stream(FILE *fp)
{
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        ESP_LOGE(TAG, "MP3InitDecoder 失败");
        return false;
    }

    /* 跳过 ID3v2 tag（如果有）。
     * ID3v2 头：
     *   "ID3" (3B) + version(2B) + flags(1B) + size(4B, syncsafe)
     *   syncsafe：每字节最高位为 0，真实值是 7 位拼接
     * 不跳过的话 tag 内部可能有像 0xFFE 的字节，被 MP3FindSyncWord 当成
     * 同步字，helix 解码垃圾数据容易读到非法地址导致 crash。 */
    uint8_t id3_hdr[10];
    long skip_to = 0;
    if (fread(id3_hdr, 1, 10, fp) == 10 &&
        id3_hdr[0] == 'I' && id3_hdr[1] == 'D' && id3_hdr[2] == '3') {
        /* syncsafe size: 28 bits */
        uint32_t tag_size =
            ((uint32_t)(id3_hdr[6] & 0x7f) << 21) |
            ((uint32_t)(id3_hdr[7] & 0x7f) << 14) |
            ((uint32_t)(id3_hdr[8] & 0x7f) << 7)  |
            ((uint32_t)(id3_hdr[9] & 0x7f));
        /* 还有 footer flag (bit 4)，有的话多 10 字节 */
        bool has_footer = (id3_hdr[5] & 0x10) != 0;
        skip_to = 10 + (long)tag_size + (has_footer ? 10 : 0);
        ESP_LOGI(TAG, "ID3v2 tag 大小 %lu 字节（含头 %ld），跳过", (unsigned long)tag_size, skip_to);
    } else {
        /* 没 ID3 tag：回到文件头 */
        skip_to = 0;
    }
    fseek(fp, skip_to, SEEK_SET);

    /* 输入缓冲必须放内部 DMA-capable DRAM：SDSPI 驱动读扇区时要 DMA 直接
     * 访问 fread 目标地址，PSRAM 不是 DMA-capable，FATFS 会分配 bounce 缓冲
     * 拷贝——内存紧张时 bounce 失败报 0x101。直接把 in_buf 放内部 DRAM 避免绕。
     * 2KB 对内部 DRAM 压力很小。 */
    uint8_t *in_buf  = heap_caps_malloc(MP3_INPUT_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    /* 解码输出不走 IO，PSRAM 最省 DRAM */
    int16_t *pcm_buf = heap_caps_malloc(MP3_MAX_PCM_SAMPLES * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM);
    int16_t *mono_buf = heap_caps_malloc((MP3_MAX_PCM_SAMPLES / 2) * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
    if (!in_buf || !pcm_buf || !mono_buf) {
        ESP_LOGE(TAG, "MP3 缓冲分配失败 (in_buf DMA-DRAM=%p, pcm PSRAM=%p, mono PSRAM=%p)",
                 in_buf, pcm_buf, mono_buf);
        goto out_free;
    }

    int in_left = 0;
    bool sample_rate_set = false;
    bool ok = true;
    uint32_t eof_consume_count = 0;
    uint32_t bad_frame_streak = 0;   /* 连续坏帧计数，超上限放弃 */

    while (!s_stop_request) {
        /* 1. 补齐输入缓冲 */
        if (in_left < MP3_INPUT_CHUNK / 2) {
            /* 把剩下的 in_left 字节搬到 buf 头部 */
            if (in_left > 0) memmove(in_buf, in_buf + (MP3_INPUT_CHUNK - in_left), in_left);
            size_t want = MP3_INPUT_CHUNK - in_left;
            size_t got  = fread(in_buf + in_left, 1, want, fp);
            if (got == 0) {
                /* 文件读完：再给解码器一次机会消费残留字节。如果两次消费不
                 * 完就退出，防止残字节卡死循环 */
                if (++eof_consume_count > 2 || in_left == 0) break;
            } else {
                eof_consume_count = 0;
            }
            in_left += (int)got;
        }

        if (in_left <= 0) break;

        /* 2. 找 sync word */
        /* memmove 后有效数据布局是 [in_buf, in_buf + in_left)，解码起点是 in_buf */
        uint8_t *read_ptr = in_buf;
        int offset = MP3FindSyncWord(read_ptr, in_left);
        if (offset < 0) {
            /* 整块都没 sync：丢弃，重新 fread */
            in_left = 0;
            continue;
        }
        read_ptr += offset;
        in_left  -= offset;

        /* 3. 解码一帧。注意：helix 的 MP3Decode 第二个参数是指针的指针，
         * 会把 bytesLeft 自减；之后 read_ptr 不会自动前进，我们要自己算位移。
         * 等同形式：MP3Decode(dec, &read_ptr, &bytesLeft, pcm, useSize) 但
         * chmorgan 版 API 是 int bytesLeft 非指针，bytesLeft 通过参数回写。*/
        int bytes_left = in_left;
        int err = MP3Decode(dec, &read_ptr, &bytes_left, pcm_buf, 0);
        int consumed = in_left - bytes_left;
        in_left = bytes_left;

        if (err != 0) {
            /* helix 错误码：
             *   ERR_MP3_INDATA_UNDERFLOW (-1) 数据不够，继续填缓冲
             *   ERR_MP3_MAINDATA_UNDERFLOW (-2) 同上
             *   其他 -3..-12 帧 corruption / 非法 bitrate，跳 1 字节重新找同步 */
            if (err == -1 || err == -2) {
                /* 等下一轮补数据 */
                continue;
            }
            bad_frame_streak++;
            if (bad_frame_streak > 32) {
                /* 连续 32 个坏帧，文件格式有问题（或不是 MP3）。放弃，
                 * 防止无限在垃圾数据里试同步字，累积内部状态把 helix 推进
                 * 不合法状态，下次 decode 读到非法内存崩溃。*/
                ESP_LOGE(TAG, "连续坏帧过多，放弃播放 (last err=%d)", err);
                ok = false;
                break;
            }
            ESP_LOGW(TAG, "MP3Decode 错 %d，跳过该帧 (streak=%u)", err, (unsigned)bad_frame_streak);
            /* 丢一字节让下轮 FindSyncWord 重新同步 */
            if (in_left > 0) { read_ptr++; in_left--; }
            continue;
        }

        /* 成功解码一帧：清坏帧计数 */
        bad_frame_streak = 0;

        /* 4. 拿帧信息（首帧切采样率） */
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);

        if (!sample_rate_set) {
            ESP_LOGI(TAG, "MP3: %d Hz, %d ch, %d kbps, bitsPerSample=%d",
                     fi.samprate, fi.nChans, fi.bitrate / 1000, fi.bitsPerSample);
            int sr = fi.samprate;
            if (sr < 8000)  sr = 8000;
            if (sr > 48000) sr = 48000;
            if (!speaker_set_sample_rate((uint32_t)sr)) {
                ESP_LOGE(TAG, "切换采样率失败: %d Hz", sr);
                ok = false;
                break;
            }
            sample_rate_set = true;
        }

        if (fi.outputSamps <= 0) continue;   /* 没输出，继续 */

        /* 5. stereo 降 mono。fi.outputSamps 是"samples 数 × 声道数" 的总值 */
        int16_t *push;
        size_t push_samples;
        if (fi.nChans == 1) {
            push = pcm_buf;
            push_samples = fi.outputSamps;
        } else {
            push_samples = fi.outputSamps / 2;
            for (size_t i = 0; i < push_samples; ++i) {
                int32_t sum = (int32_t)pcm_buf[i * 2] + (int32_t)pcm_buf[i * 2 + 1];
                mono_buf[i] = (int16_t)(sum / 2);
            }
            push = mono_buf;
        }

        if (!push_pcm_mono_blocking(push, push_samples * sizeof(int16_t))) {
            ok = false;
            break;
        }

        (void)consumed;   /* bytes_left 已经被 helix 更新，in_left 同步过了 */
    }

out_free:
    if (dec)      MP3FreeDecoder(dec);
    if (in_buf)   heap_caps_free(in_buf);
    if (pcm_buf)  heap_caps_free(pcm_buf);
    if (mono_buf) heap_caps_free(mono_buf);
    return ok;
}

#else
static bool decode_mp3_stream(FILE *fp)
{
    (void)fp;
    ESP_LOGW(TAG, "MP3 支持未编译（MUSIC_ENABLE_MP3=0）");
    return false;
}
#endif

/* ================================================================
 * 播放任务
 * ================================================================ */
typedef struct {
    char       *path;   /* strdup 得到，任务自己 free */
    music_fmt_t fmt;
} play_args_t;

static void music_task(void *arg)
{
    play_args_t *pa = (play_args_t *)arg;
    char       *path = pa->path;
    music_fmt_t fmt  = pa->fmt;
    free(pa);

    ESP_LOGI(TAG, "播放: %s (fmt=%d)", path, (int)fmt);

    /* 更新 current_name */
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    mp_lock();
    snprintf(s_current_name, sizeof(s_current_name), "%s", name);
    s_state = MUSIC_STATE_PLAYING;
    mp_unlock();

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen 失败: %s", path);
        goto done;
    }

    switch (fmt) {
    case MUSIC_FMT_WAV:
        decode_wav_stream(fp);
        break;
    case MUSIC_FMT_MP3:
        decode_mp3_stream(fp);
        break;
    default:
        ESP_LOGE(TAG, "未知格式 %d", (int)fmt);
        break;
    }

    fclose(fp);

done:
    /* 还原默认采样率，后续 TTS / GB 音频不会变调 */
    speaker_set_sample_rate(SPK_SAMPLE_RATE);

    mp_lock();
    s_state = MUSIC_STATE_IDLE;
    s_current_name[0] = '\0';
    s_task = NULL;
    mp_unlock();

    if (s_done_sem) xSemaphoreGive(s_done_sem);
    ESP_LOGI(TAG, "播放任务退出");
    free(path);
    vTaskDelete(NULL);
}

/* ================================================================
 * 公共 API
 * ================================================================ */
bool music_play(const char *path)
{
    if (!path) return false;

    if (!s_mtx)       s_mtx = xSemaphoreCreateMutex();
    if (!s_done_sem)  s_done_sem = xSemaphoreCreateBinary();
    if (!s_mtx || !s_done_sem) return false;

    /* 按后缀识别格式 */
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    music_fmt_t fmt   = fmt_from_name(name);
    if (fmt == MUSIC_FMT_UNKNOWN) {
        ESP_LOGE(TAG, "不支持的格式: %s", path);
        return false;
    }
#if !MUSIC_ENABLE_MP3
    if (fmt == MUSIC_FMT_MP3) {
        ESP_LOGE(TAG, "MP3 支持未编译，无法播放: %s", path);
        return false;
    }
#endif

    /* 如果已经在播，先 stop */
    if (s_state != MUSIC_STATE_IDLE) {
        music_stop();
    }

    char *path_dup = strdup(path);
    if (!path_dup) return false;

    play_args_t *pa = malloc(sizeof(*pa));
    if (!pa) { free(path_dup); return false; }
    pa->path = path_dup;
    pa->fmt  = fmt;

    mp_lock();
    s_stop_request  = false;
    s_pause_request = false;
    /* 清一下 done_sem 可能的旧 signal */
    xSemaphoreTake(s_done_sem, 0);

    /* 任务栈：
     *   WAV 路径实际用 ~2KB（header 解析 + memcpy）
     *   MP3 路径 helix decode 实测 ~6-7KB，加 fread(FATFS) 深调用余量
     *   给 10KB 足够（helix 官方示例也是 10KB 档），比之前 12KB 省 2KB DRAM。
     * 放内部 DRAM：helix 内部 const 表放 flash，关 cache 时不能访问→若
     * 从 PSRAM 栈切换后被抢占 + 同时刷 flash 可能崩。*/
    BaseType_t r = xTaskCreate(music_task, "music", 10240,
                               pa, 4, &s_task);
    if (r != pdPASS) {
        free(pa);
        free(path_dup);
        s_task = NULL;
        mp_unlock();
        ESP_LOGE(TAG, "任务创建失败");
        return false;
    }
    mp_unlock();
    return true;
}

/* music_stop：
 *   1. 原子置 stop_request，清 pause_request
 *   2. 循环等 s_done_sem（200ms × N 次）直到任务真正退出
 *   3. 超时兜底：直接检查 s_task 是否已被任务自己置 NULL
 *
 * 过去的实现只等 500ms 一次，超时就继续返回，容易产生僵尸任务（M4）。
 * 现在最多等 2 秒，期间持续轮询 s_task 状态；2 秒还没退出说明 music_task
 * 卡在某处（通常是 speaker_set_sample_rate 里 flush 等 tx），这属于严重
 * 异常，记 warn 日志返回，下次 music_play 会再次尝试 stop。*/
void music_stop(void)
{
    if (!s_mtx) return;

    mp_lock();
    bool has_task = (s_task != NULL);
    if (has_task) {
        s_stop_request  = true;
        s_pause_request = false;
    }
    mp_unlock();

    if (!has_task) return;

    /* 最多等 2 秒任务自己退出。每 200ms 检查一次：
     * - 能拿到 done_sem：任务已退出 ✓
     * - 拿不到 + s_task == NULL：任务在退出末尾自己清了 task 句柄也算退出 ✓
     * - 两者都不满足：继续等
     * 2 秒仍卡住：只能放弃等待记 warn，下次 stop 会再试一次 */
    bool done = false;
    for (int i = 0; i < 10; ++i) {
        if (s_done_sem && xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(200)) == pdTRUE) {
            done = true;
            break;
        }
        mp_lock();
        if (s_task == NULL) { done = true; mp_unlock(); break; }
        mp_unlock();
    }

    mp_lock();
    s_stop_request = false;
    mp_unlock();

    if (!done) {
        ESP_LOGW(TAG, "music_stop 等待任务退出超时（2s），可能有僵尸任务");
    }
}

void music_pause(void)
{
    mp_lock();
    if (s_state == MUSIC_STATE_PLAYING) {
        s_pause_request = true;
        s_state = MUSIC_STATE_PAUSED;
    }
    mp_unlock();
}

void music_resume(void)
{
    mp_lock();
    if (s_state == MUSIC_STATE_PAUSED) {
        s_pause_request = false;
        s_state = MUSIC_STATE_PLAYING;
    }
    mp_unlock();
}

music_state_t music_state(void)             { return s_state; }
const char   *music_current_name(void)      { return s_current_name; }
