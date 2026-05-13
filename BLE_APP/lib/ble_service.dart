import 'dart:async';
import 'dart:convert';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

/// ESP32-Bot BLE 通信服务
/// Service UUID: 0xFFE0, Characteristic UUID: 0xFFE1
class BleService {
  static final BleService _instance = BleService._();
  factory BleService() => _instance;
  BleService._();

  // 16-bit UUID 在 BLE 协议中的完整 128-bit 表示
  static const String serviceUuid16 = "ffe0";
  static const String charUuid16 = "ffe1";

  BluetoothDevice? _device;
  BluetoothCharacteristic? _char;
  StreamSubscription? _notifySub;

  final _notifyController = StreamController<String>.broadcast();
  Stream<String> get onNotify => _notifyController.stream;

  final _connectionController = StreamController<bool>.broadcast();
  Stream<bool> get onConnectionChanged => _connectionController.stream;

  bool get isConnected => _device != null && _device!.isConnected;
  String get deviceName => _device?.platformName ?? '';

  /// 判断 UUID 是否匹配（兼容 16-bit 短格式和 128-bit 长格式）
  bool _uuidMatches(Guid uuid, String short16) {
    final s = uuid.toString().toLowerCase();
    // 完整 128-bit: "0000ffe0-0000-1000-8000-00805f9b34fb"
    // 或短格式: "ffe0" / "0000ffe0"
    return s.contains(short16.toLowerCase());
  }

  /// 连接设备并订阅 Notify
  Future<void> connect(BluetoothDevice device) async {
    _device = device;
    await device.connect(timeout: const Duration(seconds: 10));
    _connectionController.add(true);

    // 监听断连
    device.connectionState.listen((state) {
      if (state == BluetoothConnectionState.disconnected) {
        _connectionController.add(false);
        _cleanup();
      }
    });

    // 发现服务
    final services = await device.discoverServices();
    for (final svc in services) {
      if (_uuidMatches(svc.uuid, serviceUuid16)) {
        for (final c in svc.characteristics) {
          if (_uuidMatches(c.uuid, charUuid16)) {
            _char = c;
            break;
          }
        }
      }
    }

    if (_char == null) {
      // 调试：打印所有发现的服务和特征
      final allUuids = <String>[];
      for (final svc in services) {
        allUuids.add('Svc: ${svc.uuid}');
        for (final c in svc.characteristics) {
          allUuids.add('  Chr: ${c.uuid}');
        }
      }
      throw Exception('未找到 FFE1 特征值\n发现的服务:\n${allUuids.join('\n')}');
    }

    // 订阅 Notify
    await _char!.setNotifyValue(true);
    _notifySub = _char!.onValueReceived.listen((value) {
      final text = utf8.decode(value, allowMalformed: true);
      _notifyController.add(text);
    });
  }

  /// 发送文本命令
  Future<void> send(String text) async {
    if (_char == null) return;
    final bytes = utf8.encode(text);
    // BLE MTU 通常 20 字节，flutter_blue_plus 会自动分包
    await _char!.write(bytes, withoutResponse: false);
  }

  /// 断开连接
  Future<void> disconnect() async {
    await _device?.disconnect();
    _cleanup();
  }

  void _cleanup() {
    _notifySub?.cancel();
    _notifySub = null;
    _char = null;
  }

  void dispose() {
    _notifyController.close();
    _connectionController.close();
  }
}
