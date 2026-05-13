import 'dart:async';
import 'package:flutter/material.dart';
import '../ble_service.dart';

class TerminalPage extends StatefulWidget {
  const TerminalPage({super.key});

  @override
  State<TerminalPage> createState() => _TerminalPageState();
}

class _TerminalPageState extends State<TerminalPage> {
  final _ble = BleService();
  final _inputController = TextEditingController();
  final _scrollController = ScrollController();
  final List<_LogEntry> _log = [];
  StreamSubscription? _notifySub;

  @override
  void initState() {
    super.initState();
    _notifySub = _ble.onNotify.listen((msg) {
      setState(() {
        _log.add(_LogEntry(text: msg, isRx: true));
      });
      _scrollToBottom();
    });
  }

  void _scrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scrollController.hasClients) {
        _scrollController.animateTo(
          _scrollController.position.maxScrollExtent,
          duration: const Duration(milliseconds: 200),
          curve: Curves.easeOut,
        );
      }
    });
  }

  Future<void> _send() async {
    final text = _inputController.text.trim();
    if (text.isEmpty) return;

    setState(() {
      _log.add(_LogEntry(text: text, isRx: false));
    });
    _inputController.clear();
    _scrollToBottom();

    try {
      await _ble.send(text);
    } catch (e) {
      setState(() {
        _log.add(_LogEntry(text: '[发送失败: $e]', isRx: true));
      });
    }
  }

  @override
  void dispose() {
    _notifySub?.cancel();
    _inputController.dispose();
    _scrollController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('BLE 终端'),
        actions: [
          IconButton(
            icon: const Icon(Icons.delete_outline),
            tooltip: '清空',
            onPressed: () => setState(() => _log.clear()),
          ),
        ],
      ),
      body: Column(
        children: [
          // 日志区域
          Expanded(
            child: Container(
              color: const Color(0xFF0A1628),
              child: ListView.builder(
                controller: _scrollController,
                padding: const EdgeInsets.all(12),
                itemCount: _log.length,
                itemBuilder: (context, index) {
                  final entry = _log[index];
                  return Padding(
                    padding: const EdgeInsets.symmetric(vertical: 2),
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          entry.isRx ? '← ' : '→ ',
                          style: TextStyle(
                            color: entry.isRx
                                ? Colors.greenAccent
                                : Colors.orangeAccent,
                            fontFamily: 'monospace',
                            fontSize: 13,
                          ),
                        ),
                        Expanded(
                          child: Text(
                            entry.text,
                            style: TextStyle(
                              color: entry.isRx
                                  ? Colors.greenAccent
                                  : Colors.orangeAccent,
                              fontFamily: 'monospace',
                              fontSize: 13,
                            ),
                          ),
                        ),
                      ],
                    ),
                  );
                },
              ),
            ),
          ),

          // 输入区域
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            color: const Color(0xFF16213E),
            child: Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _inputController,
                    style: const TextStyle(color: Colors.white, fontSize: 14),
                    decoration: const InputDecoration(
                      hintText: '输入命令...',
                      hintStyle: TextStyle(color: Colors.white38),
                      border: InputBorder.none,
                      contentPadding:
                          EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                    ),
                    onSubmitted: (_) => _send(),
                  ),
                ),
                IconButton(
                  icon: const Icon(Icons.send, color: Color(0xFFE94560)),
                  onPressed: _send,
                ),
              ],
            ),
          ),

          // 快捷命令
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
            color: const Color(0xFF0F3460),
            child: SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: Row(
                children: [
                  _quickBtn('/music on'),
                  _quickBtn('/music off'),
                  _quickBtn('/1'),
                  _quickBtn('/2'),
                  _quickBtn('/3'),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _quickBtn(String cmd) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 4),
      child: ActionChip(
        label: Text(cmd, style: const TextStyle(fontSize: 12)),
        backgroundColor: const Color(0xFF16213E),
        labelStyle: const TextStyle(color: Colors.white70),
        side: const BorderSide(color: Color(0xFFE94560), width: 0.5),
        onPressed: () async {
          setState(() => _log.add(_LogEntry(text: cmd, isRx: false)));
          _scrollToBottom();
          await _ble.send(cmd);
        },
      ),
    );
  }
}

class _LogEntry {
  final String text;
  final bool isRx;
  _LogEntry({required this.text, required this.isRx});
}
