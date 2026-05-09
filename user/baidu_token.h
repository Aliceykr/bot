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
 */
const char *baidu_token_get(void);

/* 强制失效当前缓存，下次调用 baidu_token_get 会重新请求。
 * 在 API 返回 401 或 token 相关错误码时调用。 */
void baidu_token_invalidate(void);

#endif /* __BAIDU_TOKEN_H */
