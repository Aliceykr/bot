#!/usr/bin/env python3
"""
recv_audio.py — 从 ESP32-S3 USB CDC 接收 PCM 音频并保存为 WAV

用法:
    python recv_audio.py [PORT] [SECONDS] [OUTPUT]
    python recv_audio.py          # 不带参数则交互式选择 COM 口

依赖:
    pip install pyserial
"""

import sys
import wave
import time
import serial
import serial.tools.list_ports

SAMPLE_RATE  = 16000
CHANNELS     = 1
SAMPLE_WIDTH = 2


def select_port():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("[!] 未检测到任何串口设备")
        sys.exit(1)
    print("检测到以下串口：")
    for i, p in enumerate(ports):
        print(f"  [{i}] {p.device}  —  {p.description}")
    while True:
        try:
            idx = int(input("请输入编号选择串口: "))
            if 0 <= idx < len(ports):
                return ports[idx].device
        except (ValueError, KeyboardInterrupt):
            pass
        print("    输入无效，请重试")


def main():
    if len(sys.argv) > 1:
        PORT = sys.argv[1]
    else:
        PORT = select_port()

    SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    OUTPUT  = sys.argv[3] if len(sys.argv) > 3 else "output.wav"

    print(f"[*] 打开串口 {PORT} ...")
    try:
        # 设置足够大的读缓冲区，避免系统层丢包
        ser = serial.Serial(PORT, baudrate=115200, timeout=0.1)
        ser.set_buffer_size(rx_size=65536)
    except serial.SerialException as e:
        print(f"[!] 无法打开串口: {e}")
        sys.exit(1)

    ser.dtr = True
    time.sleep(0.5)
    ser.reset_input_buffer()

    print(f"[*] 按时间录制 {SECONDS} 秒，随时按 Ctrl+C 提前停止...")
    buf = bytearray()
    start = time.time()

    try:
        while True:
            elapsed = time.time() - start
            if elapsed >= SECONDS:
                break
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
            received_sec = len(buf) / (SAMPLE_RATE * SAMPLE_WIDTH)
            print(f"\r    已接收: {len(buf):7d} bytes  ({received_sec:.1f}s / {SECONDS}s)",
                  end="", flush=True)
    except KeyboardInterrupt:
        print("\n[*] 用户中断")

    ser.dtr = False
    ser.close()
    print(f"\n[*] 接收完成，共 {len(buf)} bytes")

    if len(buf) < SAMPLE_RATE * SAMPLE_WIDTH:
        print("[!] 数据太少，放弃保存")
        sys.exit(1)

    # 对齐到偶数字节
    if len(buf) % 2 != 0:
        buf = buf[:-1]

    with wave.open(OUTPUT, 'wb') as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(bytes(buf))

    print(f"[+] 已保存: {OUTPUT}")
    print(f"    格式: {SAMPLE_RATE} Hz / {SAMPLE_WIDTH*8}-bit / {'Mono' if CHANNELS==1 else 'Stereo'}")
    print(f"    时长: {len(buf) / (SAMPLE_RATE * SAMPLE_WIDTH):.2f} 秒")
    print("[*] 用 Audacity 打开 WAV 文件验证")


if __name__ == "__main__":
    main()
