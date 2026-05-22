#ifndef __SNTP_TIME_H
#define __SNTP_TIME_H

#include <stdbool.h>
#include <time.h>

/* sntp_time_sync：启动 SNTP 并等待一次网络时间同步完成。
 *
 * 需在 WiFi 连接后调用；timeout_ms 为最大等待时间。
 * 成功后模块会记录已同步状态，后续 sntp_time_get 可直接读本地时间。 */
bool sntp_time_sync(uint32_t timeout_ms);

/* sntp_time_get：读取当前本地时间到 tm。
 *
 * 返回 true 表示至少完成过一次 SNTP 同步；未同步时仍会填充系统当前时间，
 * 但调用方应把返回 false 当作“不可信时间”。 */
bool sntp_time_get(struct tm *out);

/* sntp_time_is_synced：查询是否已经完成过一次 SNTP 同步。 */
bool sntp_time_is_synced(void);

#endif
