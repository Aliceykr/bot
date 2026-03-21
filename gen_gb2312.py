lines = [
    ('title', '静夜思'),
    ('line1', '床前明月光'),
    ('line2', '疑是地上霜'),
    ('line3', '举头望明月'),
    ('line4', '低头思故乡'),
]
for label, s in lines:
    b = s.encode('gb2312')
    hexstr = ''.join('\\x{:02x}'.format(c) for c in b)
    print(label + ': "' + hexstr + '"')
