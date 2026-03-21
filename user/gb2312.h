#ifndef __GB2312_H
#define __GB2312_H

#include <stdint.h>
#include <string.h>

// 将 UTF-8 编码的中文字符串转换为 GB2312，存入 buf
// buf 大小至少为 strlen(utf8)*2+1
// 返回转换后的字节数
int utf8_to_gb2312(const char *utf8, uint8_t *buf, int buf_size);

#endif
