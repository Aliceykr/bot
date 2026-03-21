import sys

chars = set()

# CJK 统一汉字基本区（20902个汉字）
for code in range(0x4E00, 0x9FFF + 1):
    chars.add(chr(code))

# 常用标点和符号（ASCII + 中文标点）
extra = list(range(0x20, 0x7F))  # ASCII 可见字符
extra += [0x3002, 0xFF0C, 0x3001, 0xFF01, 0xFF1F, 0xFF1B, 0xFF1A,  # 。，、！？；：
          0x201C, 0x201D, 0x2018, 0x2019,  # 引号
          0x300A, 0x300B, 0x3010, 0x3011,  # 《》【】
          0x2014, 0x2026, 0x00B0, 0x00B7]  # —…度·
for c in extra:
    chars.add(chr(c))

result = ''.join(sorted(chars))
with open('C:/Users/Aliceykr/Desktop/bot/chars.txt', 'w', encoding='utf-8') as f:
    f.write(result)
print(f'Total chars: {len(result)}')
