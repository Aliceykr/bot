#include "mpu6050.h"
#include <string.h>
#include <math.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "MPU6050"

/* ================================================================
 * 硬件配置
 * ================================================================ */
#define MPU_I2C_PORT      I2C_NUM_0
#define MPU_I2C_SDA_PIN   13
#define MPU_I2C_SCL_PIN   7
#define MPU_I2C_FREQ_HZ   400000   /* 400kHz fast mode */

#define MPU_ADDR          0x68     /* AD0 接 GND */

/* 寄存器地址 */
#define MPU_REG_PWR_MGMT_1   0x6B
#define MPU_REG_SMPLRT_DIV   0x19
#define MPU_REG_CONFIG       0x1A
#define MPU_REG_ACCEL_CONFIG 0x1C
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_WHO_AM_I     0x75

/* ±2g 量程下的灵敏度：16384 LSB/g */
#define ACCEL_SENS_2G        16384.0f

/* 倾斜判定阈值（g）。X/Y 方向加速度绝对值超过此值才算"已倾斜" */
#define TILT_THRESHOLD_G     0.30f

/* I2C 操作超时（毫秒）。
 * 400kHz 下读写 6 字节 < 1ms，10ms 足够覆盖任何正常情况。
 * 设短超时是为了避免 MPU6050 接线松动 / 总线挂死时阻塞游戏主循环
 * （主循环 20ms/轮，原 100ms 超时会导致 5 帧画面卡顿）。 */
#define MPU_I2C_TIMEOUT_MS  10

/* ================================================================
 * 状态
 * ================================================================ */
static i2c_master_bus_handle_t s_bus    = NULL;
static i2c_master_dev_handle_t s_dev    = NULL;
static bool                    s_ready  = false;

/* 连续读取失败计数：用于游戏期间检测 MPU6050 掉线，
 * 静默失效时给用户一个 warning 提示 */
static uint32_t s_read_fail_streak = 0;
#define READ_FAIL_WARN_THRESHOLD  20    /* 20 帧 ≈ 0.4s 都失败才告警 */

/* ================================================================
 * 辅助：读写寄存器
 * ================================================================ */
static esp_err_t mpu_write_byte(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), MPU_I2C_TIMEOUT_MS);
}

static esp_err_t mpu_read_bytes(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, MPU_I2C_TIMEOUT_MS);
}

/* 内部清理：与 mpu6050_deinit 相同但不打"已释放"日志，
 * 给 init 失败回滚使用，避免误导调试者。 */
static void cleanup_resources(void)
{
    s_ready = false;
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

/* ================================================================
 * 公共 API
 * ================================================================ */
bool mpu6050_init(void)
{
    if (s_ready) return true;

    /* 1. 创建 I2C 主机总线（如果尚未创建） */
    if (!s_bus) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = MPU_I2C_PORT,
            .sda_io_num = MPU_I2C_SDA_PIN,
            .scl_io_num = MPU_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus 失败: %s", esp_err_to_name(err));
            s_bus = NULL;
            return false;
        }
    }

    /* 2. 添加从设备 */
    if (!s_dev) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = MPU_ADDR,
            .scl_speed_hz = MPU_I2C_FREQ_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add_device 失败: %s", esp_err_to_name(err));
            s_dev = NULL;
            i2c_del_master_bus(s_bus);
            s_bus = NULL;
            return false;
        }
    }

    /* 3. 探测 WHO_AM_I 寄存器，确认设备在线（应返回 0x68） */
    uint8_t who = 0;
    if (mpu_read_bytes(MPU_REG_WHO_AM_I, &who, 1) != ESP_OK) {
        ESP_LOGE(TAG, "读 WHO_AM_I 失败，请检查接线");
        cleanup_resources();
        return false;
    }
    /* 部分山寨 MPU6050 / MPU6500 可能返回 0x70 / 0x68 / 0x71，宽松匹配 */
    if (who != 0x68 && who != 0x70 && who != 0x71 && who != 0x72) {
        ESP_LOGW(TAG, "WHO_AM_I=0x%02X（非典型值，仍尝试初始化）", who);
    } else {
        ESP_LOGI(TAG, "WHO_AM_I=0x%02X，设备识别成功", who);
    }

    /* 4. 唤醒（清除 SLEEP 位），用 PLL 时钟源（更稳定） */
    if (mpu_write_byte(MPU_REG_PWR_MGMT_1, 0x01) != ESP_OK) {
        ESP_LOGE(TAG, "写 PWR_MGMT_1 失败");
        cleanup_resources();
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));   /* 等设备稳定 */

    /* 5. 配置：采样率 100Hz，DLPF 截止 ~44Hz，加速度量程 ±2g。
     * 写入失败不阻断初始化（设备会按上电默认值运行：1kHz/无 DLPF/±2g），
     * 但记 warning 便于排查 I2C 不稳问题。 */
    esp_err_t e1 = mpu_write_byte(MPU_REG_SMPLRT_DIV,   0x09);  /* 1kHz / (1+9) = 100Hz */
    esp_err_t e2 = mpu_write_byte(MPU_REG_CONFIG,       0x03);  /* DLPF: 44Hz */
    esp_err_t e3 = mpu_write_byte(MPU_REG_ACCEL_CONFIG, 0x00);  /* ±2g */
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
        ESP_LOGW(TAG, "配置寄存器部分写入失败 (smprt=%d cfg=%d accel=%d)，"
                      "传感器将以默认参数运行", e1, e2, e3);
    }

    s_ready = true;
    s_read_fail_streak = 0;
    ESP_LOGI(TAG, "MPU6050 初始化完成 (SDA=%d SCL=%d ±2g 100Hz)",
             MPU_I2C_SDA_PIN, MPU_I2C_SCL_PIN);
    return true;
}

