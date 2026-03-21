#include "lv_port_indev.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ENC_A_PIN   GPIO_NUM_4
#define ENC_B_PIN   GPIO_NUM_5
#define ENC_SW_PIN  GPIO_NUM_6

static lv_indev_t *indev_encoder;
static volatile int32_t enc_diff = 0;   // 编码器差值
static volatile bool enc_pressed = false;
static int last_a = 1;

// 编码器读取任务
static void encoder_task(void *arg)
{
    while (1) {
        int a = gpio_get_level(ENC_A_PIN);
        int b = gpio_get_level(ENC_B_PIN);

        if (a != last_a && a == 1) {  // 只在上升沿计数
            if (b != a) enc_diff++;
            else        enc_diff--;
        }
        last_a = a;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// 按键防抖检测
static bool read_sw(void)
{
    if (gpio_get_level(ENC_SW_PIN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(ENC_SW_PIN) == 0) {
            // 等待释放
            while (gpio_get_level(ENC_SW_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(5));
            return true;
        }
    }
    return false;
}

static void encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->enc_diff = enc_diff;
    enc_diff = 0;

    if (read_sw()) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void lv_port_indev_init(void)
{
    // 配置引脚
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL<<ENC_A_PIN)|(1ULL<<ENC_B_PIN)|(1ULL<<ENC_SW_PIN);
    io_conf.pull_up_en   = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);

    last_a = gpio_get_level(ENC_A_PIN);

    // 启动编码器读取任务
    xTaskCreate(encoder_task, "enc_task", 2048, NULL, 6, NULL);

    // 注册 LVGL 输入设备
    indev_encoder = lv_indev_create();
    lv_indev_set_type(indev_encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev_encoder, encoder_read);
}
