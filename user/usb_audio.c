#include "usb_audio.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#define TAG             "USB_AUDIO"
#define RINGBUF_SIZE    (128 * 1024)  // 128 KB ≈ 4 秒缓冲
#define TX_CHUNK        512           // 每次发送块大小
#define TX_TASK_STACK   2048
#define TX_TASK_PRIO    3             // 低于 I2S 采集任务，避免抢占

static RingbufHandle_t s_ringbuf = NULL;
static bool s_usb_ready = false;

static void usb_line_state_cb(int itf, cdcacm_event_t *event)
{
    if (event->line_state_changed_data.dtr) {
        s_usb_ready = true;
        ESP_LOGI(TAG, "PC 已连接，开始接收音频");
    } else {
        s_usb_ready = false;
        ESP_LOGI(TAG, "PC 已断开");
    }
}

// ================================================================
// 发送任务：从 RingBuffer 读取并通过 CDC 发送
// ================================================================
static void usb_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        size_t recv_size = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &recv_size, pdMS_TO_TICKS(20), TX_CHUNK);
        if (item == NULL) {
            taskYIELD();
            continue;
        }
        if (s_usb_ready) {
            // tud_cdc_write 最多写到内部 FIFO，循环直到全部写完
            uint8_t *ptr = (uint8_t *)item;
            size_t remaining = recv_size;
            while (remaining > 0) {
                uint32_t written = tud_cdc_write(ptr, remaining);
                tud_cdc_write_flush();
                ptr      += written;
                remaining -= written;
                if (written == 0) {
                    // FIFO 暂满，让出 CPU 等待下次
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
        }
        // 无论是否发送都必须归还 item
        vRingbufferReturnItem(s_ringbuf, item);
    }
}

// ================================================================
// 公开接口
// ================================================================
void usb_audio_init(void)
{
    // 创建 RingBuffer（字节模式，存于内部 SRAM）
    s_ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "RingBuffer 创建失败");
        return;
    }

    // 初始化 TinyUSB
    tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,  // 使用 tusb_config.h 中的默认描述符
        .string_descriptor = NULL,
        .external_phy = false,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = usb_line_state_cb,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));

    // 启动发送任务
    xTaskCreate(usb_tx_task, "usb_tx", TX_TASK_STACK, NULL, TX_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "USB Audio CDC 初始化完成");
}

int usb_audio_send(const int16_t *pcm, size_t len_bytes)
{
    if (!s_ringbuf || len_bytes == 0) return 0;
    // xRingbufferSend 失败时不阻塞（timeout=0），直接丢弃，保证实时性
    BaseType_t ret = xRingbufferSend(s_ringbuf, pcm, len_bytes, 0);
    return (ret == pdTRUE) ? (int)len_bytes : 0;
}