void mpu6050_deinit(void)
{
    s_ready = false;
    if (s_dev) {
        /* 先把设备置为 sleep 模式（PWR_MGMT_1 bit6=1）省电。
         * 共享 3.3V 时如果不 sleep，MPU 仍以 100Hz 采样 ~3.6mA，
         * 不显著但能省一点电。这里允许失败（设备已掉线时）。 */
        mpu_write_byte(MPU_REG_PWR_MGMT_1, 0x40);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    s_read_fail_streak = 0;
    ESP_LOGI(TAG, "MPU6050 已释放");
}

bool mpu6050_is_ready(void)
{
    return s_ready;
}

bool mpu6050_read_accel(float *ax, float *ay, float *az)
{
    if (!s_ready) return false;

    /* 一次连读 6 字节：ACCEL_X/Y/Z 高低字节 */
    uint8_t buf[6];
    if (mpu_read_bytes(MPU_REG_ACCEL_XOUT_H, buf, 6) != ESP_OK) {
        /* 连续失败 N 次告警一次。游戏期间用户可能感知不到倾斜失效，
         * 通过 log 给排查者一个提示（可能是接线松动或 I2C 总线挂死）。
         * 用 == 而不是 >= 确保只告警一次，不刷屏。 */
        s_read_fail_streak++;
        if (s_read_fail_streak == READ_FAIL_WARN_THRESHOLD) {
            ESP_LOGW(TAG, "I2C 连续读取失败 %u 次，传感器可能掉线",
                     (unsigned)READ_FAIL_WARN_THRESHOLD);
        }
        return false;
    }
    /* 成功一次就清零计数，恢复正常运行的告警门限 */
    if (s_read_fail_streak >= READ_FAIL_WARN_THRESHOLD) {
        ESP_LOGI(TAG, "I2C 读取已恢复");
    }
    s_read_fail_streak = 0;

    int16_t raw_x = (int16_t)((buf[0] << 8) | buf[1]);
    int16_t raw_y = (int16_t)((buf[2] << 8) | buf[3]);
    int16_t raw_z = (int16_t)((buf[4] << 8) | buf[5]);

    if (ax) *ax = (float)raw_x / ACCEL_SENS_2G;
    if (ay) *ay = (float)raw_y / ACCEL_SENS_2G;
    if (az) *az = (float)raw_z / ACCEL_SENS_2G;
    return true;
}

mpu_dir_t mpu6050_get_direction(void)
{
    float ax, ay, az;
    if (!mpu6050_read_accel(&ax, &ay, &az)) return MPU_DIR_NONE;

    /* 取 X/Y 绝对值大的那个判方向。Z 轴用于检测设备朝向但不参与方向决策。
     *
     * 坐标约定（MPU6050 PCB 平放，标签朝上）：
     *   设备静止 → ax≈0, ay≈0, az≈+1g（重力指向 -Z）
     *   设备右倾（右侧抬高）→ ax 变负
     *   设备左倾（左侧抬高）→ ax 变正
     *   设备前倾（前端低）  → ay 变负
     *   设备后倾（前端高）  → ay 变正
     *
     * 实际安装时如果方向反了，调整下面的判断符号即可。
     * 默认按"屏幕朝上、MPU 与屏幕同向"配置：
     *   ax > 阈值 → 设备左倾 → 游戏 LEFT
     *   ax < -阈值 → 设备右倾 → 游戏 RIGHT
     *   ay > 阈值 → 设备后倾（屏幕顶部抬高）→ 游戏 UP
     *   ay < -阈值 → 设备前倾（屏幕底部抬高）→ 游戏 DOWN
     */
    float abs_x = fabsf(ax);
    float abs_y = fabsf(ay);

    if (abs_x < TILT_THRESHOLD_G && abs_y < TILT_THRESHOLD_G) {
        return MPU_DIR_NONE;  /* 平放，不动 */
    }

    if (abs_x > abs_y) {
        /* X 主导：左右 */
        return (ax > 0) ? MPU_DIR_LEFT : MPU_DIR_RIGHT;
    } else {
        /* Y 主导：上下 */
        return (ay > 0) ? MPU_DIR_UP : MPU_DIR_DOWN;
    }
}
