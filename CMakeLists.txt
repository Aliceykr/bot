# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_SOURCE_DIR}/lvgl
    ${CMAKE_SOURCE_DIR}/game
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 告诉 GCC 源码文件是 UTF-8 编码（Windows 上 GCC 默认按系统代码页 GBK
# 解析，会把 UTF-8 中文注释里的高字节当成 stray 字符报错 Werror=comment）。
# 必须在 project() 之前调用，否则只对项目最后一个 add_executable 生效。
# -finput-charset=UTF-8  输入源文件字符集
# -fexec-charset=UTF-8   字符串字面量在目标机器上的字符集（保持 UTF-8）
idf_build_set_property(COMPILE_OPTIONS
    "-finput-charset=UTF-8;-fexec-charset=UTF-8"
    APPEND)

project(bot)

# ROM 和音乐文件从 SD 卡读取，不再打包 SPIFFS 镜像。
# 原来的 spiffs_image/ 目录只作为参考样例保留，不再参与构建。
