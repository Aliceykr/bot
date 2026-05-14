#ifndef __BAIDU_TOKEN_H
#define __BAIDU_TOKEN_H

/*
 * 百度 OAuth2 access_token 共享缓存
 *
 * ASR 和 TTS 使用同一个百度应用（相同 API key/secret），token 可共用。
 * 模块内部带过期时间管理：
 *   - 首次调用时请求并缓存
 *   - 过期前 1 小时自动主动刷新
 *   - 调用方遇到 401/token 失效错误可 invalidate 强制下次重取
 * 使用 mutex 保护，ASR/TTS 并发调用安全。
 */

/* 获取当前有效 token 字符串。
 * 返回非 NULL：有效 token，调用方只需读取不能释放
 * 返回 NULL：获取失败（网络错误或认证失败）
 *
 * 注意：返回的指针指向模块内部 static 缓冲区，释放锁后可能被其他线程
 * 覆写。如果需要跨时间使用 token，请用 baidu_token_copy() 代替。
 */
const char *baidu_token_get(void);

/* 线程安全地将当前有效 token 拷贝到调用方缓冲区。
 * 返回 true 表示成功（buf 已填充 \0 结尾的 token）；
 * 返回 false 表示获取失败（网络错误或认证失败），buf 内容未定义。
 * buf_size 建议 >= 256。
 */
bool baidu_token_copy(char *buf, size_t buf_size);

/* 强制失效当前缓存，下次调用 baidu_token_get 会重新请求。
 * 在 API 返回 401 或 token 相关错误码时调用。 */
void baidu_token_invalidate(void);

/* 必须在 app_main 启动阶段单线程调用一次，确保 mutex 在任何并发调用前创建 */
void baidu_token_init(void);

#endif /* __BAIDU_TOKEN_H */
