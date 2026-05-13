import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import '../ble_service.dart';
import 'control_page.dart';

class ScanPage extends StatefulWidget {
  const ScanPage({super.key});

  @override
  State<ScanPage> createState() => _ScanPageState();
}

class _ScanPageState extends State<ScanPage> {
  final List<ScanResult> _results = [];
  bool _scanning = false;
  StreamSubscription? _scanSub;

  @override
  void initState() {
    super.initState();
    _requestPermissions();
  }

  Future<void> _requestPermissions() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.location,
    ].request();
  }

  Future<void> _startScan() async {
    setState(() {
      _results.clear();
      _scanning = true;
    });

    _scanSub = FlutterBluePlus.scanResults.listen((results) {
      setState(() {
        _results.clear();
        for (final r in results) {
          // 只显示有名字的设备
          if (r.device.platformName.isNotEmpty) {
            _results.add(r);
          }
        }
      });
    });

    await FlutterBluePlus.startScan(timeout: const Duration(seconds: 8));
    setState(() => _scanning = false);
  }

  Future<void> _connectDevice(BluetoothDevice device) async {
    // 停止扫描
    await FlutterBluePlus.stopScan();
    _scanSub?.cancel();

    // 显示连接中
    if (!mounted) return;
    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (_) => const AlertDialog(
        content: Row(
          children: [
            CircularProgressIndicator(),
            SizedBox(width: 16),
            Text('连接中...'),
          ],
        ),
      ),
    );

    try {
      await BleService().connect(device);
      if (!mounted) return;
      Navigator.pop(context); // 关闭 loading
      Navigator.push(
        context,
        MaterialPageRoute(builder: (_) => const ControlPage()),
      );
    } catch (e) {
      if (!mounted) return;
      Navigator.pop(context); // 关闭 loading
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('连接失败: $e')),
      );
    }
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('ESP32-Bot BLE'),
        actions: [
          if (_scanning)
            const Padding(
              padding: EdgeInsets.all(16),
              child: SizedBox(
                width: 20,
                height: 20,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
            ),
        ],
      ),
      body: _results.isEmpty
          ? Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(Icons.bluetooth_searching,
                      size: 80, color: Colors.white24),
                  const SizedBox(height: 16),
                  Text(
                    _scanning ? '扫描中...' : '点击下方按钮开始扫描',
                    style: const TextStyle(color: Colors.white54),
                  ),
                ],
              ),
            )
          : ListView.builder(
              itemCount: _results.length,
              itemBuilder: (context, index) {
                final r = _results[index];
                final isEsp = r.device.platformName.contains('ESP32');
                return ListTile(
                  leading: Icon(
                    Icons.bluetooth,
                    color: isEsp ? const Color(0xFFE94560) : Colors.white38,
                  ),
                  title: Text(
                    r.device.platformName,
                    style: TextStyle(
                      color: Colors.white,
                      fontWeight: isEsp ? FontWeight.bold : FontWeight.normal,
                    ),
                  ),
                  subtitle: Text(
                    '${r.device.remoteId}  RSSI: ${r.rssi}',
                    style: const TextStyle(color: Colors.white38, fontSize: 12),
                  ),
                  trailing: isEsp
                      ? const Icon(Icons.star, color: Color(0xFFE94560))
                      : null,
                  onTap: () => _connectDevice(r.device),
                );
              },
            ),
      floatingActionButton: FloatingActionButton(
        onPressed: _scanning ? null : _startScan,
        backgroundColor: const Color(0xFFE94560),
        child: Icon(_scanning ? Icons.hourglass_top : Icons.search),
      ),
    );
  }
}
