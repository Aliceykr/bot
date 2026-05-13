import 'dart:async';
import 'package:flutter/material.dart';
import '../ble_service.dart';
import 'wifi_page.dart';
import 'music_page.dart';
import 'terminal_page.dart';

class ControlPage extends StatefulWidget {
  const ControlPage({super.key});

  @override
  State<ControlPage> createState() => _ControlPageState();
}

class _ControlPageState extends State<ControlPage> {
  final _ble = BleService();
  bool _connected = true;
  StreamSubscription? _connSub;

  @override
  void initState() {
    super.initState();
    _connSub = _ble.onConnectionChanged.listen((connected) {
      setState(() => _connected = connected);
      if (!connected && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('设备已断开')),
        );
        Navigator.popUntil(context, (route) => route.isFirst);
      }
    });
  }

  @override
  void dispose() {
    _connSub?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(_ble.deviceName.isNotEmpty ? _ble.deviceName : 'ESP32-Bot'),
        actions: [
          IconButton(
            icon: const Icon(Icons.bluetooth_disabled),
            tooltip: '断开连接',
            onPressed: () async {
              await _ble.disconnect();
              if (mounted) Navigator.pop(context);
            },
          ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          children: [
            // 连接状态
            Container(
              padding: const EdgeInsets.all(16),
              decoration: BoxDecoration(
                color: const Color(0xFF16213E),
                borderRadius: BorderRadius.circular(12),
              ),
              child: Row(
                children: [
                  Icon(
                    Icons.bluetooth_connected,
                    color: _connected ? Colors.greenAccent : Colors.red,
                  ),
                  const SizedBox(width: 12),
                  Text(
                    _connected ? '已连接' : '已断开',
                    style: const TextStyle(color: Colors.white, fontSize: 16),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 32),

            // 功能按钮
            _buildFeatureCard(
              icon: Icons.wifi,
              title: 'WiFi 配网',
              subtitle: '发送 SSID 和密码连接 WiFi',
              onTap: () => Navigator.push(
                context,
                MaterialPageRoute(builder: (_) => const WifiPage()),
              ),
            ),
            const SizedBox(height: 16),
            _buildFeatureCard(
              icon: Icons.music_note,
              title: '音乐控制',
              subtitle: '远程播放/停止 SD 卡音乐',
              onTap: () => Navigator.push(
                context,
                MaterialPageRoute(builder: (_) => const MusicPage()),
              ),
            ),
            const SizedBox(height: 16),
            _buildFeatureCard(
              icon: Icons.terminal,
              title: '终端',
              subtitle: '自由发送命令 / 查看回复',
              onTap: () => Navigator.push(
                context,
                MaterialPageRoute(builder: (_) => const TerminalPage()),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildFeatureCard({
    required IconData icon,
    required String title,
    required String subtitle,
    required VoidCallback onTap,
  }) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(12),
      child: Container(
        padding: const EdgeInsets.all(20),
        decoration: BoxDecoration(
          color: const Color(0xFF16213E),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: const Color(0xFFE94560).withValues(alpha: 0.3)),
        ),
        child: Row(
          children: [
            Icon(icon, color: const Color(0xFFE94560), size: 32),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(title,
                      style: const TextStyle(
                          color: Colors.white,
                          fontSize: 18,
                          fontWeight: FontWeight.bold)),
                  const SizedBox(height: 4),
                  Text(subtitle,
                      style:
                          const TextStyle(color: Colors.white54, fontSize: 13)),
                ],
              ),
            ),
            const Icon(Icons.chevron_right, color: Colors.white38),
          ],
        ),
      ),
    );
  }
}
