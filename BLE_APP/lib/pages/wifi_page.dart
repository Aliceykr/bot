import 'dart:async';
import 'package:flutter/material.dart';
import '../ble_service.dart';

class WifiPage extends StatefulWidget {
  const WifiPage({super.key});

  @override
  State<WifiPage> createState() => _WifiPageState();
}

class _WifiPageState extends State<WifiPage> {
  final _ssidController = TextEditingController();
  final _pwdController = TextEditingController();
  final _ble = BleService();
  String _status = '';
  bool _sending = false;
  StreamSubscription? _notifySub;

  @override
  void initState() {
    super.initState();
    _notifySub = _ble.onNotify.listen((msg) {
      setState(() => _status += '$msg\n');
    });
  }

  @override
  void dispose() {
    _notifySub?.cancel();
    _ssidController.dispose();
    _pwdController.dispose();
    super.dispose();
  }

  Future<void> _sendWifiCredentials() async {
    final ssid = _ssidController.text.trim();
    final pwd = _pwdController.text.trim();
    if (ssid.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('请输入 SSID')),
      );
      return;
    }

    setState(() {
      _sending = true;
      _status = '发送配网信息...\n';
    });

    try {
      // 协议格式: "SSID_名称 password_密码"
      final cmd = 'SSID_$ssid password_$pwd';
      await _ble.send(cmd);
      setState(() => _status += '已发送，等待设备回复...\n');
    } catch (e) {
      setState(() => _status += '发送失败: $e\n');
    } finally {
      setState(() => _sending = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('WiFi 配网')),
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            TextField(
              controller: _ssidController,
              style: const TextStyle(color: Colors.white),
              decoration: const InputDecoration(
                labelText: 'WiFi 名称 (SSID)',
                labelStyle: TextStyle(color: Colors.white54),
                prefixIcon: Icon(Icons.wifi, color: Colors.white38),
                enabledBorder: OutlineInputBorder(
                  borderSide: BorderSide(color: Colors.white24),
                ),
                focusedBorder: OutlineInputBorder(
                  borderSide: BorderSide(color: Color(0xFFE94560)),
                ),
              ),
            ),
            const SizedBox(height: 16),
            TextField(
              controller: _pwdController,
              obscureText: true,
              style: const TextStyle(color: Colors.white),
              decoration: const InputDecoration(
                labelText: '密码',
                labelStyle: TextStyle(color: Colors.white54),
                prefixIcon: Icon(Icons.lock, color: Colors.white38),
                enabledBorder: OutlineInputBorder(
                  borderSide: BorderSide(color: Colors.white24),
                ),
                focusedBorder: OutlineInputBorder(
                  borderSide: BorderSide(color: Color(0xFFE94560)),
                ),
              ),
            ),
            const SizedBox(height: 24),
            ElevatedButton.icon(
              onPressed: _sending ? null : _sendWifiCredentials,
              icon: _sending
                  ? const SizedBox(
                      width: 16,
                      height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.send),
              label: const Text('发送配网'),
              style: ElevatedButton.styleFrom(
                backgroundColor: const Color(0xFFE94560),
                foregroundColor: Colors.white,
                padding: const EdgeInsets.symmetric(vertical: 14),
              ),
            ),
            const SizedBox(height: 24),
            const Text('设备回复:',
                style: TextStyle(color: Colors.white54, fontSize: 13)),
            const SizedBox(height: 8),
            Expanded(
              child: Container(
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: const Color(0xFF0A1628),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: SingleChildScrollView(
                  child: Text(
                    _status.isEmpty ? '等待...' : _status,
                    style: const TextStyle(
                        color: Colors.greenAccent,
                        fontFamily: 'monospace',
                        fontSize: 13),
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
