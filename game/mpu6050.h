#ifndef __MPU6050_H
#define __MPU6050_H

#include <stdbool.h>
#include <stdint.h>

/*
 * MPU6050 六轴传感器驱动（I2C 接口）
 *
 * 硬件接线：
 *   SDA  → GPIO20
 *   SCL  → GPIO7
 *   VCC  → 3.3V
 *   GND  → GND
 *   AD0  → GND（地址 0x68）
 *
 * ============ 使用约束 ============
 *
 * 1. 仅在 2048 游戏中使用：
 *    本驱动只在 game_2048_run() 进入时 init、退出时 deinit。
 *    其他场景（菜单、聊天、语音等）一律不要调用，以节省 RAM 占用
 *    （I2C 总线驱动 + DMA 缓冲约 ~2KB DRAM，仅游戏期间临时占用）。
 *
 * 2. I2C_NUM_0 独占：
 *    本模块独占 I2C_NUM_0 端口和 GPIO20/7。如果将来其他模块（OLED、
 *    其他传感器等）需要使用 I2C，应该走 I2C_NUM_1 或者重构为共享
 *    I2C bus 设计（先创建 bus 再 add device）。当前 deinit 会
 *    完整销毁 bus，与"共享"模式不兼容。
 *
 * 3. 非线程安全：
 *    init/deinit/read 系列函数都没有互斥保护。当前仅在 2048 主循环
 *    单线程中调用。如果将来从多任务并发调用，需要在外层加锁。
 *
 * 4. 方向值与游戏 dir int 值的契约：
 *    mpu_dir_t 的整数值与 game_2048.c::parse_direction() 返回的
 *    方向 int 值必须保持一致（NONE=0, UP=1, DOWN=2, LEFT=3, RIGHT=4），
 *    这样 game_2048.c 可以直接把 tilt 强转传给 do_move(int)。
 *    若修改此处枚举或游戏方向编码，必须同步修改另一处。
 */

/* 倾斜方向（与 2048 / GB 的方向键映射对齐）
 *
 * 数值约定（与 game_2048.c::parse_direction() 返回值保持一致）：
 *   0 = NONE, 1 = UP, 2 = DOWN, 3 = LEFT, 4 = RIGHT
 *
 * game_2048.c 中有 _Static_assert 校验此约定，修改此枚举会触发编译错误。 */
typedef enum {
    MPU_DIR_NONE  = 0,
    MPU_DIR_UP    = 1,   /* 向上倾斜 */
    MPU_DIR_DOWN  = 2,   /* 向下倾斜 */
    MPU_DIR_LEFT  = 3,   /* 向左倾斜 */
    MPU_DIR_RIGHT = 4,   /* 向右倾斜 */
} mpu_dir_t;

/* 初始化 I2C 总线 + MPU6050。仅在游戏主循环中调用。
 * 成功返回 true。重复调用幂等，但不要在多任务并发场景下调用。 */
bool mpu6050_init(void);

/* 释放 I2C 总线，回收资源。退出游戏时调用。 */
void mpu6050_deinit(void);

/* 是否初始化成功（用于上层判断是否启用倾斜控制） */
bool mpu6050_is_ready(void);

/* 读取一次加速度（单位 g，浮点）。
 * 返回 false 表示读取失败（I2C 错误或未初始化）。 */
bool mpu6050_read_accel(float *ax, float *ay, float *az);

/* 高层接口：读取一次加速度并返回当前倾斜方向。
 * 初始化时会把当前姿态校准为中立点；内部带阈值、滞后和连续样本确认。
 * 回到中立姿态时返回 MPU_DIR_NONE。 */
mpu_dir_t mpu6050_get_direction(void);

#endif /* __MPU6050_H */
