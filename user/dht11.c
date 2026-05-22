#include "dht11.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define TAG "DHT11"

#define DHT11_GPIO          GPIO_NUM_13

/* 缓存有效期（毫秒）。DHT11 datasheet 要求两次采样最少间隔 1s，
 * 这里设 800ms 让上层 1s 触发的轮询能稳定命中下一次实测窗口
 * （1000ms - 800ms = 200ms 余量给一次实际握手 ~25ms + 调度抖动）。 */
#define CACHE_TTL_MS        800

/* SMP 安全的关键段保护。读 40 位需要 ~5ms 严格时序，期间不能被
 * 调度或中断打断。portMUX 同时禁中断 + 抢占（双核都不会被中断）。 */
static portMUX_TYPE s_critical_mux = portMUX_INITIALIZER_UNLOCKED;

/* ================================================================
 * 模块状态
 * ================================================================ */
static SemaphoreHandle_t s_mtx = NULL;

/* 上次成功读取的数据 + 时间戳（μs since boot） */
static int      s_cached_humi  = 0;
static int      s_cached_temp  = 0;
static int64_t  s_cached_us    = 0;   /* 0 表示无效 */

/* DHT11 上电后需要 ≥1 秒稳定时间，dht11_init 记录上电时刻；
 * 第一次读取若距上电不足 1s，先 delay 补够 */
static int64_t  s_init_us      = 0;

/* ================================================================
 * GPIO 总线操作
 * ================================================================ */
