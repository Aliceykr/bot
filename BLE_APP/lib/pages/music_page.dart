import 'dart:async';
import 'package:flutter/material.dart';
import '../ble_service.dart';

class MusicPage extends StatefulWidget {
  const MusicPage({super.key});

  @override
  State<MusicPage> createState() => _MusicPageState();
}

class _MusicPageState extends State<MusicPage> {
  final _ble = BleService();
  final List<String> _songs = [];
  String _status = '';
  bool _loading = false;
  StreamSubscription? _notifySub;

  @override
  void initState() {
    super.initState();
    _notifySub = _ble.onNotify.listen(_handleNotify);
  }

  void _handleNotify(String msg) {
    setState(() {
      // 设备回复的歌曲列表格式: "1. xxx.mp3\n2. yyy.wav\n..."
      // 或单行状态消息: "已停止" / "播放: xxx"
      if (msg.contains('. ') && RegExp(r'^\d+\.').hasMatch(msg.trim())) {
        // 歌曲列表行
        for (final line in msg.split('\n')) {
          final trimmed = line.trim();
          if (trimmed.isNotEmpty && RegExp(r'^\d+\.').hasMatch(trimmed)) {
            _songs.add(trimmed);
          }
        }
        _loading = false;
      } else {
        _status = msg.trim();
        if (_status == '已停止' || _status.startsWith('播放')) {
          _loading = false;
        }
      }
    });
  }

  Future<void> _fetchSongList() async {
    setState(() {
      _songs.clear();
      _status = '获取歌曲列表...';
      _loading = true;
    });
    await _ble.send('/music on');
  }

  Future<void> _playSong(int index) async {
    setState(() => _status = '正在播放 #${index + 1}...');
    await _ble.send('/${index + 1}');
  }

  Future<void> _stopMusic() async {
    setState(() => _status = '停止中...');
    await _ble.send('/music off');
  }

  @override
  void dispose() {
    _notifySub?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('音乐控制'),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: '刷新列表',
            onPressed: _loading ? null : _fetchSongList,
          ),
          IconButton(
            icon: const Icon(Icons.stop_circle_outlined),
            tooltip: '停止播放',
            onPressed: _stopMusic,
          ),
        ],
      ),
      body: Column(
        children: [
          // 状态栏
          Container(
            width: double.infinity,
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
            color: const Color(0xFF16213E),
            child: Row(
              children: [
                if (_loading)
                  const SizedBox(
                    width: 16,
                    height: 16,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  ),
                if (_loading) const SizedBox(width: 12),
                Expanded(
                  child: Text(
                    _status.isEmpty ? '点击刷新获取歌曲列表' : _status,
                    style: const TextStyle(color: Colors.white70, fontSize: 14),
                  ),
                ),
              ],
            ),
          ),

          // 歌曲列表
          Expanded(
            child: _songs.isEmpty
                ? Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        const Icon(Icons.library_music,
                            size: 64, color: Colors.white24),
                        const SizedBox(height: 16),
                        const Text('暂无歌曲',
                            style: TextStyle(color: Colors.white38)),
                        const SizedBox(height: 16),
                        ElevatedButton.icon(
                          onPressed: _loading ? null : _fetchSongList,
                          icon: const Icon(Icons.download),
                          label: const Text('获取列表'),
                          style: ElevatedButton.styleFrom(
                            backgroundColor: const Color(0xFFE94560),
                            foregroundColor: Colors.white,
                          ),
                        ),
                      ],
                    ),
                  )
                : ListView.builder(
                    itemCount: _songs.length,
                    itemBuilder: (context, index) {
                      return ListTile(
                        leading: CircleAvatar(
                          backgroundColor: const Color(0xFFE94560),
                          child: Text('${index + 1}',
                              style: const TextStyle(color: Colors.white)),
                        ),
                        title: Text(
                          _songs[index].replaceFirst(RegExp(r'^\d+\.\s*'), ''),
                          style: const TextStyle(color: Colors.white),
                        ),
                        trailing: IconButton(
                          icon: const Icon(Icons.play_arrow,
                              color: Colors.greenAccent),
                          onPressed: () => _playSong(index),
                        ),
                        onTap: () => _playSong(index),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}
