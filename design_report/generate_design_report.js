const fs = require("fs");
const path = require("path");
const sharp = require("/tmp/codex-docx-node/node_modules/sharp");
const {
  AlignmentType,
  BorderStyle,
  Document,
  Footer,
  Header,
  HeadingLevel,
  ImageRun,
  LeaderType,
  LevelFormat,
  Packer,
  PageBreak,
  PageNumber,
  Paragraph,
  ShadingType,
  Table,
  TableCell,
  TableRow,
  Tab,
  TabStopType,
  TextRun,
  VerticalAlign,
  WidthType,
} = require("/tmp/codex-docx-node/node_modules/docx");

const ROOT = path.resolve(__dirname, "..");
const OUT_DIR = path.join(ROOT, "design_report");
const SCH_PATH = path.join(ROOT, "SCH_Schematic1_1-P1_2026-05-22.png");
const FLOW_SVG = path.join(OUT_DIR, "software_flow.svg");
const FLOW_PNG = path.join(OUT_DIR, "software_flow.png");
const DOCX_OUT = path.join(OUT_DIR, "ESP32-S3_AI智能语音助手与游戏系统设计报告.docx");

const PAGE_W = 11906; // A4
const PAGE_H = 16838;
const MARGIN = { top: 1134, right: 1134, bottom: 1134, left: 1134 };
const CONTENT_W = PAGE_W - MARGIN.left - MARGIN.right;
const FONT = "SimSun";
const FONT_LATIN = "Arial";
const COMPACT = true;