static inline void bus_output_low(void)
{
    gpio_set_direction(DHT11_GPIO, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(DHT11_GPIO, 0);
}

static inline void bus_release(void)
{
    /* 改回输入 + 上拉，让外部上拉电阻把线拉高 */
    gpio_set_direction(DHT11_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DHT11_GPIO, GPIO_PULLUP_ONLY);
}

static inline int bus_read(void)
{
    return gpio_get_level(DHT11_GPIO);
}

/* 等待引脚电平变成 expect_level，返回等待用时（μs），超时返回 -1。
 *
 * 用 esp_rom_delay_us 轮询而非中断采样：在 240MHz / -O2 下每次循环
 * 约 1μs，足够 DHT11 协议 26μs/70μs 的时间分辨率。 */
static int wait_level(int expect_level, int timeout_us)
{
    int elapsed = 0;
    while (bus_read() != expect_level) {
        if (elapsed >= timeout_us) return -1;
        esp_rom_delay_us(1);
        elapsed++;
    }
    return elapsed;
}

/* ================================================================
 * 一次完整握手 + 读取 5 字节
 * ================================================================ */
static bool dht11_sample_locked(int *out_humi, int *out_temp)
{
    uint8_t data[5] = {0};

    /* 1. 主机起始：拉低 ≥18ms（DHT11 要求 18ms 起，不能太短） */
    bus_output_low();
    vTaskDelay(pdMS_TO_TICKS(20));   /* 让 RTOS 调度，省 CPU */

    /* 2. 释放总线，让 DHT 接管。
     *    GPIO 方向切换 + 上拉模式属于 driver API，不能在临界段内调，
     *    所以在进临界段前先做。释放后 30μs 才进临界段开始读时序。 */
    bus_release();
    esp_rom_delay_us(30);   /* 等 20-40μs 让 DHT 接管总线 */

    /* 3. 进入 SMP 临界段：禁本核中断 + 抢占。临界段内只调用 gpio_get_level
     *    （IRAM 安全的寄存器读），不能再调 gpio_set_direction 等 driver API。 */
    portENTER_CRITICAL(&s_critical_mux);

    /* 4. DHT 应答：低 80μs → 高 80μs */
    if (wait_level(0, 100) < 0) goto fail;   /* 等 DHT 拉低 */
    if (wait_level(1, 120) < 0) goto fail;   /* 等 DHT 拉高 */
    if (wait_level(0, 120) < 0) goto fail;   /* 等 DHT 重新拉低（数据起始） */

    /* 5. 读 40 位数据：每位 = 50μs 低 + (26~28μs 高=0 / 70μs 高=1) */
    for (int i = 0; i < 40; ++i) {
        if (wait_level(1, 80) < 0) goto fail;       /* 等 50μs 低结束 */
        int high_us = wait_level(0, 100);            /* 测量高电平时长 */
        if (high_us < 0) goto fail;

        /* 阈值 40μs：>40μs 判 1，否则判 0 */
        data[i / 8] <<= 1;
        if (high_us > 40) data[i / 8] |= 1;
    }

    portEXIT_CRITICAL(&s_critical_mux);

    /* 6. 校验和：前四字节累加低 8 位 == 第五字节 */
    uint8_t sum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (sum != data[4]) {
        /* warmup / 偶发毛刺都会到这里。降到 DEBUG 级别避免污染日志，
         * 上层会重试，最终失败才在 env UI 显示一次"读取失败"。 */
        ESP_LOGD(TAG, "checksum 错误: %02x %02x %02x %02x sum=%02x recv=%02x",
                 data[0], data[1], data[2], data[3], sum, data[4]);
        return false;
    }

    /* DHT11 整数分辨率：data[0]=湿度整数, data[2]=温度整数
     * data[1] / data[3] 恒为 0 不用读 */
    *out_humi = data[0];
    *out_temp = data[2];
    return true;

fail:
    portEXIT_CRITICAL(&s_critical_mux);
    return false;
}

/* 取最新数据（缓存命中直接返回，否则触发一次实测） */
static bool dht11_get_locked(int *out_humi, int *out_temp)
{
    int64_t now_us = esp_timer_get_time();

    /* 缓存有效：< CACHE_TTL_MS 内的旧数据可复用 */
    if (s_cached_us != 0 &&
        (now_us - s_cached_us) < (int64_t)CACHE_TTL_MS * 1000) {
        if (out_humi) *out_humi = s_cached_humi;
        if (out_temp) *out_temp = s_cached_temp;
        return true;
    }

    /* DHT11 datasheet：上电后 1s 内传感器未稳定，握手必失败。
     * 第一次实测前补足 1s 的等待，之后路径不再触发。 */
    if (s_init_us != 0) {
        int64_t since_boot_us = now_us - s_init_us;
        if (since_boot_us < 1000 * 1000) {
            int wait_ms = (int)((1000 * 1000 - since_boot_us) / 1000) + 50;
            ESP_LOGI(TAG, "等 DHT11 上电稳定 %d ms", wait_ms);
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }

    /* 首次实测前做一次静默 warmup：DHT11 第一次被握手后需要 ~100ms
     * 才能回稳定数据，加上 ESP-IDF GPIO driver 第一次调用的冷启动开销，
     * 真正的第一次实测大概率失败。这里先做一次握手把传感器和驱动都
     * 唤醒，结果丢弃（不日志、不算失败），用户看不到这次"哑读"。 */
    static bool s_warmup_done = false;
    if (!s_warmup_done) {
        int dh = 0, dt = 0;
        (void)dht11_sample_locked(&dh, &dt);
        s_warmup_done = true;
        vTaskDelay(pdMS_TO_TICKS(150));  /* 让 DHT11 充分恢复 */
    }

    /* 实测重试：最多 3 次，间隔逐步拉长。这套节奏对市面上各种 DHT11
     * 仿品兼容性最好（部分仿品时序偏离 datasheet ~10%）。 */
    int humi = 0, temp = 0;
    bool ok = false;
    static const int retry_delays_ms[3] = { 0, 80, 200 };
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (retry_delays_ms[attempt] > 0) {
            vTaskDelay(pdMS_TO_TICKS(retry_delays_ms[attempt]));
        }
        ok = dht11_sample_locked(&humi, &temp);
        if (ok) break;
    }
    if (!ok) return false;

    /* 合理性校验：DHT11 工作量程 0-50℃ / 20-90%RH。
     * 偶尔遇到全 0xff 或异常大值，直接当通讯失败处理。 */
    if (temp > 60 || humi > 95) {
        ESP_LOGW(TAG, "数据异常 humi=%d temp=%d，丢弃", humi, temp);
        return false;
    }

    s_cached_humi = humi;
    s_cached_temp = temp;
    s_cached_us   = now_us;

    if (out_humi) *out_humi = humi;
    if (out_temp) *out_temp = temp;
    return true;
}

/* ================================================================
 * 公共 API
 * ================================================================ */

void dht11_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << DHT11_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* 记录上电时刻，第一次读取前自动补足 1s 稳定时间 */
    s_init_us = esp_timer_get_time();

    ESP_LOGI(TAG, "DHT11 初始化完成 GPIO=%d", DHT11_GPIO);
}

/* 读取温度。内部会走缓存/实测路径，并用 mutex 串行化 DHT11 单总线时序。 */
bool dht11_read_temp(int *out_temp_c)
{
    if (!s_mtx || !out_temp_c) return false;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return false;

    int humi = 0, temp = 0;
    bool ok = dht11_get_locked(&humi, &temp);
    if (ok) *out_temp_c = temp;

    xSemaphoreGive(s_mtx);
    return ok;
}

/* 读取湿度。与温度读取共享同一份缓存，避免上层连续读温湿度时重复握手。 */
bool dht11_read_humi(int *out_humi_pct)
{
    if (!s_mtx || !out_humi_pct) return false;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return false;

    int humi = 0, temp = 0;
    bool ok = dht11_get_locked(&humi, &temp);
    if (ok) *out_humi_pct = humi;

    xSemaphoreGive(s_mtx);
    return ok;
}
