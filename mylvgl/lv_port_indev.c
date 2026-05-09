#include "lv_port_indev.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "INDEV"

#define ENC_A_PIN   GPIO_NUM_4
#define ENC_B_PIN   GPIO_NUM_5
#define ENC_SW_PIN  GPIO_NUM_6

/* 按键状态机轮询周期（A/B 相由 PCNT 硬件计数，不再轮询）*/
#define SW_POLL_MS 5
/* 按键防抖时长（ms），须为 SW_POLL_MS 的整数倍 */
#define SW_DEBOUNCE_MS 20

/* 编码器每"卡位"脉冲数。
 * 常见旋转编码器一个机械卡位产生 4 个正交脉冲（四倍频解码），
 * 但有些编码器（如 EC11 的某些型号）只产生 2 个脉冲。
 * 若你的编码器转动一个卡位 LVGL 跳 >1 格，把这个值调大（4）。
 * 若转多次才跳一格，调小（1）。 */
#define ENC_PULSES_PER_STEP 2

static lv_indev_t *indev_encoder;

/* PCNT 单元：硬件累积正交计数，溢出自动回绕 */
static pcnt_unit_handle_t s_pcnt = NULL;
static int32_t s_last_count = 0;

/* 按键状态机：由 encoder_task 维护，LVGL 线程只读 s_sw_state */
typedef enum {
    SW_IDLE,          /* 空闲，等待低电平 */
    SW_DEBOUNCE,      /* 检测到低电平，防抖计时中 */
    SW_PRESSED_WAIT,  /* 已上报按下，等待释放 */
} sw_phase_t;

static volatile lv_indev_state_t s_sw_state = LV_INDEV_STATE_RELEASED;

/* 按键扫描任务：5ms 周期运行按键状态机，不阻塞、不占用重度 CPU */
static void encoder_task(void *arg)
{
    (void)arg;
    sw_phase_t sw_phase = SW_IDLE;
    uint32_t   sw_tick  = 0;

    while (1) {
        int sw = gpio_get_level(ENC_SW_PIN);
        switch (sw_phase) {
        case SW_IDLE:
            if (sw == 0) {
                sw_phase = SW_DEBOUNCE;
                sw_tick  = 0;
            }
            break;
        case SW_DEBOUNCE:
            if (sw != 0) {
                sw_phase = SW_IDLE;  /* 毛刺 */
            } else {
                sw_tick += SW_POLL_MS;
                if (sw_tick >= SW_DEBOUNCE_MS) {
                    s_sw_state = LV_INDEV_STATE_PRESSED;
                    sw_phase   = SW_PRESSED_WAIT;
                }
            }
            break;
        case SW_PRESSED_WAIT:
            if (sw != 0) {
                s_sw_state = LV_INDEV_STATE_RELEASED;
                sw_phase   = SW_IDLE;
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(SW_POLL_MS));
    }
}

/* LVGL 输入读回调：从 PCNT 取增量，除以每卡位脉冲数 */
static void encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (s_pcnt) {
        int cur = 0;
        pcnt_unit_get_count(s_pcnt, &cur);
        int32_t diff = (int32_t)cur - s_last_count;
        s_last_count = cur;
        data->enc_diff = diff / ENC_PULSES_PER_STEP;
        /* 未达一整卡位的残余脉冲保留在 s_last_count 差值里；
         * 但每次 read 后我们都已经 s_last_count = cur，残余被丢弃。
         * 为避免丢失残余，只累加已产生完整卡位的量： */
        int32_t consumed = (diff / ENC_PULSES_PER_STEP) * ENC_PULSES_PER_STEP;
        s_last_count = cur - (diff - consumed);
    } else {
        data->enc_diff = 0;
    }
    data->state = s_sw_state;
}

/* PCNT 初始化：A 相为边沿信号，B 相为电平控制，形成正交计数 */
static bool pcnt_init(void)
{
    pcnt_unit_config_t unit_cfg = {
        .high_limit = 32767,
        .low_limit  = -32768,
        .flags.accum_count = 1,  /* 溢出自动累加，避免越界复位导致丢数 */
    };
    if (pcnt_new_unit(&unit_cfg, &s_pcnt) != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_unit 失败");
        return false;
    }

    /* 加 glitch filter 过滤 <1μs 的噪声（编码器接触弹跳）*/
    pcnt_glitch_filter_config_t filter_cfg = { .max_glitch_ns = 1000 };
    pcnt_unit_set_glitch_filter(s_pcnt, &filter_cfg);

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num  = ENC_A_PIN,
        .level_gpio_num = ENC_B_PIN,
    };
    pcnt_channel_handle_t chan = NULL;
    if (pcnt_new_channel(s_pcnt, &chan_cfg, &chan) != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_channel 失败");
        return false;
    }
    /* A 上升沿 & B=1 → 增；A 上升沿 & B=0 → 减
     * A 下降沿则反向。这是标准正交解码（四倍频）*/
    pcnt_channel_set_edge_action(chan,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(chan,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    pcnt_unit_enable(s_pcnt);
    pcnt_unit_clear_count(s_pcnt);
    pcnt_unit_start(s_pcnt);
    s_last_count = 0;
    ESP_LOGI(TAG, "PCNT 编码器初始化完成 A=%d B=%d", ENC_A_PIN, ENC_B_PIN);
    return true;
}

void lv_port_indev_init(void)
{
    /* SW 引脚手动配置为输入 + 上拉；A/B 引脚由 PCNT 接管 */
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << ENC_SW_PIN);
    io_conf.pull_up_en   = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);

    /* A/B 引脚也需要上拉（PCNT 只消费信号不负责电气配置）*/
    gpio_config_t ab_conf = {};
    ab_conf.intr_type    = GPIO_INTR_DISABLE;
    ab_conf.mode         = GPIO_MODE_INPUT;
    ab_conf.pin_bit_mask = (1ULL << ENC_A_PIN) | (1ULL << ENC_B_PIN);
    ab_conf.pull_up_en   = 1;
    ab_conf.pull_down_en = 0;
    gpio_config(&ab_conf);

    pcnt_init();

    /* 按键扫描任务（5ms 周期，不再轮询 A/B）*/
    xTaskCreate(encoder_task, "enc_task", 2048, NULL, 6, NULL);

    /* 注册 LVGL 输入设备 */
    indev_encoder = lv_indev_create();
    lv_indev_set_type(indev_encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev_encoder, encoder_read);
}
