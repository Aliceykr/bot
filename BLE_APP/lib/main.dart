import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'pages/scan_page.dart';

void main() {
  FlutterBluePlus.setLogLevel(LogLevel.warning);
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ESP32-Bot',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.dark(
          primary: const Color(0xFFE94560),
          secondary: const Color(0xFF1E90FF),
          surface: const Color(0xFF16213E),
        ),
        scaffoldBackgroundColor: const Color(0xFF0F3460),
        appBarTheme: const AppBarTheme(
          backgroundColor: Color(0xFF16213E),
          foregroundColor: Colors.white,
          elevation: 0,
        ),
        useMaterial3: true,
      ),
      home: const ScanPage(),
    );
  }
}
