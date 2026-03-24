#!/usr/bin/env python3
"""
speaker_audio.py — 发送 WAV 文件通过 USB CDC 串口给 ESP32 播放

依赖安装：
    pip install pyserial numpy

WAV 格式要求：自动转换为 16kHz、16bit、mono、little-endian

用法示例：
    # 交互式选择文件和串口（推荐）
    python speaker_audio.py

    # 直接指定
    python speaker_audio.py --port COM3 --file audio.wav
"""

import argparse
import os
import signal
import sys
import time
import wave
import struct
from typing import Optional

import numpy as np
import serial
import serial.tools.list_ports

# ================================================================
# 常量配置
# ================================================================
TARGET_RATE = 16000  # ESP32 端采样率（须与 SPK_SAMPLE_RATE 一致）
CHUNK_BYTES = 512    # 每次串口发送字节数

# ================================================================
# 全局退出标志
# ================================================================
_stop = False


def _signal_handler(sig, frame):
    """Ctrl+C 优雅退出。"""
    global _stop
    print("\n[INFO] 收到退出信号，正在停止...", flush=True)
    _stop = True


# ================================================================
# WAV 解析与转换
# ================================================================

def load_wav_as_pcm16(file_path: str) -> bytes:
    """
    读取 WAV 文件，自动转换为 16kHz 16bit mono little-endian PCM。
    支持任意采样率、8/16/24/32bit、mono/stereo 输入。
    """
    with wave.open(file_path, 'rb') as wf:
        n_channels = wf.getnchannels()
        sampwidth  = wf.getsampwidth()   # 字节数：1=8bit, 2=16bit, 3=24bit, 4=32bit
        src_rate   = wf.getframerate()
        n_frames   = wf.getnframes()
        raw        = wf.readframes(n_frames)

    print(f"[INFO] WAV: {src_rate}Hz {sampwidth*8}bit {n_channels}ch {n_frames} frames")

    # 解码为 float32 [-1.0, 1.0]
    if sampwidth == 1:   # 8bit unsigned
        samples = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128) / 128.0
    elif sampwidth == 2: # 16bit signed
        samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    elif sampwidth == 3: # 24bit signed — 手动解包
        arr = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        # 将 3 字节拼成 int32（little-endian，符号扩展）
        i32 = (arr[:, 0].astype(np.int32)
               | (arr[:, 1].astype(np.int32) << 8)
               | (arr[:, 2].astype(np.int32) << 16))
        # 符号扩展
        i32[i32 >= 0x800000] -= 0x1000000
        samples = i32.astype(np.float32) / 8388608.0
    elif sampwidth == 4: # 32bit signed
        samples = np.frombuffer(raw, dtype=np.int32).astype(np.float32) / 2147483648.0
    else:
        raise ValueError(f"不支持的位宽: {sampwidth*8}bit")

    # 多声道 → mono（取均值）
    if n_channels > 1:
        samples = samples.reshape(-1, n_channels).mean(axis=1)

    # 重采样到 TARGET_RATE
    if src_rate != TARGET_RATE:
        from fractions import Fraction
        ratio = Fraction(TARGET_RATE, src_rate).limit_denominator(100)
        up, down = ratio.numerator, ratio.denominator
        # 使用 scipy 如果有，否则用简单线性插值
        try:
            from scipy.signal import resample_poly
            samples = resample_poly(samples, up, down)
        except ImportError:
            # 线性插值 fallback
            old_len = len(samples)
            new_len = int(old_len * TARGET_RATE / src_rate)
            samples = np.interp(
                np.linspace(0, old_len - 1, new_len),
                np.arange(old_len),
                samples
            )
        print(f"[INFO] 重采样: {src_rate}Hz → {TARGET_RATE}Hz")

    # 转换为 int16 little-endian
    samples = np.clip(samples, -1.0, 1.0)
    pcm16 = (samples * 32767).astype(np.int16)
    return pcm16.tobytes()


# ================================================================
# 交互式选择
# ================================================================