function escapeXml(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function run(text, opts = {}) {
  return new TextRun({
    text,
    font: opts.font || FONT,
    size: opts.size || 22,
    bold: opts.bold || false,
    color: opts.color,
    italics: opts.italics || false,
    break: opts.break,
  });
}

function para(text, opts = {}) {
  const children = Array.isArray(text) ? text : [run(text, opts.run || {})];
  return new Paragraph({
    children,
    alignment: opts.alignment,
    spacing: {
      before: opts.before === undefined ? 0 : opts.before,
      after: opts.after === undefined ? 80 : opts.after,
      line: opts.line || 320,
    },
    indent: opts.indent,
    heading: opts.heading,
    numbering: opts.numbering,
    keepNext: opts.keepNext,
    keepLines: opts.keepLines,
    tabStops: opts.tabStops,
  });
}

function titlePara(text, size = 34) {
  return new Paragraph({
    children: [run(text, { bold: true, size })],
    alignment: AlignmentType.CENTER,
    spacing: { before: 80, after: 160, line: 380 },
    keepLines: true,
  });
}

function h1(text) {
  return new Paragraph({
    text,
    heading: HeadingLevel.HEADING_1,
    spacing: { before: 160, after: 100 },
    keepNext: true,
  });
}

function h2(text) {
  return new Paragraph({
    text,
    heading: HeadingLevel.HEADING_2,
    spacing: { before: 120, after: 70 },
    keepNext: true,
  });
}

function h3(text) {
  return new Paragraph({
    text,
    heading: HeadingLevel.HEADING_3,
    spacing: { before: 100, after: 60 },
    keepNext: true,
  });
}

function bullet(text, level = 0) {
  return para(text, {
    numbering: { reference: "bullets", level },
    after: 45,
  });
}

function num(text, level = 0) {
  return para(text, {
    numbering: { reference: "numbers", level },
    after: 45,
  });
}

const thinBorder = {
  style: BorderStyle.SINGLE,
  size: 1,
  color: "BFBFBF",
};
const cellBorders = {
  top: thinBorder,
  bottom: thinBorder,
  left: thinBorder,
  right: thinBorder,
};

function tableCell(text, width, opts = {}) {
  const lines = Array.isArray(text) ? text : [text];
  return new TableCell({
    borders: cellBorders,
    width: { size: width, type: WidthType.DXA },
    verticalAlign: VerticalAlign.CENTER,
    shading: opts.header ? { fill: "D9EAF7", type: ShadingType.CLEAR } : undefined,
    margins: { top: 60, bottom: 60, left: 100, right: 100 },
    children: lines.map((line) => para(line, {
      after: 0,
      line: 270,
      alignment: opts.center ? AlignmentType.CENTER : undefined,
      keepLines: true,
      run: { bold: opts.header || false, size: opts.size || 18 },
    })),
  });
}

function makeTable(headers, rows, widths) {
  return new Table({
    width: { size: CONTENT_W, type: WidthType.DXA },
    columnWidths: widths,
    rows: [
      new TableRow({
        tableHeader: true,
        cantSplit: true,
        children: headers.map((h, i) => tableCell(h, widths[i], { header: true, center: true })),
      }),
      ...rows.map((row) =>
        new TableRow({
          cantSplit: true,
          children: row.map((c, i) => tableCell(c, widths[i])),
        })
      ),
    ],
  });
}

function caption(text) {
  return new Paragraph({
    children: [run(text, { size: 18, italics: true, color: "666666" })],
    alignment: AlignmentType.CENTER,
    spacing: { before: 50, after: 90, line: 260 },
    keepLines: true,
  });
}

function pageBreak() {
  return COMPACT ? new Paragraph({ children: [], spacing: { before: 0, after: 0 } }) : new Paragraph({ children: [new PageBreak()] });
}

function hardPageBreak() {
  return new Paragraph({ children: [new PageBreak()] });
}

function tocLine(title, pageHint, level = 0) {
  return new Paragraph({
    children: [
      run(title, { size: level === 0 ? 22 : 20, bold: level === 0 }),
      new TextRun({ children: [new Tab()], font: FONT, size: level === 0 ? 22 : 20 }),
      run(pageHint, { size: level === 0 ? 22 : 20 }),
    ],
    spacing: { before: 0, after: level === 0 ? 60 : 30, line: 280 },
    indent: { left: level * 360 },
    tabStops: [
      {
        type: TabStopType.RIGHT,
        position: CONTENT_W,
        leader: LeaderType.DOT,
      },
    ],
  });
}

function visibleToc() {
  return [
    h1("目录"),
    tocLine("1 项目概述", "1"),
    tocLine("1.1 设计目标", "1", 1),
    tocLine("1.2 系统功能", "1", 1),
    tocLine("2 方案论证", "3"),
    tocLine("2.1 主控平台方案", "3", 1),
    tocLine("2.2 显示与交互方案", "4", 1),
    tocLine("2.3 语音方案", "4", 1),
    tocLine("2.4 存储与音频方案", "4", 1),
    tocLine("2.5 资源管理方案", "4", 1),
    tocLine("3 硬件电路设计", "5"),
    tocLine("3.1 原理图", "5", 1),
    tocLine("3.2 电源与主控", "5", 1),
    tocLine("3.3 LCD 显示电路", "6", 1),
    tocLine("3.4 音频输入输出电路", "6", 1),
    tocLine("3.5 输入、传感器与存储电路", "6", 1),
    tocLine("4 系统工作原理", "7"),
    tocLine("5 软件流程设计", "8"),
    tocLine("6 关键模块设计说明", "10"),
    tocLine("7 测试与验证建议", "11"),
    tocLine("8 总结", "12"),
    para("注：目录页码为报告生成时的版式参考；如在 Word 中调整字体、纸张或图片大小，可使用 Word 的目录更新功能重新生成精确页码。", {
      after: 80,
      line: 280,
      run: { size: 18, color: "666666" },
    }),
  ];
}

function card(x, y, w, h, title, body, fill = "#F6FBFF", stroke = "#2B6CB0") {
  const titleY = y + 28;
  const bodyY = y + 64;
  const bodyLines = body.split("\n");
  return `
    <rect x="${x}" y="${y}" width="${w}" height="${h}" rx="10" fill="${fill}" stroke="${stroke}" stroke-width="2"/>
    <text x="${x + w / 2}" y="${titleY}" font-size="20" font-weight="700" text-anchor="middle" fill="#13324D">${escapeXml(title)}</text>
    ${bodyLines.map((line, i) => `<text x="${x + 16}" y="${bodyY + i * 22}" font-size="15" fill="#233443">${escapeXml(line)}</text>`).join("")}
  `;
}

function arrow(x1, y1, x2, y2, label = "") {
  const midX = (x1 + x2) / 2;
  const midY = (y1 + y2) / 2;
  return `
    <line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="#365B7B" stroke-width="2.2" marker-end="url(#arrow)"/>
    ${label ? `<text x="${midX}" y="${midY - 8}" font-size="14" text-anchor="middle" fill="#365B7B">${escapeXml(label)}</text>` : ""}
  `;
}

function createFlowSvg() {
  const svg = `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="1600" height="1050" viewBox="0 0 1600 1050">
  <defs>
    <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse">
      <path d="M 0 0 L 10 5 L 0 10 z" fill="#365B7B"/>
    </marker>
    <style>
      text { font-family: "Noto Sans CJK SC", "Microsoft YaHei", Arial, sans-serif; }
    </style>
  </defs>
  <rect width="1600" height="1050" fill="#FFFFFF"/>
  <text x="800" y="54" font-size="30" font-weight="700" text-anchor="middle" fill="#14324A">软件总体流程图</text>
  ${card(650, 90, 300, 92, "系统上电 / app_main", "NVS、PSRAM 管理初始化\n外设和业务模块初始化", "#FFF8E6", "#C27B16")}
  ${card(650, 220, 300, 100, "硬件与框架启动", "LCD / I2S / SD / 键盘\nLVGL 显示与输入注册", "#F6FBFF", "#2B6CB0")}
  ${card(650, 360, 300, 96, "主菜单循环", "lv_timer_handler 周期调度\n编码器选择功能并确认", "#F0FFF4", "#2F855A")}
  ${arrow(800, 182, 800, 220)}
  ${arrow(800, 320, 800, 360)}

  ${card(70, 520, 250, 120, "环境监测", "创建 env_read 任务\nDHT11 1s 采样\n温湿度卡片刷新", "#F7FAFC", "#718096")}
  ${card(350, 520, 250, 120, "天气与时间", "WiFi 检查/连接\nHTTPS 获取天气 JSON\nSNTP 实时时钟显示", "#F7FAFC", "#3182CE")}
  ${card(630, 520, 250, 120, "聊天助手", "屏幕键盘输入\nOpenAI 兼容接口请求\n滚动对话记录更新", "#F7FAFC", "#805AD5")}
  ${card(910, 520, 250, 120, "在线语音助手", "INMP441 录音\n百度 ASR -> LLM\n百度 TTS -> 扬声器", "#F7FAFC", "#D53F8C")}
  ${card(1190, 520, 250, 120, "离线语音命令", "释放在线 ASR I2S\nESP-SR AFE + MultiNet\n识别后分发菜单动作", "#F7FAFC", "#DD6B20")}

  ${card(70, 760, 250, 120, "游戏", "暂停 WiFi/BLE 与 LVGL flush\n2048 或 GB ROM 运行\n退出后恢复菜单", "#F7FAFC", "#2C5282")}
  ${card(350, 760, 250, 120, "音乐", "扫描 /sdcard/music\nWAV/MP3 解码\n动态采样率 I2S 输出", "#F7FAFC", "#2F855A")}
  ${card(630, 760, 250, 120, "BLE 配网/控制", "NimBLE 广播\nSSID/password 配网\n音乐与音色远程命令", "#F7FAFC", "#319795")}
  ${card(910, 760, 250, 120, "智能设备", "拉取巴法云设备列表\n发送 on/off\n语音控制关键词匹配", "#F7FAFC", "#B7791F")}
  ${card(1190, 760, 250, 120, "音量与系统监控", "滑块调节音量并写 NVS\n健康任务周期打印堆水位", "#F7FAFC", "#4A5568")}

  ${arrow(690, 456, 195, 520)}
  ${arrow(740, 456, 475, 520)}
  ${arrow(790, 456, 755, 520)}
  ${arrow(840, 456, 1035, 520)}
  ${arrow(890, 456, 1315, 520)}
  ${arrow(690, 456, 195, 760)}
  ${arrow(740, 456, 475, 760)}
  ${arrow(800, 456, 755, 760)}
  ${arrow(860, 456, 1035, 760)}
  ${arrow(910, 456, 1315, 760)}

  <path d="M 195 640 C 195 704, 600 700, 720 476" fill="none" stroke="#A0AEC0" stroke-width="1.8" stroke-dasharray="8 7" marker-end="url(#arrow)"/>
  <path d="M 755 640 C 780 700, 825 700, 860 476" fill="none" stroke="#A0AEC0" stroke-width="1.8" stroke-dasharray="8 7" marker-end="url(#arrow)"/>
  <path d="M 1035 880 C 1030 950, 770 960, 790 456" fill="none" stroke="#A0AEC0" stroke-width="1.8" stroke-dasharray="8 7" marker-end="url(#arrow)"/>
  <text x="800" y="1008" font-size="18" text-anchor="middle" fill="#666666">后台任务通过 FreeRTOS Queue / lv_timer 回传结果，所有 LVGL 对象更新集中在 LVGL 线程执行</text>
</svg>`;
  fs.writeFileSync(FLOW_SVG, svg);
}

async function createFlowPng() {
  await sharp(FLOW_SVG).png().toFile(FLOW_PNG);
}

function imageParagraph(imagePath, widthPx, heightPx, altText) {
  const data = fs.readFileSync(imagePath);
  return new Paragraph({
    children: [
      new ImageRun({
        type: "png",
        data,
        transformation: { width: widthPx, height: heightPx },
        altText: { title: altText, description: altText, name: altText },
      }),
    ],
    alignment: AlignmentType.CENTER,
    spacing: { before: 40, after: 50 },
    keepLines: true,
  });
}

function svgImageParagraph(svgPath, pngPath, widthPx, heightPx, altText) {
  return new Paragraph({
    children: [
      new ImageRun({
        type: "svg",
        data: fs.readFileSync(svgPath),
        fallback: {
          type: "png",
          data: fs.readFileSync(pngPath),
        },
        transformation: { width: widthPx, height: heightPx },
        altText: { title: altText, description: altText, name: altText },
      }),
    ],
    alignment: AlignmentType.CENTER,
    spacing: { before: 40, after: 50 },
    keepLines: true,
  });
}

function buildDoc() {
  const children = [];

  children.push(
    new Paragraph({ spacing: { before: 1200, after: 160 }, children: [] }),
    titlePara("ESP32-S3 AI 智能语音助手与游戏系统", 40),
    titlePara("设计报告", 38),
    new Paragraph({ spacing: { before: 420, after: 160 }, children: [] }),
    para("项目名称：Bot 嵌入式智能终端", { alignment: AlignmentType.CENTER, after: 80, run: { size: 24 } }),
    para("开发平台：ESP-IDF + LVGL + ESP-SR", { alignment: AlignmentType.CENTER, after: 80, run: { size: 24 } }),
    para("主控芯片：ESP32-S3-DevKitC-1 N16R8", { alignment: AlignmentType.CENTER, after: 80, run: { size: 24 } }),
    para("报告日期：2026 年 5 月 22 日", { alignment: AlignmentType.CENTER, after: 80, run: { size: 24 } }),
    hardPageBreak(),
    ...visibleToc(),
    hardPageBreak(),
  );

  children.push(
    h1("1 项目概述"),
    para("本工程是一个基于 ESP32-S3 的多功能嵌入式智能终端，系统在 2.4 英寸 ILI9341 TFT LCD 上提供 LVGL 图形菜单，使用旋转编码器和 3x3 矩阵键盘进行交互，并集成 WiFi、BLE、在线语音识别、语音合成、离线中文命令词识别、天气查询、LLM 聊天、DHT11 环境监测、SD 卡音乐播放、Game Boy 模拟器、2048 游戏和巴法云智能设备控制等功能。"),
    para("系统设计重点不是单一外设驱动，而是多个高占用资源模块在 ESP32-S3 有限内部 SRAM 下的协同运行。工程通过 PSRAM 任务栈、HTTP 响应缓冲动态分配、WiFi/BLE/ESP-SR 懒加载与资源释放、LCD 与 SD 卡 SPI 总线隔离、I2S 输入输出分离等方法，保证语音、图形、网络、音频和游戏任务能够稳定共存。"),
    h2("1.1 设计目标"),
    bullet("实现一个具备图形化菜单、人机交互、联网智能问答、语音交互、环境采集、音乐播放和游戏娱乐能力的 ESP32-S3 终端。"),
    bullet("说明硬件连接关系和各功能模块工作原理，能够从电路图看出信号流、总线分配和供电方式。"),
    bullet("说明软件初始化流程、主循环调度方式、后台任务通信方式和典型业务流程。"),
    bullet("在有限 DRAM 条件下保持系统稳定运行，避免网络、BLE、ESP-SR、游戏和音频模块互相抢占关键资源。"),
    h2("1.2 系统功能"),
    makeTable(
      ["功能模块", "实现内容", "关键资源"],
      [
        ["环境监测", "DHT11 读取温湿度，LVGL 卡片式实时显示。", "GPIO13 单总线，env_read 后台任务"],
        ["天气与日期", "HTTPS 获取天气 JSON，SNTP 同步时间并显示实时时钟。", "WiFi、esp_http_client、cJSON"],
        ["聊天助手", "屏幕键盘输入文本，调用 OpenAI 兼容接口返回回复。", "WiFi、LLM REST API、PSRAM 缓冲"],
        ["在线语音助手", "录音、百度 ASR、LLM 回复、百度 TTS 流式播放。", "INMP441、MAX98357A、I2S0/I2S1、PSRAM"],
        ["离线语音命令", "ESP-SR AFE + MultiNet7 中文命令词识别，识别后跳转功能。", "ESP-SR 模型分区、I2S0、DRAM/PSRAM"],
        ["游戏", "内置 2048 与 Game Boy ROM 模拟器，支持矩阵键盘和 MPU6050 倾斜控制。", "LCD SPI2、SD SPI3、PSRAM、矩阵键盘"],
        ["音乐", "扫描 SD 卡音乐目录，播放 WAV/MP3，支持暂停、停止、切歌。", "MicroSD、Helix MP3、I2S1"],
        ["BLE 配网", "NimBLE 广播，接收 SSID/password 并触发 WiFi 连接，同时支持远程音乐控制。", "BLE controller、NimBLE GATT"],
        ["智能设备", "通过巴法云 API 拉取设备、发送 on/off，并支持语音控制。", "WiFi、HTTPS、ASR、JSON"],
      ],
      [1800, 4600, CONTENT_W - 6400],
    ),
    pageBreak(),
  );

  children.push(
    h1("2 方案论证"),
    h2("2.1 主控平台方案"),
    para("本系统选用 ESP32-S3-DevKitC-1 N16R8 作为主控，主要原因是其具备双核 240 MHz MCU、WiFi、BLE、USB 下载调试能力、16 MB Flash 和 8 MB Octal PSRAM。项目需要同时运行 LVGL、HTTPS、语音识别/合成、MP3 解码、游戏模拟器和 BLE，普通 ESP32 或无 PSRAM 方案难以承受录音缓冲、ROM 数据、HTTP 响应和图形缓冲的内存需求。"),
    makeTable(
      ["方案", "优点", "不足", "结论"],
      [
        ["ESP32-S3 N16R8", "集成 WiFi/BLE，8MB PSRAM，AI 指令和 ESP-SR 支持较好，生态成熟。", "内部 SRAM 仍有限，需要精细资源管理。", "采用"],
        ["普通 ESP32", "成本低，资料丰富。", "PSRAM/向量能力和 BLE/WiFi 共存资源较弱，语音和图形压力较大。", "不采用"],
        ["STM32 + 外挂 WiFi/BLE", "实时性好，可扩展外设多。", "网络、TLS、语音云接口和 BLE 软件栈集成成本高。", "不采用"],
        ["Linux SBC", "算力强，UI 与音频处理简单。", "功耗、成本和嵌入式实时性不符合本项目定位。", "不采用"],
      ],
      [1900, 3000, 3000, CONTENT_W - 7900],
    ),
    h2("2.2 显示与交互方案"),
    para("显示部分采用 240x320 ILI9341 SPI 屏。该屏幕接口简单、刷新速度足够，80 MHz SPI 配合 LVGL PARTIAL 模式和 DMA 双缓冲能够满足菜单、天气和聊天界面的刷新要求。输入部分采用旋转编码器作为菜单导航设备，A/B 相由 PCNT 硬件正交计数，按键单独防抖；游戏输入使用 3x3 矩阵键盘，方向键、A/B、START/SELECT 和 EXIT 都有独立映射。"),
    para("相比纯触摸屏方案，编码器与矩阵键盘不依赖触控校准，结构简单，便于游戏类输入。相比多个独立按键，矩阵键盘节省 GPIO，并且可以通过扫描任务统一消抖。"),
    h2("2.3 语音方案"),
    para("语音功能分为在线语音助手和离线语音命令两条路径。在线路径使用 INMP441 数字麦克风采集 16 kHz 单声道 PCM，发送到百度 ASR，识别文本再送入 LLM，最后通过百度 TTS 返回 PCM 音频并由 MAX98357A 播放。该方案识别和回复能力强，适合自然语言问答。"),
    para("离线路径使用 ESP-SR 的 AFE 和 MultiNet7 中文命令词模型，命令词固定为“打开环境监测、打开天气和日期、打开游戏”等。它不依赖网络，适合菜单跳转和固定控制。两条路径共享 I2S_NUM_0，因此系统采用 deinit/reinit 方式在在线 ASR 与 ESP-SR 之间切换，避免 I2S 资源冲突。"),
    h2("2.4 存储与音频方案"),
    para("项目将 ROM 和音乐文件放置在 MicroSD 卡中，而不是存放在内部 Flash。这样可以避免 16 MB Flash 被大文件占满，也便于用户通过读卡器更新 ROM 和音乐。SD 卡使用 SPI3_HOST，LCD 使用 SPI2_HOST，两条 SPI 总线互相隔离。音频输出采用 MAX98357A I2S D 类功放，占用 I2S_NUM_1，与麦克风输入 I2S_NUM_0 分离。"),
    h2("2.5 资源管理方案"),
    para("ESP32-S3 内部 DRAM 是系统稳定性的关键限制。工程中 HTTPS/cJSON 业务任务、音乐任务和部分响应缓冲优先放入 PSRAM；ESP-SR、BLE、WiFi 等大资源模块按需开启和释放；游戏模式下暂停 WiFi/BLE 和 LVGL 刷新，让游戏渲染独占 LCD SPI。这些策略共同降低了内存碎片和实时任务互相干扰的风险。"),
    pageBreak(),
  );

  children.push(
    h1("3 硬件电路设计"),
    h2("3.1 原理图"),
    para("本工程原理图文件位于工程根目录，文件名为 SCH_Schematic1_1-P1_2026-05-22.png。原理图展示了 ESP32-S3 两排排针与 LCD、MAX98357A、INMP441、SD 卡、MPU6050、DHT11、矩阵键盘、编码器和传感器接口之间的连接关系。"),
    imageParagraph(SCH_PATH, 560, Math.round(560 * 1672 / 2386), "系统原理图"),
    caption("图 1 系统原理图（根目录 SCH_Schematic1_1-P1_2026-05-22.png）"),
    h2("3.2 电源与主控"),
    para("系统通过外部 5 V 输入给开发板供电，外设模块主要使用开发板提供的 3.3 V。ESP32-S3-DevKitC-1 N16R8 的 16 MB Flash 用于固件、OTA 和 ESP-SR 模型分区，8 MB Octal PSRAM 用于录音、HTTP 响应、ROM、图形和部分任务栈。原理图中 U1/U2 排针将 GPIO 引出到各功能模块。"),
    h2("3.3 LCD 显示电路"),
    makeTable(
      ["信号", "ESP32-S3 GPIO", "说明"],
      [
        ["MOSI", "GPIO11", "SPI2 数据输出"],
        ["SCLK", "GPIO12", "SPI2 时钟，工程配置 80 MHz"],
        ["RES", "GPIO10", "LCD 复位"],
        ["DC", "GPIO9", "命令/数据选择"],
        ["BLK", "GPIO46", "背光控制，高电平点亮"],
        ["VCC/GND", "3.3 V/GND", "屏幕供电"],
      ],
      [2200, 2200, CONTENT_W - 4400],
    ),
    para("LCD 驱动芯片为 ILI9341，工程在 lcd.c 中完成复位、寄存器初始化、RGB565 色彩格式和背光控制。LVGL 显示端口使用 PARTIAL 渲染模式，两个 34 行 DMA 缓冲放在内部 DRAM 中，flush 时按脏区连续像素分块异步 DMA 到 SPI。游戏模式下通过 lv_port_disp_suspend() 暂停 LVGL 输出，让游戏 runtime 独占 SPI。"),
    h2("3.4 音频输入输出电路"),
    makeTable(
      ["模块", "接口信号", "GPIO", "说明"],
      [
        ["INMP441 麦克风", "BCLK / LRCK / DATA", "GPIO38 / GPIO39 / GPIO40", "I2S_NUM_0，32bit stereo 采集后软件取左声道转 16bit mono。"],
        ["MAX98357A 功放", "BCLK / LRCK / DIN", "GPIO15 / GPIO16 / GPIO17", "I2S_NUM_1，16bit mono 输出到 D 类功放。"],
      ],
      [1600, 2100, 2400, CONTENT_W - 6100],
    ),
    para("INMP441 的 L/R 接地时输出左声道，软件读取 stereo 槽的左声道数据，并右移转换成 16 bit PCM。MAX98357A 使用 I2S 标准 Philips 格式输出，speaker.c 中加入 RingBuffer、DMA auto_clear、DC-block 高通滤波、淡入和音量 Q15 定点增益，降低爆音、直流偏置和断续播放噪声。"),
    h2("3.5 输入、传感器与存储电路"),
    makeTable(
      ["模块", "连接", "说明"],
      [
        ["旋转编码器", "A=GPIO4，B=GPIO5，SW=GPIO6", "A/B 相使用 PCNT 计数，SW 5 ms 轮询并 20 ms 防抖。"],
        ["3x3 矩阵键盘", "行 GPIO1/GPIO2/GPIO14，列 GPIO41/GPIO42/GPIO47", "2 ms 扫描，连续多次一致后确认；游戏模式下输出方向和功能键。"],
        ["DHT11", "DATA=GPIO13", "单总线时序读取温湿度，首次 warmup，1 s 采样。"],
        ["MPU6050", "SDA=GPIO20，SCL=GPIO7", "I2C_NUM_0，400 kHz，2048 游戏进入时初始化并校准中立姿态。"],
        ["MicroSD", "CS=GPIO0，MOSI=GPIO8，SCK=GPIO18，MISO=GPIO21", "SPI3_HOST，FAT32，挂载点 /sdcard，保存 ROM 和音乐文件。"],
      ],
      [1700, 3200, CONTENT_W - 4900],
    ),
    para("硬件接口分配遵循总线隔离原则：LCD 独占 SPI2，SD 卡使用 SPI3，麦克风使用 I2S0，扬声器使用 I2S1。这样可以降低高速显示刷新、文件读写和音频流之间的冲突。GPIO0 用作 SD 卡 CS，需要注意上电时不要按住 Boot 键。"),
    pageBreak(),
  );

  children.push(
    h1("4 系统工作原理"),
    h2("4.1 总体架构"),
    para("系统可以分为硬件驱动层、系统服务层、业务功能层和图形交互层。硬件驱动层包括 LCD、I2S 麦克风、I2S 扬声器、SD 卡、DHT11、MPU6050、编码器和矩阵键盘；系统服务层包括 WiFi、BLE、SNTP、PSRAM 任务管理、音频 RingBuffer 和健康监控；业务功能层包括天气、LLM、ASR、TTS、ESP-SR、音乐、游戏和巴法云；图形交互层由 LVGL 主菜单和各功能页面组成。"),
    para("app_main() 初始化 NVS、PSRAM 任务管理、音频、各业务模块互斥锁、DHT11、LCD、SD 卡、矩阵键盘和 LVGL，然后创建 lv_tick 与 lv_task。lv_task 先调用 my_demo() 创建主菜单，再循环执行 lv_timer_handler()。后台任务不能直接操作 LVGL 对象，而是通过 FreeRTOS Queue 将结果送回，LVGL timer 在 UI 线程中轮询并刷新界面。"),
    h2("4.2 在线语音助手工作原理"),
    num("用户在语音助手界面按键开始录音，asr_record_start() 使能 I2S_NUM_0。"),
    num("asr_rec 任务持续读取 INMP441 32bit stereo 数据，取左声道转换成 16 kHz、16 bit、mono PCM，并写入 PSRAM 录音缓冲。"),
    num("用户停止录音后，asr_recognize() 将 PCM 以 audio/pcm;rate=16000 发送到百度 ASR API。"),
    num("识别结果文本作为用户输入送入 model_chat()，该函数按 OpenAI 兼容 chat/completions JSON 格式发起 HTTPS 请求。"),
    num("LLM 回复送入 tts_speak()，百度 TTS 返回 WAV/PCM 音频流，HTTP 回调边收边调用 speaker_play()。"),
    num("speaker_play() 对 PCM 做字节对齐、DC-block、淡入和音量处理后写入 RingBuffer，spk_tx 任务输出到 MAX98357A。"),
    h2("4.3 离线语音命令工作原理"),
    para("离线语音命令由 ESP-SR 实现。进入该功能时，系统先释放在线 ASR 占用的 I2S_NUM_0，然后加载模型分区中的 MultiNet7 中文模型，创建 AFE 和 MultiNet 实例，并注册 9 条中文拼音命令。监听阶段分为 read、feed、detect 三个任务：read 任务将 I2S 数据转换成 mono PCM 写入 RingBuffer，feed 任务凑齐 AFE chunk 后喂入 AFE，detect 任务从 AFE fetch 特征并执行 MultiNet 检测。检测到命令后回调 my_demo.c，由状态机先停止 ESP-SR、释放资源，再根据命令跳转到目标功能。"),
    h2("4.4 游戏工作原理"),
    para("游戏列表包含内置 2048 和 SD 卡 /sdcard/rom/ 下的 Game Boy ROM。进入游戏时系统暂停 WiFi、BLE 和 LVGL flush，矩阵键盘切换到游戏模式。Game Boy 模拟器使用 Walnut-CGB，ROM 从 SD 卡加载到 PSRAM，GB 原始 160x144 图像按 1.5 倍缩放为 240x216，通过 SPI DMA 分组刷新 LCD。MiniGB APU 在 Core 0 独立任务中合成音频，主仿真在 Core 1 运行并每帧通知音频任务。"),
    para("2048 游戏使用 PSRAM framebuffer 绘制 240x240 棋盘，动画阶段先完整绘制到内存，再整块刷新到 LCD，避免直接多次打屏造成撕裂。MPU6050 进入游戏时初始化并采样当前姿态作为中立基线，之后使用 ENTER/EXIT 双阈值和方向锁定，将倾斜映射为上下左右；矩阵键盘优先于倾斜输入。"),
    h2("4.5 音乐播放工作原理"),
    para("音乐模块扫描 /sdcard/music 目录中的 WAV 和 MP3 文件。WAV 解析 RIFF/PCM 头后读取 16 bit PCM，并在双声道情况下做 mono 降混；MP3 采用 Helix 定点解码器，先跳过 ID3v2 tag，再查找同步字并逐帧解码。音乐播放前根据文件采样率调用 speaker_set_sample_rate() 动态切换 I2S 时钟，播放结束后恢复默认 16 kHz，避免 TTS 和 GB 音频变调。"),
    pageBreak(),
  );

  children.push(
    h1("5 软件流程设计"),
    para("软件流程图如下图所示。图中上半部分描述系统初始化和主菜单调度，下半部分描述各功能分支。虚线表示功能结束或后台结果回传后返回 LVGL 主线程更新界面。"),
    svgImageParagraph(FLOW_SVG, FLOW_PNG, 590, 387, "软件总体流程图"),
    caption("图 2 软件总体流程图"),
    h2("5.1 初始化流程"),
    num("初始化 NVS。若 NVS 版本或页空间异常，则擦除后重新初始化。"),
    num("启动 PSRAM 任务管理器，用于创建栈位于 PSRAM 的一次性业务任务，并由 cleaner 回收栈和 TCB。"),
    num("初始化 speaker、bemfa、baidu_token、model、weather、music、DHT11 等模块，使互斥锁和内部状态提前就绪。"),
    num("初始化 LCD，并尝试挂载 SD 卡；挂载失败不影响主菜单，但 ROM 和音乐功能不可用。"),
    num("初始化矩阵键盘、LVGL、显示端口和编码器输入端口。"),
    num("创建 LVGL tick 任务和 LVGL 主任务，最后启动 health 监控任务。"),
    h2("5.2 主菜单与 UI 调度"),
    para("my_demo() 创建 LVGL 主菜单列表，用户旋转编码器选择功能，按下确认后进入对应页面。各页面创建自己的 LVGL group，弹窗和页面销毁时释放 group，避免编码器焦点残留。网络、语音、天气、巴法云等耗时操作均在后台任务中执行，任务结果写入 Queue，LVGL timer 周期读取 Queue 后更新界面。"),
    h2("5.3 线程与任务设计"),
    makeTable(
      ["任务/机制", "作用", "设计要点"],
      [
        ["lv_tick / lv_task", "提供 LVGL 时钟和 UI 渲染主循环。", "所有 LVGL 对象操作集中在 lv_task，游戏暂停时跳过 lv_timer_handler。"],
        ["enc_task / kpad_task", "采集编码器按键和矩阵键盘。", "编码器 A/B 走 PCNT，键盘 2 ms 扫描并投票消抖。"],
        ["spk_tx", "从 RingBuffer 取 PCM 并写 I2S。", "输出路径非阻塞，空闲时 DMA auto_clear 输出零电平。"],
        ["asr_rec", "在线 ASR 录音。", "常驻任务，录音时读取 I2S0，写入 PSRAM PCM 缓冲。"],
        ["weather/chat/asr_llm/bemfa", "一次性网络业务任务。", "栈使用 PSRAM，结果通过 Queue 回传 UI。"],
        ["game_run/apu_task", "游戏仿真与 Game Boy 音频。", "游戏主循环高优先级，APU 独立核运行。"],
        ["esp_sr_read/feed/detect", "离线命令识别流水线。", "I2S 读取、AFE 喂入、MultiNet 检测分离。"],
        ["psram_cleaner", "回收 PSRAM 任务栈和 TCB。", "任务函数正常 return 后排队释放资源。"],
      ],
      [2200, 3000, CONTENT_W - 5200],
    ),
    h2("5.4 关键异常处理"),
    bullet("WiFi 连接失败时通过 UI 弹窗提示，运行期断线由 wifi_guard 指数退避重连。"),
    bullet("SD 卡挂载失败不阻塞启动，只影响需要文件系统的 ROM 和音乐功能。"),
    bullet("HTTP 响应缓冲按 4 KB 起步动态扩容，最大 32 KB，接口返回后释放，避免长期占用 PSRAM。"),
    bullet("ESP-SR 停止时先等待任务退出，再释放 I2S 和模型资源；若极端情况下任务无法退出，优先避免系统崩溃。"),
    bullet("音乐切歌和停止通过后台任务异步执行，避免阻塞 LVGL 线程。"),
    pageBreak(),
  );

  children.push(
    h1("6 关键模块设计说明"),
    h2("6.1 LCD 与 LVGL 显示"),
    para("lcd.c 完成 ILI9341 的底层 SPI 和寄存器初始化，lv_port_disp.c 在此基础上注册 LVGL display。LVGL 使用 RGB565_SWAPPED 色彩格式，两个 16 KB 左右的 DMA buffer 放在内部 DRAM，避免 PSRAM 到 DMA 的 bounce copy。disp_flush() 先等待上一轮 DMA 完成，再设置 LCD 地址窗口，然后将脏区像素按 ESP32-S3 SPI DMA 单次上限分块入队。"),
    h2("6.2 音频播放"),
    para("speaker.c 以 RingBuffer 连接上层音频生产者和 I2S 输出任务。上层可以来自 TTS、Game Boy APU 或音乐解码器。播放路径处理了 HTTP chunk 奇数字节导致的 16 bit 样本错位问题，并加入高通滤波、线性淡入和对数音量。speaker_set_sample_rate() 在切换采样率前通过 flush 协议清空 ring，避免旧采样率数据被新采样率播放。"),
    h2("6.3 网络与云服务"),
    para("WiFi 模块维护连接状态、IP 快照和断线守护任务。天气、LLM、百度 ASR/TTS、巴法云等模块均使用 esp_http_client 进行 HTTPS 请求，并通过 cJSON 解析响应。为避免并发 HTTP client 和共享缓冲冲突，各模块使用 mutex 串行化请求。"),
    h2("6.4 BLE 配网"),
    para("BLE 使用 NimBLE 外设角色，设备名为 ESP32-Bot，提供 HM-10 兼容的 0xFFE0/0xFFE1 服务和特征。手机发送 SSID_名称 password_密码 后，BLE worker 解析凭据并调用 wifi_set_credentials()，随后由 UI 侧触发 WiFi 连接。BLE 还支持 /music on、/music off、/序号 和 /voice、/v1 等控制命令。"),
    h2("6.5 存储与文件系统"),
    para("MicroSD 通过 SDSPI 挂载到 /sdcard，FATFS 开启 UTF-8 API、长文件名和 GBK 代码页 936。工程约定 /sdcard/rom 存放 .gb/.gbc ROM，/sdcard/music 存放 .wav/.mp3 音乐。sdcard.c 对挂载、卸载、容量查询和总线初始化状态使用 mutex 保护。"),
    h2("6.6 内存管理"),
    para("工程大量使用 PSRAM 来承载大缓冲和任务栈，但关键 DMA buffer、TCB 和某些实时路径仍放内部 DRAM。sdkconfig.defaults 开启 SPIRAM_RODATA、WiFi/LwIP PSRAM 分配、mbedTLS 外部内存分配和 FreeRTOS 外部栈支持。分区表为双 OTA app 分区和 ESP-SR 模型分区预留空间，ROM/音乐不占用内部 Flash。"),
    pageBreak(),
  );

  children.push(
    h1("7 测试与验证建议"),
    makeTable(
      ["测试项", "测试方法", "期望结果"],
      [
        ["上电启动", "烧录固件后观察串口和 LCD。", "LCD 背光点亮并进入主菜单，串口打印初始化日志。"],
        ["DHT11", "进入环境监测页面，观察温湿度刷新。", "约 1 s 刷新一次，偶发读失败不闪烁。"],
        ["WiFi/BLE 配网", "进入蓝牙页面，手机连接 ESP32-Bot，发送 SSID/password。", "BLE 返回结果，WiFi 成功连接并显示 IP。"],
        ["在线语音助手", "连接 WiFi 后录音提问。", "ASR 得到文字，LLM 返回回复，TTS 能播放。"],
        ["离线语音命令", "开启语音命令，说“打开游戏”等命令词。", "识别后自动关闭 SR 并跳转功能。"],
        ["音乐播放", "SD 卡放入 WAV/MP3，进入音乐页面播放。", "可扫描列表，播放、暂停、停止和切歌正常。"],
        ["游戏", "SD 卡放入 ROM 或进入内置 2048。", "游戏运行时按键响应，长按中键退出后回到菜单。"],
        ["智能设备", "配置巴法云 UID 后进入设备页面。", "能拉取设备列表并发送 on/off。"],
      ],
      [1900, 4000, CONTENT_W - 5900],
    ),
    h1("8 总结"),
    para("本系统围绕 ESP32-S3 构建了一个集语音、图形、网络、传感器、音乐和游戏于一体的嵌入式智能终端。硬件上通过 SPI、I2S、I2C、单总线、矩阵键盘和 BLE/WiFi 等接口完成多外设连接；软件上通过 LVGL 单线程 UI、FreeRTOS 多任务、PSRAM 资源调度、异步 Queue 回传和模块级 mutex 实现复杂功能协同。"),
    para("方案论证表明，ESP32-S3 N16R8 在成本、集成度、PSRAM 容量和生态支持之间较为均衡，适合本项目。工程中的关键设计在于资源冲突处理：I2S0 在在线 ASR 与离线 ESP-SR 间切换，LCD SPI 在 LVGL 与游戏间切换，WiFi/BLE/ESP-SR 在内存紧张场景下按需启停，音频输出通过统一 speaker 模块仲裁。整体上，该设计能够说明系统硬件连接、工作原理和软件流程，并具备进一步扩展传感器、云端控制和本地应用的基础。"),
  );

  return new Document({
    styles: {
      default: {
        document: {
          run: { font: FONT, size: 22 },
          paragraph: { spacing: { line: 320 } },
        },
      },
      paragraphStyles: [
        {
          id: "Heading1",
          name: "Heading 1",
          basedOn: "Normal",
          next: "Normal",
          quickFormat: true,
          run: { size: 32, bold: true, font: FONT },
          paragraph: { spacing: { before: 160, after: 100 }, outlineLevel: 0, keepNext: true },
        },
        {
          id: "Heading2",
          name: "Heading 2",
          basedOn: "Normal",
          next: "Normal",
          quickFormat: true,
          run: { size: 28, bold: true, font: FONT },
          paragraph: { spacing: { before: 120, after: 70 }, outlineLevel: 1, keepNext: true },
        },
        {
          id: "Heading3",
          name: "Heading 3",
          basedOn: "Normal",
          next: "Normal",
          quickFormat: true,
          run: { size: 24, bold: true, font: FONT },
          paragraph: { spacing: { before: 100, after: 60 }, outlineLevel: 2, keepNext: true },
        },
      ],
    },
    numbering: {
      config: [
        {
          reference: "bullets",
          levels: [
            {
              level: 0,
              format: LevelFormat.BULLET,
              text: "•",
              alignment: AlignmentType.LEFT,
              style: { paragraph: { indent: { left: 720, hanging: 360 } } },
            },
          ],
        },
        {
          reference: "numbers",
          levels: [
            {
              level: 0,
              format: LevelFormat.DECIMAL,
              text: "%1.",
              alignment: AlignmentType.LEFT,
              style: { paragraph: { indent: { left: 720, hanging: 360 } } },
            },
          ],
        },
      ],
    },
    sections: [
      {
        properties: {
          page: {
            size: { width: PAGE_W, height: PAGE_H },
            margin: MARGIN,
          },
        },
        headers: {
          default: new Header({
            children: [
              new Paragraph({
                children: [run("ESP32-S3 AI 智能语音助手与游戏系统设计报告", { size: 18, color: "666666" })],
                alignment: AlignmentType.CENTER,
              }),
            ],
          }),
        },
        footers: {
          default: new Footer({
            children: [
              new Paragraph({
                alignment: AlignmentType.CENTER,
                children: [
                  run("第 ", { size: 18, color: "666666" }),
                  new TextRun({ children: [PageNumber.CURRENT], size: 18, font: FONT_LATIN, color: "666666" }),
                  run(" 页", { size: 18, color: "666666" }),
                ],
              }),
            ],
          }),
        },
        children,
      },
    ],
  });
}

async function main() {
  if (!fs.existsSync(SCH_PATH)) {
    throw new Error(`missing schematic image: ${SCH_PATH}`);
  }
  createFlowSvg();
  await createFlowPng();
  const doc = buildDoc();
  const buffer = await Packer.toBuffer(doc);
  fs.writeFileSync(DOCX_OUT, buffer);
  console.log(DOCX_OUT);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
