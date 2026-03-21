#include "gb2312.h"

// UTF-8 单字符解码为 Unicode 码点
static uint32_t utf8_decode(const char *s, int *bytes_consumed)
{
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80) {
        *bytes_consumed = 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        *bytes_consumed = 2;
        return ((c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
    } else if ((c & 0xF0) == 0xE0) {
        *bytes_consumed = 3;
        return ((c & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F);
    }
    *bytes_consumed = 1;
    return '?';
}

// Unicode 码点转 GB2312（只覆盖常用汉字范围）
// GB2312 区位码与 Unicode 的映射通过查表实现
// 这里用 iconv 思路：GB2312 = 0xB0A1~0xF7FE 对应 Unicode 一级汉字
// 简化实现：仅支持 ASCII + 常用 GB2312 汉字
static int unicode_to_gb2312(uint32_t cp, uint8_t *out)
{
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    // 使用 esp-idf 内置的 newlib，通过标准库转换
    // 直接硬编码：将 UTF-8 源码字符在编译时已经是确定字节
    // 这里提供一个小型 Unicode->GB2312 表（仅静夜思所需汉字）
    static const struct { uint32_t unicode; uint8_t gb[2]; } tbl[] = {
        {0x9759, {0xbe, 0xb2}}, // 静
        {0x591c, {0xd2, 0xb9}}, // 夜
        {0x601d, {0xcb, 0xbc}}, // 思
        {0x5e8a, {0xb4, 0xb2}}, // 床
        {0x524d, {0xc7, 0xb0}}, // 前
        {0x660e, {0xc3, 0xf7}}, // 明
        {0x6708, {0xd4, 0xc2}}, // 月
        {0x5149, {0xb9, 0xe2}}, // 光
        {0x7591, {0xd2, 0xc9}}, // 疑
        {0x662f, {0xca, 0xc7}}, // 是
        {0x5730, {0xb5, 0xd8}}, // 地
        {0x4e0a, {0xc9, 0xcf}}, // 上
        {0x971c, {0xcb, 0xaa}}, // 霜
        {0x4e3e, {0xbe, 0xd9}}, // 举
        {0x5934, {0xcd, 0xb7}}, // 头
        {0x671b, {0xcd, 0xfb}}, // 望
        {0x4f4e, {0xb5, 0xcd}}, // 低
        {0x6545, {0xb9, 0xca}}, // 故
        {0x4e61, {0xcf, 0xe7}}, // 乡
    };
    for (int i = 0; i < (int)(sizeof(tbl)/sizeof(tbl[0])); i++) {
        if (tbl[i].unicode == cp) {
            out[0] = tbl[i].gb[0];
            out[1] = tbl[i].gb[1];
            return 2;
        }
    }
    out[0] = '?';
    return 1;
}

int utf8_to_gb2312(const char *utf8, uint8_t *buf, int buf_size)
{
    int out_len = 0;
    int i = 0;
    int len = strlen(utf8);
    while (i < len && out_len < buf_size - 2) {
        int consumed;
        uint32_t cp = utf8_decode(utf8 + i, &consumed);
        uint8_t tmp[2];
        int n = unicode_to_gb2312(cp, tmp);
        if (out_len + n < buf_size) {
            buf[out_len++] = tmp[0];
            if (n == 2) buf[out_len++] = tmp[1];
        }
        i += consumed;
    }
    buf[out_len] = 0;
    return out_len;
}