def select_port() -> str:
    """交互式选择串口，返回串口名。"""
    ports = list(serial.tools.list_ports.comports())
    print("\n=== 可用串口 ===")
    if ports:
        for i, p in enumerate(ports):
            print(f"  {i}) {p.device}  {p.description}")
    else:
        print("  （未检测到串口，请检查 USB 连接）")

    while True:
        try:
            raw = input("\n请选择串口编号，或直接输入串口名（如 COM3）: ").strip()
            try:
                idx = int(raw)
                if ports and 0 <= idx < len(ports):
                    print(f"[INFO] 已选择串口: {ports[idx].device}")
                    return ports[idx].device
            except ValueError:
                if raw:
                    print(f"[INFO] 已选择串口: {raw}")
                    return raw
        except KeyboardInterrupt:
            sys.exit(0)
        print("  输入无效，请重试")


def select_file() -> str:
    """交互式选择 WAV 文件，返回文件路径。"""
    wav_files = [f for f in os.listdir('.') if f.lower().endswith('.wav')]

    print("\n=== 当前目录 WAV 文件 ===")
    if wav_files:
        for i, f in enumerate(wav_files):
            size = os.path.getsize(f)
            print(f"  {i}) {f}  ({size} bytes)")
    else:
        print("  （当前目录无 .wav 文件）")

    while True:
        try:
            if wav_files:
                raw = input(
                    f"\n请选择文件编号 [0-{len(wav_files)-1}]，或直接输入文件路径: "
                ).strip()
            else:
                raw = input("\n请输入 WAV 文件路径: ").strip()

            try:
                idx = int(raw)
                if wav_files and 0 <= idx < len(wav_files):
                    path = wav_files[idx]
                    print(f"[INFO] 已选择文件: {path}")
                    return path
            except ValueError:
                pass

            if raw and os.path.isfile(raw):
                print(f"[INFO] 已选择文件: {raw}")
                return raw
            elif raw:
                print(f"  文件不存在: {raw}，请重试")

        except KeyboardInterrupt:
            sys.exit(0)


# ================================================================
# 串口管理
# ================================================================

def open_serial(port: str) -> serial.Serial:
    """打开串口并置高 DTR，触发 ESP32 端 s_usb_ready。"""
    ser = serial.Serial(port, baudrate=921600, timeout=1)
    ser.dtr = True
    ser.rts = False
    print(f"[INFO] 串口已打开: {port}")
    return ser


# ================================================================
# 发送主逻辑
# ================================================================

def run_file(port: str, file_path: str) -> None:
    """
    读取 WAV 文件，转换为 16kHz 16bit mono PCM，按实时速率发送给 ESP32。
    """
    global _stop
    _stop = False

    ser: Optional[serial.Serial] = None
    try:
        print("[INFO] 解析 WAV 文件...")
        data = load_wav_as_pcm16(file_path)
        total = len(data)
        duration = total / 2 / TARGET_RATE
        print(f"[INFO] PCM 数据: {total} bytes（{duration:.1f} 秒）")

        ser = open_serial(port)
        print("[INFO] 开始发送，按 Ctrl+C 停止...")

        offset = 0
        while offset < total and not _stop:
            chunk = data[offset: offset + CHUNK_BYTES]
            if not ser.is_open:
                raise serial.SerialException("串口已关闭")
            ser.write(chunk)
            offset += len(chunk)

        if not _stop:
            print("[INFO] 文件发送完毕")

    except Exception as e:
        print(f"[ERROR] 发送异常: {e}")
    finally:
        if ser and ser.is_open:
            ser.dtr = False
            ser.close()
            print("[INFO] 串口已关闭")
        print("[INFO] 已退出")


# ================================================================
# 入口
# ================================================================

def main():
    parser = argparse.ArgumentParser(
        description="发送 WAV 文件通过 USB CDC 串口给 ESP32 MAX98357A 播放"
    )
    parser.add_argument('--port', '-p', default=None, help='串口号，例如 COM3')
    parser.add_argument('--file', '-f', default=None, help='WAV 文件路径')
    args = parser.parse_args()

    signal.signal(signal.SIGINT,  _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    port      = args.port or select_port()
    file_path = args.file or select_file()

    print()
    run_file(port, file_path)


if __name__ == '__main__':
    main()
