#ifndef __MUSIC_H
#define __MUSIC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * 音乐模块（扫描 + 解码 + 播放）
 *
 * 音乐来自 SD 卡 /sdcard/music/ 下的音频文件。
 * 支持后缀（大小写不敏感）：
 *   .wav   RIFF/PCM 16bit mono 或 stereo，采样率 8k..48k
 *   .mp3   MPEG-1/2 Layer III（需启用 MUSIC_ENABLE_MP3）
 *
 * 架构：
 *   music_scan       列目录、按后缀过滤、返回条目清单
 *   music_play(path) 启动后台播放任务，内部按后缀派发给相应解码器
 *   music_stop       打断任务并等它退出
 *   music_pause/resume / state / current_name 播控 + 状态
 *
 * 播放任务内部：
 *   1. 打开文件，识别格式
 *   2. 解析头，提取采样率/声道数
 *   3. speaker_set_sample_rate() 切到文件速率
 *   4. 循环：读一块 → 解码成 PCM16 mono → speaker_play 推 ring
 *   5. 任务结束（文件读完或 stop）前还原采样率到 SPK_SAMPLE_RATE
 *
 * 只能同时播一首。调 music_play 时会先 stop 当前曲目。
 * ================================================================ */

#define MUSIC_MAX_NAME   64
#define MUSIC_MAX_COUNT  64

typedef enum {
    MUSIC_FMT_UNKNOWN = 0,
    MUSIC_FMT_WAV,
    MUSIC_FMT_MP3,
} music_fmt_t;

typedef struct {
    char       name[MUSIC_MAX_NAME];   /* 纯文件名，例如 "song.wav" */
    size_t     size;                   /* 文件字节数 */
    music_fmt_t fmt;                   /* 由扩展名推断 */
} music_entry_t;

typedef enum {
    MUSIC_STATE_IDLE = 0,
    MUSIC_STATE_PLAYING,
    MUSIC_STATE_PAUSED,
} music_state_t;

typedef enum {
    MUSIC_ERR_NONE = 0,
    MUSIC_ERR_INVALID_ARG,
    MUSIC_ERR_INIT_FAILED,
    MUSIC_ERR_UNSUPPORTED_FORMAT,
    MUSIC_ERR_NO_MEM,
    MUSIC_ERR_TASK_CREATE,
} music_error_t;

/* 扫描 /sdcard/music/ 下的支持文件，写入 list + 实际数量。
 * 返回 true 表示扫描成功（0 首歌也返回 true）；SD 未挂载返回 false。*/
bool music_scan(music_entry_t *list, int *count);

/* 根据文件名生成完整路径（/sdcard/music/name）。out 至少 96 字节。*/
void music_full_path(const char *name, char *out, size_t cap);

/* 启动播放。path 必须是 music_full_path 生成的完整路径。
 * 内部 strdup 路径，调用方可以释放参数。
 * 如果当前已在播放，先安全 stop 旧任务再启动新任务。*/
bool music_play(const char *path);

/* 必须在 app_main 启动阶段单线程调用一次，确保 mutex/sem 在任何并发调用前创建 */
void music_init(void);

/* music_stop：请求停止当前播放并等待后台任务退出。
 *
 * 会清 pause 状态并最多等待约 2 秒。若任务异常卡住，会记录 warning 后返回。 */
void music_stop(void);

/* music_pause：暂停当前播放任务推送 PCM。
 *
 * 只影响音乐模块，speaker 里已经缓冲的短尾音仍可能继续播完。 */
void music_pause(void);

/* music_resume：恢复被 music_pause 暂停的播放。 */
void music_resume(void);

/* music_state：返回当前播放器状态（空闲/播放/暂停）。 */
music_state_t music_state(void);

/* music_current_name：返回当前播放文件名。
 *
 * 返回指向模块内部静态字符串，调用方只读，不要保存用于长期跨任务修改。 */
const char   *music_current_name(void);

/* music_last_error：返回最近一次 music_play 失败原因枚举。 */
music_error_t music_last_error(void);

/* music_last_error_text：返回最近一次错误的可读短文本，便于 BLE/UI 提示。 */
const char   *music_last_error_text(void);

#endif
