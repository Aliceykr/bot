# SPIFFS 镜像源

本目录的内容会在 `idf.py build` 时打包成 SPIFFS 镜像烧入 `storage` 分区。

## 放置 ROM

把你的 `.gb` 或 `.gbc` 文件放到 `roms/` 目录下，例如：

```
spiffs_image/
├── roms/
│   ├── zelda_dx.gbc
│   └── tobu.gb
```

## 注意

- 分区大小约 4MB，可放多个 ROM
- 重新烧录时必须 `idf.py flash`（含 storage），否则 SPIFFS 不会更新
- ROM 版权归原持有者，请自行确保使用合法
