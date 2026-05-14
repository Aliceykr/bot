const fs = require("fs");
const { Document, Packer, Paragraph, TextRun, Table, TableRow, TableCell,
        Header, Footer, AlignmentType, LevelFormat,
        HeadingLevel, BorderStyle, WidthType, ShadingType,
        PageNumber, PageBreak, TableOfContents } = require("docx");

// Helper functions
const h1 = (text) => new Paragraph({ heading: HeadingLevel.HEADING_1, children: [new TextRun({ text, bold: true })] });
const h2 = (text) => new Paragraph({ heading: HeadingLevel.HEADING_2, children: [new TextRun({ text, bold: true })] });
const h3 = (text) => new Paragraph({ heading: HeadingLevel.HEADING_3, children: [new TextRun({ text, bold: true })] });
const p = (text, opts = {}) => new Paragraph({ spacing: { after: 120 }, children: [new TextRun({ text, size: 24, ...opts })] });
const pIndent = (text) => new Paragraph({ spacing: { after: 100 }, indent: { left: 480 }, children: [new TextRun({ text, size: 24 })] });
const empty = () => new Paragraph({ spacing: { after: 120 }, children: [] });

const border = { style: BorderStyle.SINGLE, size: 1, color: "999999" };
const borders = { top: border, bottom: border, left: border, right: border };
const cellMargins = { top: 60, bottom: 60, left: 100, right: 100 };

function makeCell(text, width, opts = {}) {
  return new TableCell({
    borders, width: { size: width, type: WidthType.DXA },
    margins: cellMargins,
    shading: opts.header ? { fill: "D5E8F0", type: ShadingType.CLEAR } : undefined,
    children: [new Paragraph({ children: [new TextRun({ text, size: 22, bold: !!opts.header })] })]
  });
}

function makeTable(headers, rows, colWidths) {
  const totalWidth = colWidths.reduce((a, b) => a + b, 0);
  const headerRow = new TableRow({ children: headers.map((h, i) => makeCell(h, colWidths[i], { header: true })) });
  const dataRows = rows.map(row => new TableRow({ children: row.map((cell, i) => makeCell(cell, colWidths[i])) }));
  return new Table({ width: { size: totalWidth, type: WidthType.DXA }, columnWidths: colWidths, rows: [headerRow, ...dataRows] });
}


// ============ Document Content ============
const doc = new Document({
  styles: {
    default: { document: { run: { font: "SimSun", size: 24 } } },
    paragraphStyles: [
      { id: "Heading1", name: "Heading 1", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { size: 36, bold: true, font: "SimHei" },
        paragraph: { spacing: { before: 360, after: 240 }, outlineLevel: 0 } },
      { id: "Heading2", name: "Heading 2", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { size: 30, bold: true, font: "SimHei" },
        paragraph: { spacing: { before: 240, after: 180 }, outlineLevel: 1 } },
      { id: "Heading3", name: "Heading 3", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { size: 26, bold: true, font: "SimHei" },
        paragraph: { spacing: { before: 180, after: 120 }, outlineLevel: 2 } },
    ]
  },
  numbering: {
    config: [
      { reference: "bullets", levels: [{ level: 0, format: LevelFormat.BULLET, text: "\u2022", alignment: AlignmentType.LEFT, style: { paragraph: { indent: { left: 720, hanging: 360 } } } }] },
      { reference: "numbers", levels: [{ level: 0, format: LevelFormat.DECIMAL, text: "%1.", alignment: AlignmentType.LEFT, style: { paragraph: { indent: { left: 720, hanging: 360 } } } }] },
    ]
  },
  sections: [
    // ===== Cover Page =====
    {
      properties: {
        page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } }
      },
      children: [
        empty(), empty(), empty(), empty(), empty(), empty(),
        new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 600 }, children: [new TextRun({ text: "ESP32-S3 AI\u667A\u80FD\u8BED\u97F3\u52A9\u624B", size: 52, bold: true, font: "SimHei" })] }),
        new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 200 }, children: [new TextRun({ text: "& Game Boy \u6A21\u62DF\u5668", size: 44, bold: true, font: "SimHei" })] }),
        new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 600 }, children: [new TextRun({ text: "\u8BBE\u8BA1\u62A5\u544A", size: 48, bold: true, font: "SimHei" })] }),
        empty(), empty(), empty(), empty(),
        new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 200 }, children: [new TextRun({ text: "\u57FA\u4E8E ESP-IDF + LVGL \u7684\u5D4C\u5165\u5F0F\u591A\u529F\u80FD\u667A\u80FD\u7EC8\u7AEF", size: 24 })] }),
        empty(), empty(),
        new Paragraph({ alignment: AlignmentType.CENTER, children: [new TextRun({ text: "2026 \u5E74 5 \u6708", size: 24 })] }),
      ]
    },
    // ===== TOC =====
    {
      properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
      headers: { default: new Header({ children: [new Paragraph({ children: [new TextRun({ text: "ESP32-S3 AI\u667A\u80FD\u8BED\u97F3\u52A9\u624B \u8BBE\u8BA1\u62A5\u544A", size: 18, italics: true })] })] }) },
      footers: { default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER, children: [new TextRun({ text: "\u7B2C ", size: 20 }), new TextRun({ children: [PageNumber.CURRENT], size: 20 }), new TextRun({ text: " \u9875", size: 20 })] })] }) },
      children: [
        h1("\u76EE\u5F55"),
        empty(),
        p("1 \u9879\u76EE\u6982\u8FF0"),
        pIndent("1.1 \u9879\u76EE\u80CC\u666F"),
        pIndent("1.2 \u4E3B\u8981\u529F\u80FD"),
        pIndent("1.3 \u786C\u4EF6\u5E73\u53F0"),
        p("2 \u65B9\u6848\u8BBA\u8BC1"),
        pIndent("2.1 \u4E3B\u63A7\u82AF\u7247\u9009\u578B"),
        pIndent("2.2 \u97F3\u9891\u67B6\u6784\u65B9\u6848\u5BF9\u6BD4"),
        pIndent("2.3 \u5185\u5B58\u7BA1\u7406\u7B56\u7565"),
        pIndent("2.4 \u6E38\u620F\u6A21\u62DF\u5668\u65B9\u6848"),
        p("3 \u7CFB\u7EDF\u5DE5\u4F5C\u539F\u7406"),
        pIndent("3.1 \u7CFB\u7EDF\u603B\u4F53\u67B6\u6784"),
        pIndent("3.2 \u8BED\u97F3\u52A9\u624B\u5DE5\u4F5C\u6D41\u7A0B"),
        pIndent("3.3 \u6E38\u620F\u6A21\u62DF\u5668\u5DE5\u4F5C\u539F\u7406"),
        pIndent("3.4 \u97F3\u9891\u8F93\u51FA\u539F\u7406"),
        p("4 \u7535\u8DEF\u8BBE\u8BA1"),
        pIndent("4.1 \u7CFB\u7EDF\u7535\u8DEF\u6846\u56FE"),
        pIndent("4.2 \u5F15\u811A\u5206\u914D\u8868"),
        pIndent("4.3 \u7535\u8DEF\u539F\u7406\u56FE"),
        p("5 \u8F6F\u4EF6\u6D41\u7A0B"),
        pIndent("5.1 \u7CFB\u7EDF\u542F\u52A8\u6D41\u7A0B"),
        pIndent("5.2 \u4E3B\u83DC\u5355\u4EA4\u4E92\u6D41\u7A0B"),
        pIndent("5.3 \u591A\u4EFB\u52A1\u534F\u4F5C\u6A21\u578B"),
        pIndent("5.4 I2S \u8D44\u6E90\u5171\u4EAB\u673A\u5236"),
        p("6 \u5173\u952E Bug \u5206\u6790\u4E0E\u4FEE\u590D"),
        pIndent("6.1 WiFi \u8FDE\u63A5\u65F6\u5F00\u542F\u84DD\u7259\u5D29\u6E83"),
        pIndent("6.2 \u5B88\u62A4\u4EFB\u52A1\u786C\u6740\u5BFC\u81F4 Mutex \u6B7B\u9501"),
        pIndent("6.3 \u767E\u5EA6 Token \u6307\u9488\u60AC\u7A7A"),
        pIndent("6.4 \u65E0 WiFi \u65F6\u8BED\u97F3\u5BF9\u8BDD\u5D29\u6E83"),
        pIndent("6.5 \u6E38\u620F\u4E0E WiFi \u5185\u5B58\u51B2\u7A81\u5D29\u6E83"),
        pIndent("6.6 \u97F3\u9891\u6742\u97F3\u4E0E\u5361\u5572\u58F0"),
        pIndent("6.7 \u6A21\u5757 Mutex \u5E76\u53D1\u521D\u59CB\u5316\u7ADE\u6001"),
        p("7 \u603B\u7ED3"),
        pIndent("7.1 \u5B9E\u73B0\u6210\u679C"),
        pIndent("7.2 \u5F00\u53D1\u7ECF\u9A8C\u603B\u7ED3"),
        pIndent("7.3 \u4EE3\u7801\u7EDF\u8BA1"),
        new Paragraph({ children: [new PageBreak()] }),
      ]
    },

    // ===== Chapter 1: Overview =====
    {
      properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
      headers: { default: new Header({ children: [new Paragraph({ children: [new TextRun({ text: "ESP32-S3 AI\u667A\u80FD\u8BED\u97F3\u52A9\u624B \u8BBE\u8BA1\u62A5\u544A", size: 18, italics: true })] })] }) },
      footers: { default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER, children: [new TextRun({ text: "\u7B2C ", size: 20 }), new TextRun({ children: [PageNumber.CURRENT], size: 20 }), new TextRun({ text: " \u9875", size: 20 })] })] }) },
      children: [
        h1("1 \u9879\u76EE\u6982\u8FF0"),
        h2("1.1 \u9879\u76EE\u80CC\u666F"),
        p("\u672C\u9879\u76EE\u8BBE\u8BA1\u5E76\u5B9E\u73B0\u4E86\u4E00\u6B3E\u57FA\u4E8E ESP32-S3 \u7684\u591A\u529F\u80FD\u667A\u80FD\u7EC8\u7AEF\uFF0C\u96C6\u6210\u4E86 AI \u8BED\u97F3\u52A9\u624B\u3001Game Boy \u6E38\u620F\u6A21\u62DF\u5668\u3001\u97F3\u4E50\u64AD\u653E\u5668\u3001\u667A\u80FD\u5BB6\u5C45\u63A7\u5236\u7B49\u529F\u80FD\u4E8E\u4E00\u4F53\u3002\u9879\u76EE\u91C7\u7528 ESP-IDF v5.4 \u5F00\u53D1\u6846\u67B6\uFF0C\u914D\u5408 LVGL \u56FE\u5F62\u754C\u9762\u5E93\uFF0C\u5B9E\u73B0\u4E86\u4E30\u5BCC\u7684\u4EBA\u673A\u4EA4\u4E92\u754C\u9762\u3002"),

        empty(),
        h2("1.2 \u4E3B\u8981\u529F\u80FD"),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u5929\u6C14\u67E5\u8BE2\u4E0E\u5B9E\u65F6\u65F6\u949F\u663E\u793A", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "AI \u804A\u5929\u52A9\u624B\uFF08LLM \u5927\u6A21\u578B\u5BF9\u8BDD\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u5728\u7EBF\u8BED\u97F3\u52A9\u624B\uFF08\u5F55\u97F3 \u2192 ASR \u2192 LLM \u2192 TTS\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u79BB\u7EBF\u8BED\u97F3\u547D\u4EE4\u8BC6\u522B\uFF08ESP-SR MultiNet7\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "Game Boy \u6E38\u620F\u6A21\u62DF\u5668\uFF08Walnut-CGB\uFF0960fps\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "SD \u5361\u97F3\u4E50\u64AD\u653E\u5668\uFF08WAV + MP3\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "BLE \u84DD\u7259\u914D\u7F51\u4E0E\u8FDC\u7A0B\u97F3\u4E50\u63A7\u5236", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u5DF4\u6CD5\u4E91\u667A\u80FD\u8BBE\u5907\u63A7\u5236", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u97F3\u91CF\u8C03\u8282\uFF08NVS \u6301\u4E45\u5316\uFF09", size: 24 })] }),
        empty(),
        h2("1.3 \u786C\u4EF6\u5E73\u53F0"),
        makeTable(
          ["\u7EC4\u4EF6", "\u578B\u53F7/\u89C4\u683C", "\u8BF4\u660E"],
          [
            ["ESP32-S3", "DevKitC-1 N16R8", "16MB Flash + 8MB PSRAM\uFF0C\u53CC\u6838 240MHz"],
            ["LCD", "2.4\u201D ILI9341 TFT", "240x320\uFF0CSPI 80MHz"],
            ["\u9EA6\u514B\u98CE", "INMP441", "I2S \u6570\u5B57\u9EA6\u514B\u98CE\uFF0C16kHz"],
            ["\u529F\u653E", "MAX98357A", "I2S D\u7C7B\u529F\u653E + \u55C7\u53ED"],
            ["\u8F93\u5165", "\u65CB\u8F6C\u7F16\u7801\u5668 + 3x3\u77E9\u9635\u952E\u76D8", "PCNT\u786C\u4EF6\u89E3\u7801 + GPIO\u626B\u63CF"],
            ["\u5B58\u50A8", "MicroSD\u5361", "SPI3_HOST\uFF0CFAT32"],
          ],
          [2000, 3500, 3526]
        ),
        new Paragraph({ children: [new PageBreak()] }),
      ]
    },

    // ===== Chapter 2: Design Rationale =====
    {
      properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
      headers: { default: new Header({ children: [new Paragraph({ children: [new TextRun({ text: "ESP32-S3 AI\u667A\u80FD\u8BED\u97F3\u52A9\u624B \u8BBE\u8BA1\u62A5\u544A", size: 18, italics: true })] })] }) },
      footers: { default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER, children: [new TextRun({ text: "\u7B2C ", size: 20 }), new TextRun({ children: [PageNumber.CURRENT], size: 20 }), new TextRun({ text: " \u9875", size: 20 })] })] }) },
      children: [
        h1("2 \u65B9\u6848\u8BBA\u8BC1"),
        h2("2.1 \u4E3B\u63A7\u82AF\u7247\u9009\u578B"),
        p("\u9009\u62E9 ESP32-S3 \u800C\u975E ESP32 \u6216 STM32 \u7684\u539F\u56E0\uFF1A"),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u53CC\u6838 Xtensa LX7 @ 240MHz\uFF0C\u8DB3\u4EE5\u540C\u65F6\u8FD0\u884C\u6E38\u620F\u6A21\u62DF\u5668\uFF08Core 1\uFF09\u548C\u97F3\u9891\u5904\u7406\uFF08Core 0\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "8MB Octal PSRAM\uFF0C\u6EE1\u8DB3 ROM \u52A0\u8F7D\u3001\u5F55\u97F3\u7F13\u51B2\u3001HTTP \u54CD\u5E94\u7B49\u5927\u5185\u5B58\u9700\u6C42", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u539F\u751F\u652F\u6301 WiFi + BLE 5.0 \u5171\u5B58\uFF0C\u65E0\u9700\u5916\u6302\u6A21\u5757", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u5B98\u65B9 ESP-SR \u79BB\u7EBF\u8BED\u97F3\u8BC6\u522B\u6846\u67B6\u4EC5\u652F\u6301 ESP32-S3", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "ESP-IDF \u751F\u6001\u6210\u719F\uFF0C\u7EC4\u4EF6\u5E93\u4E30\u5BCC\uFF08LVGL\u3001Helix MP3\u3001NimBLE\u7B49\uFF09", size: 24 })] }),
        empty(),
        h2("2.2 \u97F3\u9891\u67B6\u6784\u65B9\u6848\u5BF9\u6BD4"),
        p("\u5F00\u53D1\u8FC7\u7A0B\u4E2D\u97F3\u9891\u7CFB\u7EDF\u7ECF\u5386\u4E86\u591A\u6B21\u91CD\u6784\uFF1A"),
        makeTable(
          ["\u65B9\u6848", "\u63CF\u8FF0", "\u95EE\u9898", "\u7ED3\u679C"],
          [
            ["\u65B9\u6848A", "\u8F6F\u4EF6 Stereo \u5C55\u5F00 + \u4E3B\u52A8\u586B\u5145\u9759\u97F3", "Ring \u9965\u997F\u5BFC\u81F4\u5468\u671F\u6027\u5361\u5572\u58F0", "\u5E9F\u5F03"],
            ["\u65B9\u6848B", "\u786C\u4EF6 MONO \u69FD + DMA auto_clear", "\u65E0\u6742\u97F3\uFF0C\u7B80\u5316\u6570\u636E\u6D41", "\u91C7\u7528"],
            ["\u65B9\u6848C", "\u5B57\u8282\u5C3E\u5BF9\u9F50 + DC-block HPF", "\u6D88\u9664 HTTP chunked \u5947\u6570\u5B57\u8282\u9519\u4F4D + \u76F4\u6D41\u504F\u7F6E", "\u91C7\u7528"],
          ],
          [1200, 3500, 2800, 1526]
        ),
        empty(),
        h2("2.3 \u5185\u5B58\u7BA1\u7406\u7B56\u7565"),
        p("ESP32-S3 \u5185\u90E8 DRAM \u4EC5 ~338KB\uFF0C\u9879\u76EE\u91C7\u7528\u5206\u5C42\u5185\u5B58\u7B56\u7565\uFF1A"),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "PSRAM \u4F18\u5148\uFF1AHTTP \u7F13\u51B2\u3001\u5F55\u97F3\u3001ROM\u3001\u97F3\u4E50\u89E3\u7801\u5747\u653E PSRAM", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u7528\u540E\u5373\u91CA\uFF1AHTTP \u54CD\u5E94\u7F13\u51B2\u5728 API \u8FD4\u56DE\u524D\u7ACB\u5373\u91CA\u653E\uFF0C\u4E0D\u5E38\u9A7B", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u61D2\u52A0\u8F7D\uFF1ABLE/ESP-SR \u4EC5\u5728\u8FDB\u5165\u5BF9\u5E94\u754C\u9762\u65F6\u521D\u59CB\u5316\uFF0C\u9000\u51FA\u65F6\u91CA\u653E ~100KB DRAM", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "NVS \u5199\u5165\u9632\u6296\uFF1A\u97F3\u91CF slider 500ms \u5408\u5E76\u5199\u5165\uFF0C\u907F\u514D Flash \u78E8\u635F", size: 24 })] }),
        empty(),
        h2("2.4 \u6E38\u620F\u6A21\u62DF\u5668\u65B9\u6848"),
        p("\u6700\u7EC8\u91C7\u7528 Walnut-CGB\uFF08Peanut-GB \u9AD8\u6027\u80FD\u91CD\u5199\u7248\uFF09\uFF0C\u539F\u56E0\uFF1A"),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u53CC\u6307\u4EE4\u53D6\u6784\u67B6 + 32\u4F4D DMA \u8DEF\u5F84\uFF0C\u4E13\u4E3A 32\u4F4D MCU \u4F18\u5316", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "DMG \u6A21\u5F0F\u7A33\u5B9A 60fps\uFF08CGB \u5F69\u8272\u6A21\u5F0F\u5E27\u7387\u4E0D\u8DB3\uFF0C\u56DE\u9000\u7070\u767D\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "APU \u97F3\u9891\u5408\u6210\u72EC\u7ACB\u4EFB\u52A1 Core 0\uFF0C\u4E0E\u4E3B\u4EFF\u771F Core 1 \u771F\u6B63\u5E76\u884C", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "1.5\u500D\u7F29\u653E\u6E32\u67D3\uFF08160x144 \u2192 240x216\uFF09\uFF0C\u5F02\u6B65 DMA \u53CC\u884C\u7F13\u51B2", size: 24 })] }),
        new Paragraph({ children: [new PageBreak()] }),
      ]
    },

    // ===== Chapter 3: System Principle =====
    {
      properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
      headers: { default: new Header({ children: [new Paragraph({ children: [new TextRun({ text: "ESP32-S3 AI\u667A\u80FD\u8BED\u97F3\u52A9\u624B \u8BBE\u8BA1\u62A5\u544A", size: 18, italics: true })] })] }) },
      footers: { default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER, children: [new TextRun({ text: "\u7B2C ", size: 20 }), new TextRun({ children: [PageNumber.CURRENT], size: 20 }), new TextRun({ text: " \u9875", size: 20 })] })] }) },
      children: [
        h1("3 \u7CFB\u7EDF\u5DE5\u4F5C\u539F\u7406"),
        h2("3.1 \u7CFB\u7EDF\u603B\u4F53\u67B6\u6784"),
        p("\u7CFB\u7EDF\u91C7\u7528 FreeRTOS \u591A\u4EFB\u52A1\u67B6\u6784\uFF0C\u5404\u529F\u80FD\u6A21\u5757\u4EE5\u72EC\u7ACB\u4EFB\u52A1\u8FD0\u884C\uFF0C\u901A\u8FC7 Queue\u3001Semaphore\u3001RingBuffer \u7B49\u673A\u5236\u534F\u4F5C\u3002\u4E3B\u8981\u5206\u4E3A\u4EE5\u4E0B\u5C42\u6B21\uFF1A"),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u786C\u4EF6\u9A71\u52A8\u5C42\uFF1ALCD SPI\u3001I2S \u97F3\u9891\u3001GPIO \u952E\u76D8\u3001SD \u5361 SPI", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u4E2D\u95F4\u4EF6\u5C42\uFF1ALVGL \u56FE\u5F62\u5E93\u3001ESP-SR \u8BED\u97F3\u5F15\u64CE\u3001NimBLE \u534F\u8BAE\u6808", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u5E94\u7528\u5C42\uFF1A\u5929\u6C14\u3001\u804A\u5929\u3001\u8BED\u97F3\u3001\u6E38\u620F\u3001\u97F3\u4E50\u3001\u667A\u80FD\u8BBE\u5907\u63A7\u5236", size: 24 })] }),
        empty(),
        h2("3.2 \u8BED\u97F3\u52A9\u624B\u5DE5\u4F5C\u6D41\u7A0B"),
        p("\u5728\u7EBF\u8BED\u97F3\u52A9\u624B\u7684\u5B8C\u6574\u6D41\u7A0B\uFF1A"),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u7528\u6237\u6309\u952E\u5F00\u59CB\u5F55\u97F3\uFF0CINMP441 \u901A\u8FC7 I2S \u91C7\u96C6 16kHz/16bit \u97F3\u9891", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u5F55\u97F3\u6570\u636E\u5B58\u5165 PSRAM \u7F13\u51B2\u533A\uFF08\u6700\u957F 10 \u79D2\uFF0C~320KB\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u901A\u8FC7 HTTPS POST \u53D1\u9001\u81F3\u767E\u5EA6\u8BED\u97F3\u8BC6\u522B API\uFF0C\u8FD4\u56DE\u6587\u5B57", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u8BC6\u522B\u6587\u5B57\u9001\u5165 LLM \u5927\u6A21\u578B\uFF08OpenAI \u517C\u5BB9\u63A5\u53E3\uFF09\uFF0C\u83B7\u53D6\u56DE\u590D", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u56DE\u590D\u6587\u5B57\u901A\u8FC7\u767E\u5EA6 TTS \u5408\u6210\u8BED\u97F3\uFF0C\u6D41\u5F0F\u8FD4\u56DE WAV PCM", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "PCM \u6570\u636E\u63A8\u5165 Speaker RingBuffer\uFF0CMAX98357A \u901A\u8FC7 I2S DMA \u64AD\u653E", size: 24 })] }),
        empty(),
        h2("3.3 \u6E38\u620F\u6A21\u62DF\u5668\u5DE5\u4F5C\u539F\u7406"),
        p("Game Boy \u6A21\u62DF\u5668\u91C7\u7528 Walnut-CGB \u5185\u6838\uFF0C\u5DE5\u4F5C\u6D41\u7A0B\uFF1A"),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u4ECE SD \u5361\u52A0\u8F7D ROM \u6587\u4EF6\u5230 PSRAM\uFF08\u6700\u5927 4MB\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u6682\u505C WiFi/BLE \u91CA\u653E DRAM\uFF0C\u521D\u59CB\u5316\u6A21\u62DF\u5668\u4E0A\u4E0B\u6587", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u4E3B\u5FAA\u73AF\uFF08Core 1\uFF09\uFF1A\u8BFB\u952E\u76D8 \u2192 \u6267\u884C\u4E00\u5E27\u4EFF\u771F \u2192 \u6E32\u67D3\u5230 LCD \u2192 \u5E27\u8282\u62CD\u7B49\u5F85", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "APU \u4EFB\u52A1\uFF08Core 0\uFF09\uFF1A\u6BCF\u5E27\u5408\u6210 268 \u4E2A mono \u6837\u672C\uFF0C\u63A8\u5165 Speaker", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "LCD \u6E32\u67D3\uFF1A1.5\u500D\u7F29\u653E + SPI \u5F02\u6B65 DMA \u53CC\u884C\u7F13\u51B2\uFF0C\u6BCF 2 \u884C\u6279\u91CF\u4F20\u8F93", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u4E2D\u95F4\u952E\u957F\u6309 800ms \u9000\u51FA\uFF0C\u6062\u590D WiFi/BLE\u3001\u8FD8\u539F\u91C7\u6837\u7387", size: 24 })] }),
        empty(),
        h2("3.4 \u97F3\u9891\u8F93\u51FA\u539F\u7406"),
        p("\u97F3\u9891\u8F93\u51FA\u91C7\u7528 MAX98357A I2S D\u7C7B\u529F\u653E\uFF0C\u5173\u952E\u8BBE\u8BA1\uFF1A"),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u786C\u4EF6 MONO \u69FD\u4F4D\uFF1A\u65E0\u9700\u8F6F\u4EF6 stereo \u5C55\u5F00\uFF0C\u6570\u636E\u91CF\u51CF\u534A", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "DMA auto_clear\uFF1A\u65E0\u6570\u636E\u65F6\u81EA\u52A8\u8F93\u51FA\u96F6\u7535\u5E73\uFF0C\u6D88\u9664\u7A7A\u95F2\u566A\u58F0", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "DC-block \u9AD8\u901A\u6EE4\u6CE2\uFF08\u622A\u6B62 12.7Hz\uFF09\uFF1A\u6D88\u9664\u529F\u653E\u76F4\u6D41\u504F\u7F6E\u5E95\u566A", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u5B57\u8282\u5C3E\u5BF9\u9F50\uFF1A\u4FDD\u7559\u5947\u6570\u5B57\u8282\u5230\u4E0B\u6B21\u62FC\u5408\uFF0C\u9632\u6B62 16bit \u6837\u672C\u9519\u4F4D", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u7EBF\u6027\u6DE1\u5165\uFF0864\u6837\u672C/4ms\uFF09\uFF1A\u6D88\u9664\u64AD\u653E\u542F\u52A8\u7206\u7834\u58F0", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "Q15 \u5B9A\u70B9\u5BF9\u6570\u97F3\u91CF\u63A7\u5236\uFF1Avolatile \u539F\u5B50\u5199\u5165\uFF0C\u64AD\u653E\u8DEF\u5F84\u65E0\u9501", size: 24 })] }),
        new Paragraph({ children: [new PageBreak()] }),
      ]
    },

    // ===== Chapter 4: Circuit =====
    {
      properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
      headers: { default: new Header({ children: [new Paragraph({ children: [new TextRun({ text: "ESP32-S3 AI\u667A\u80FD\u8BED\u97F3\u52A9\u624B \u8BBE\u8BA1\u62A5\u544A", size: 18, italics: true })] })] }) },
      footers: { default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER, children: [new TextRun({ text: "\u7B2C ", size: 20 }), new TextRun({ children: [PageNumber.CURRENT], size: 20 }), new TextRun({ text: " \u9875", size: 20 })] })] }) },
      children: [
        h1("4 \u7535\u8DEF\u8BBE\u8BA1"),
        h2("4.1 \u7CFB\u7EDF\u7535\u8DEF\u6846\u56FE"),
        p("\uFF08\u6B64\u5904\u63D2\u5165\u7CFB\u7EDF\u7535\u8DEF\u6846\u56FE\uFF09"),
        empty(), empty(), empty(), empty(), empty(),
        h2("4.2 \u5F15\u811A\u5206\u914D\u8868"),
        makeTable(
          ["\u529F\u80FD\u6A21\u5757", "\u4FE1\u53F7", "GPIO \u5F15\u811A", "\u8BF4\u660E"],
          [
            ["LCD (SPI2)", "MOSI", "GPIO11", "SPI \u6570\u636E"],
            ["LCD (SPI2)", "SCLK", "GPIO12", "SPI \u65F6\u949F 80MHz"],
            ["LCD (SPI2)", "RES", "GPIO10", "\u590D\u4F4D"],
            ["LCD (SPI2)", "DC", "GPIO9", "\u6570\u636E/\u547D\u4EE4"],
            ["LCD (SPI2)", "BLK", "GPIO46", "\u80CC\u5149"],
            ["INMP441 (I2S0)", "BCLK", "GPIO38", "\u4F4D\u65F6\u949F"],
            ["INMP441 (I2S0)", "WS", "GPIO39", "\u5B57\u9009\u62E9"],
            ["INMP441 (I2S0)", "SD", "GPIO40", "\u6570\u636E\u8F93\u5165"],
            ["MAX98357A (I2S1)", "BCLK", "GPIO15", "\u4F4D\u65F6\u949F"],
            ["MAX98357A (I2S1)", "LRC", "GPIO16", "\u5DE6\u53F3\u58F0\u9053"],
            ["MAX98357A (I2S1)", "DIN", "GPIO17", "\u6570\u636E\u8F93\u51FA"],
            ["\u7F16\u7801\u5668", "A", "GPIO4", "PCNT \u786C\u4EF6\u89E3\u7801"],
            ["\u7F16\u7801\u5668", "B", "GPIO5", "PCNT \u786C\u4EF6\u89E3\u7801"],
            ["\u7F16\u7801\u5668", "SW", "GPIO6", "\u6309\u952E"],
            ["SD\u5361 (SPI3)", "CS", "GPIO0", "\u7247\u9009"],
            ["SD\u5361 (SPI3)", "MOSI", "GPIO8", "\u6570\u636E\u8F93\u51FA"],
            ["SD\u5361 (SPI3)", "SCK", "GPIO18", "\u65F6\u949F"],
            ["SD\u5361 (SPI3)", "MISO", "GPIO21", "\u6570\u636E\u8F93\u5165"],
            ["\u77E9\u9635\u952E\u76D8 \u884C", "R0/R1/R2", "GPIO1/2/14", "\u626B\u63CF\u9A71\u52A8"],
            ["\u77E9\u9635\u952E\u76D8 \u5217", "C0/C1/C2", "GPIO41/42/47", "\u4E0A\u62C9\u8BFB\u53D6"],
          ],
          [1800, 1200, 1800, 4226]
        ),
        empty(),
        h2("4.3 \u7535\u8DEF\u539F\u7406\u56FE"),
        p("\uFF08\u6B64\u5904\u63D2\u5165\u5B8C\u6574\u7535\u8DEF\u539F\u7406\u56FE\uFF09"),
        empty(), empty(), empty(), empty(), empty(),
        new Paragraph({ children: [new PageBreak()] }),
      ]
    },

    // ===== Chapter 5: Software Flow =====
    {
      properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
      headers: { default: new Header({ children: [new Paragraph({ children: [new TextRun({ text: "ESP32-S3 AI\u667A\u80FD\u8BED\u97F3\u52A9\u624B \u8BBE\u8BA1\u62A5\u544A", size: 18, italics: true })] })] }) },
      footers: { default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER, children: [new TextRun({ text: "\u7B2C ", size: 20 }), new TextRun({ children: [PageNumber.CURRENT], size: 20 }), new TextRun({ text: " \u9875", size: 20 })] })] }) },
      children: [
        h1("5 \u8F6F\u4EF6\u6D41\u7A0B"),
        h2("5.1 \u7CFB\u7EDF\u542F\u52A8\u6D41\u7A0B"),
        p("app_main \u542F\u52A8\u987A\u5E8F\uFF1A"),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "NVS Flash \u521D\u59CB\u5316\uFF08\u5B58\u50A8 WiFi \u51ED\u8BC1\u3001\u97F3\u91CF\u8BBE\u7F6E\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "PSRAM \u4EFB\u52A1\u7BA1\u7406\u5668\u521D\u59CB\u5316\uFF08cleaner \u4EFB\u52A1\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "Speaker I2S \u521D\u59CB\u5316\uFF08RingBuffer + DMA + \u64AD\u653E\u4EFB\u52A1\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u5404\u6A21\u5757 mutex \u521D\u59CB\u5316\uFF08bemfa/baidu_token/model/weather/music\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "LCD \u786C\u4EF6\u521D\u59CB\u5316\uFF08SPI2 80MHz\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "SD \u5361\u6302\u8F7D\uFF08SPI3\uFF0CFAT32\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u77E9\u9635\u952E\u76D8\u521D\u59CB\u5316\uFF082ms \u626B\u63CF\u4EFB\u52A1\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "LVGL \u521D\u59CB\u5316\uFF08\u663E\u793A\u9A71\u52A8 + \u8F93\u5165\u8BBE\u5907 + tick \u4EFB\u52A1 + \u4E3B\u4EFB\u52A1\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u5065\u5EB7\u76D1\u63A7\u4EFB\u52A1\u542F\u52A8\uFF0860s \u5468\u671F\u6253\u5370\u5806\u6C34\u4F4D\uFF09", size: 24 })] }),
        empty(),
        h2("5.2 \u4E3B\u83DC\u5355\u4EA4\u4E92\u6D41\u7A0B"),
        p("LVGL \u4E3B\u4EFB\u52A1\u8FD0\u884C\u83DC\u5355\u754C\u9762\uFF0C\u7528\u6237\u901A\u8FC7\u65CB\u8F6C\u7F16\u7801\u5668\u4E0A\u4E0B\u6EDA\u52A8\u9009\u62E9\u529F\u80FD\u9879\uFF0C\u6309\u4E0B\u786E\u8BA4\u8FDB\u5165\u5BF9\u5E94\u529F\u80FD\u9875\u9762\u3002\u5404\u529F\u80FD\u9875\u9762\u7684\u7F51\u7EDC\u8BF7\u6C42\u5747\u901A\u8FC7 PSRAM \u540E\u53F0\u4EFB\u52A1\u6267\u884C\uFF0C\u901A\u8FC7 FreeRTOS Queue \u4F20\u9012\u7ED3\u679C\uFF0Clv_timer \u56DE\u8C03\u8F6E\u8BE2\u66F4\u65B0 UI\uFF0C\u4FDD\u8BC1 LVGL \u7EBF\u7A0B\u96F6\u963B\u585E\u3002"),
        empty(),
        h2("5.3 \u591A\u4EFB\u52A1\u534F\u4F5C\u6A21\u578B"),
        makeTable(
          ["\u4EFB\u52A1\u540D", "\u4F18\u5148\u7EA7", "\u6838\u5FC3", "\u529F\u80FD"],
          [
            ["lv_tick", "5", "\u4EFB\u610F", "LVGL 5ms \u65F6\u949F"],
            ["lv_task", "4", "\u4EFB\u610F", "LVGL \u6E32\u67D3\u4E3B\u5FAA\u73AF"],
            ["spk_tx", "3", "\u4EFB\u610F", "RingBuffer \u2192 I2S DMA"],
            ["asr_rec", "5", "\u4EFB\u610F", "I2S \u5F55\u97F3\u5E38\u9A7B"],
            ["kpad_task", "6", "\u4EFB\u610F", "\u77E9\u9635\u952E\u76D8 2ms \u626B\u63CF"],
            ["wifi_guard", "4", "\u4EFB\u610F", "\u65AD\u7EBF\u91CD\u8FDE\u5B88\u62A4"],
            ["game_run", "10", "Core 1", "GB \u6A21\u62DF\u5668\u4E3B\u5FAA\u73AF"],
            ["apu_task", "5", "Core 0", "GB \u97F3\u9891\u5408\u6210"],
            ["sr_detect", "5", "Core 1", "ESP-SR \u547D\u4EE4\u68C0\u6D4B"],
          ],
          [2000, 1200, 1200, 4626]
        ),
        empty(),
        h2("5.4 I2S \u8D44\u6E90\u5171\u4EAB\u673A\u5236"),
        p("I2S_NUM_0 \u88AB\u5728\u7EBF ASR \u548C\u79BB\u7EBF ESP-SR \u5171\u4EAB\uFF0C\u901A\u8FC7\u5B89\u5168\u4EA4\u63A5\u534F\u8BAE\u5207\u6362\uFF1A"),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "asr_mic_deinit()\uFF1A\u7F6E s_rec_active=false \u2192 \u7B49\u5F55\u97F3\u4EFB\u52A1\u786E\u8BA4\u9000\u51FA \u2192 i2s_del_channel", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "sr_i2s_init()\uFF1A\u91CD\u65B0\u521B\u5EFA I2S \u901A\u9053\uFF0816bit mono \u914D\u7F6E\uFF09", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "ESP-SR \u505C\u6B62\u540E\uFF1Asr_i2s_deinit() \u2192 asr_mic_reinit() \u6062\u590D\u539F\u914D\u7F6E", size: 24 })] }),
        p("\u8FD9\u907F\u514D\u4E86\u5728 i2s_channel_read \u963B\u585E\u671F\u95F4\u91CA\u653E\u5E95\u5C42\u8D44\u6E90\u5BFC\u81F4\u7684 HardFault\u3002"),
        new Paragraph({ children: [new PageBreak()] }),
      ]
    },

    // ===== Chapter 6: Bug Analysis =====
    {
      properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
      headers: { default: new Header({ children: [new Paragraph({ children: [new TextRun({ text: "ESP32-S3 AI\u667A\u80FD\u8BED\u97F3\u52A9\u624B \u8BBE\u8BA1\u62A5\u544A", size: 18, italics: true })] })] }) },
      footers: { default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER, children: [new TextRun({ text: "\u7B2C ", size: 20 }), new TextRun({ children: [PageNumber.CURRENT], size: 20 }), new TextRun({ text: " \u9875", size: 20 })] })] }) },
      children: [
        h1("6 \u5173\u952E Bug \u5206\u6790\u4E0E\u4FEE\u590D"),
        p("\u9879\u76EE\u5F00\u53D1\u8FC7\u7A0B\u4E2D\u9047\u5230\u5E76\u4FEE\u590D\u4E86\u591A\u4E2A\u5173\u952E Bug\uFF0C\u4EE5\u4E0B\u603B\u7ED3\u6700\u5177\u4EE3\u8868\u6027\u7684\u95EE\u9898\uFF1A"),
        empty(),
        h2("6.1 WiFi \u8FDE\u63A5\u65F6\u5F00\u542F\u84DD\u7259\u5D29\u6E83"),
        p("\u73B0\u8C61\uFF1A\u5F00\u542F BLE \u914D\u7F51\u65F6\uFF0C\u5982\u679C WiFi \u5DF2\u8FDE\u63A5\uFF0C\u7CFB\u7EDF\u7ACB\u5373\u5D29\u6E83\u3002", { bold: true }),
        p("\u539F\u56E0\uFF1ABLE controller \u9700\u8981\u5927\u5757\u8FDE\u7EED DRAM\uFF0CWiFi \u9A71\u52A8\u5360\u7528\u4E86\u5927\u90E8\u5206\u5185\u90E8 RAM\uFF0C\u5BFC\u81F4 NimBLE \u521D\u59CB\u5316\u65F6\u5185\u5B58\u5206\u914D\u5931\u8D25\u3002"),
        p("\u4FEE\u590D\uFF1A\u65B0\u589E wifi_full_shutdown_for_ble()\uFF0C\u5F00\u542F BLE \u524D\u5B8C\u6574\u91CA\u653E WiFi \u9A71\u52A8\u6808\uFF08\u542B esp_wifi_deinit\uFF09\uFF0C\u817E\u51FA ~30KB DRAM\u3002\u914D\u7F51\u5B8C\u6210\u540E\u91CD\u65B0 wifi_connect \u8D70\u5B8C\u6574 init \u6D41\u7A0B\u3002"),
        empty(),
        h2("6.2 \u5B88\u62A4\u4EFB\u52A1\u786C\u6740\u5BFC\u81F4 Mutex \u6B7B\u9501"),
        p("\u73B0\u8C61\uFF1Awifi_full_shutdown_for_ble \u4E2D\u4F7F\u7528 vTaskDelete \u786C\u6740\u5B88\u62A4\u4EFB\u52A1\uFF0C\u5076\u53D1\u6B7B\u9501\u3002", { bold: true }),
        p("\u539F\u56E0\uFF1A\u5B88\u62A4\u4EFB\u52A1\u5185\u90E8\u6709 WIFI_LOCK/WIFI_UNLOCK \u8C03\u7528\uFF0C\u5982\u679C\u6070\u597D\u5728\u6301\u6709 mutex \u65F6\u88AB vTaskDelete\uFF0Cmutex \u6C38\u4E45\u88AB\u5360\u7528\uFF0C\u540E\u7EED\u6240\u6709 WiFi \u64CD\u4F5C\u6B7B\u9501\u3002"),
        p("\u4FEE\u590D\uFF1A\u6539\u4E3A\u534F\u4F5C\u5F0F\u9000\u51FA\u2014\u2014\u53D1\u9001 GUARDIAN_NOTIFY_SHUTDOWN \u901A\u77E5\u8BA9\u4EFB\u52A1\u81EA\u884C\u9000\u51FA\uFF0C\u7B49\u5F85\u786E\u8BA4\u540E\u518D\u7EE7\u7EED\u3002\u9000\u907F\u7B49\u5F85\u6539\u4E3A 100ms \u5206\u6BB5\u8F6E\u8BE2\uFF0C\u4FBF\u4E8E\u53CA\u65F6\u54CD\u5E94 shutdown\u3002"),
        empty(),
        h2("6.3 \u767E\u5EA6 Token \u6307\u9488\u60AC\u7A7A"),
        p("\u73B0\u8C61\uFF1AASR/TTS \u5076\u53D1\u8BF7\u6C42\u5931\u8D25\u6216\u4F7F\u7528\u9519\u8BEF\u7684 token\u3002", { bold: true }),
        p("\u539F\u56E0\uFF1Abaidu_token_get() \u8FD4\u56DE\u5185\u90E8 static \u7F13\u51B2\u533A\u6307\u9488\uFF0C\u91CA\u653E\u9501\u540E\u53E6\u4E00\u4E2A\u7EBF\u7A0B\u53EF\u80FD\u8C03\u7528 baidu_token_invalidate() \u6E05\u7A7A\u6216 fetch_token_locked() \u8986\u5199\u8BE5\u7F13\u51B2\u533A\uFF0C\u8C03\u7528\u65B9\u62FF\u5230\u7684\u6307\u9488\u6307\u5411\u5DF2\u88AB\u4FEE\u6539\u7684\u5185\u5BB9\u3002"),
        p("\u4FEE\u590D\uFF1A\u65B0\u589E baidu_token_copy() \u63A5\u53E3\uFF0C\u5728\u6301\u6709 mutex \u671F\u95F4\u5C06 token \u62F7\u8D1D\u5230\u8C03\u7528\u65B9\u7684\u6808\u7F13\u51B2\u533A\uFF0C\u91CA\u653E\u9501\u540E\u4F7F\u7528\u672C\u5730\u526F\u672C\uFF0C\u5F7B\u5E95\u6D88\u9664\u8DE8\u7EBF\u7A0B\u6307\u9488\u60AC\u7A7A\u98CE\u9669\u3002"),
        empty(),
        h2("6.4 \u65E0 WiFi \u65F6\u8BED\u97F3\u5BF9\u8BDD\u5D29\u6E83"),
        p("\u73B0\u8C61\uFF1A\u672A\u8FDE\u63A5 WiFi \u65F6\u5F00\u542F\u8BED\u97F3\u52A9\u624B\uFF0C\u7CFB\u7EDF panic\u3002", { bold: true }),
        p("\u539F\u56E0\uFF1AWiFi \u672A\u8FDE\u63A5\u65F6 lwIP tcpip \u961F\u5217\u65E0\u6548\uFF0C\u76F4\u63A5\u53D1\u8D77 HTTP \u8BF7\u6C42\u4F1A\u89E6\u53D1 assert failed: Invalid mbox\u3002"),
        p("\u4FEE\u590D\uFF1Aasr_recognize() \u5165\u53E3\u589E\u52A0 wifi_get_status() \u5B88\u536B\u68C0\u67E5\uFF0CWiFi \u672A\u8FDE\u63A5\u65F6\u76F4\u63A5\u8FD4\u56DE\u9519\u8BEF\u4FE1\u606F\uFF0C\u4E0D\u53D1\u8D77\u7F51\u7EDC\u8BF7\u6C42\u3002"),
        empty(),
        h2("6.5 \u6E38\u620F\u4E0E WiFi \u5185\u5B58\u51B2\u7A81\u5D29\u6E83"),
        p("\u73B0\u8C61\uFF1A\u8FDE\u63A5 WiFi \u540E\u8FDB\u5165\u6E38\u620F\uFF0C\u8FD0\u884C\u51E0\u79D2\u540E\u5D29\u6E83\u3002", { bold: true }),
        p("\u539F\u56E0\uFF1AWiFi \u9A71\u52A8\u5360\u7528 ~100KB \u5185\u90E8 DRAM\uFF0C\u6E38\u620F\u6A21\u62DF\u5668\u4EFB\u52A1\u6808 + LVGL \u7F13\u51B2\u5269\u4F59\u5185\u5B58\u4E0D\u8DB3\uFF0C\u5BFC\u81F4\u6808\u6EA2\u51FA\u3002"),
        p("\u4FEE\u590D\uFF1A\u8FDB\u5165\u6E38\u620F\u524D\u8C03\u7528 wifi_suspend_for_game() \u6682\u505C WiFi\uFF08\u91CA\u653E\u7F13\u51B2\u6C60\uFF09\uFF0C\u9000\u51FA\u540E wifi_resume_after_game() \u6062\u590D\u3002\u540C\u65F6\u5C06\u6E38\u620F\u4EFB\u52A1\u6808\u653E\u5728\u5185\u90E8 DRAM\uFF08\u800C\u975E PSRAM\uFF09\uFF0C\u907F\u514D\u5173 cache \u65F6 PSRAM \u4E0D\u53EF\u8BBF\u95EE\u3002"),
        empty(),
        h2("6.6 \u97F3\u9891\u6742\u97F3\u4E0E\u5361\u5572\u58F0"),
        p("\u73B0\u8C61\uFF1ATTS \u64AD\u653E\u65F6\u6709\u660E\u663E\u767D\u566A\u58F0\u548C\u5468\u671F\u6027\u5361\u5572\u3002", { bold: true }),
        p("\u539F\u56E0\uFF1A(1) HTTP chunked \u4F20\u8F93\u7ED9\u51FA\u5947\u6570\u5B57\u8282\uFF0C\u5BFC\u81F4 16bit \u6837\u672C\u9519\u4F4D 1 \u5B57\u8282\uFF0C\u6574\u6BB5 PCM \u53D8\u6210\u96EA\u82B1\u767D\u566A\u58F0\uFF1B(2) \u8F6F\u4EF6 stereo \u5C55\u5F00\u653E\u5927\u4E00\u500D\u6570\u636E\u91CF\uFF0CRing \u9965\u997F\u66F4\u5FEB\uFF0C\u4E3B\u52A8\u586B\u5145\u9759\u97F3\u5BFC\u81F4\u5468\u671F\u6027\u5361\u5572\u3002"),
        p("\u4FEE\u590D\uFF1A(1) \u5B57\u8282\u5C3E\u5BF9\u9F50\u673A\u5236\u4FDD\u7559\u5947\u6570\u5B57\u8282\u5230\u4E0B\u6B21\u62FC\u5408\uFF1B(2) \u6539\u7528\u786C\u4EF6 MONO \u69FD + DMA auto_clear\uFF0C\u53BB\u6389\u4E3B\u52A8\u586B\u5145\u9759\u97F3\u903B\u8F91\uFF1B(3) \u589E\u52A0 DC-block HPF \u6D88\u9664\u76F4\u6D41\u504F\u7F6E\u5E95\u566A\u3002"),
        empty(),
        h2("6.7 \u6A21\u5757 Mutex \u5E76\u53D1\u521D\u59CB\u5316\u7ADE\u6001"),
        p("\u73B0\u8C61\uFF1A\u6781\u5C11\u60C5\u51B5\u4E0B\u591A\u4E2A\u6A21\u5757\u7684 mutex \u53EF\u80FD\u88AB\u521B\u5EFA\u4E24\u6B21\uFF08\u6CC4\u6F0F\u4E00\u4E2A\uFF09\u3002", { bold: true }),
        p("\u539F\u56E0\uFF1Amodel/weather/music \u6A21\u5757\u7684 ensure_mutex() \u91C7\u7528 lazy-init\uFF0C\u5982\u679C\u4E24\u4E2A\u4EFB\u52A1\u540C\u65F6\u9996\u6B21\u8C03\u7528\uFF0C\u53EF\u80FD\u5404\u521B\u5EFA\u4E00\u4E2A mutex\u3002"),
        p("\u4FEE\u590D\uFF1A\u4E3A\u6BCF\u4E2A\u6A21\u5757\u65B0\u589E _init() \u51FD\u6570\uFF0C\u5728 app_main \u5355\u7EBF\u7A0B\u9636\u6BB5\u63D0\u524D\u521B\u5EFA mutex\uFF0C\u6D88\u9664\u5E76\u53D1\u7A97\u53E3\u3002"),
        new Paragraph({ children: [new PageBreak()] }),
      ]
    },

    // ===== Chapter 7: Summary =====
    {
      properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
      headers: { default: new Header({ children: [new Paragraph({ children: [new TextRun({ text: "ESP32-S3 AI\u667A\u80FD\u8BED\u97F3\u52A9\u624B \u8BBE\u8BA1\u62A5\u544A", size: 18, italics: true })] })] }) },
      footers: { default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER, children: [new TextRun({ text: "\u7B2C ", size: 20 }), new TextRun({ children: [PageNumber.CURRENT], size: 20 }), new TextRun({ text: " \u9875", size: 20 })] })] }) },
      children: [
        h1("7 \u603B\u7ED3"),
        h2("7.1 \u5B9E\u73B0\u6210\u679C"),
        p("\u672C\u9879\u76EE\u6210\u529F\u5B9E\u73B0\u4E86\u4E00\u6B3E\u57FA\u4E8E ESP32-S3 \u7684\u591A\u529F\u80FD\u667A\u80FD\u7EC8\u7AEF\uFF0C\u4E3B\u8981\u6280\u672F\u6307\u6807\uFF1A"),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u6E38\u620F\u6A21\u62DF\u5668\u7A33\u5B9A 60fps\uFF0C\u5E26\u5B8C\u6574 4 \u58F0\u9053\u97F3\u9891\u8F93\u51FA", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u8BED\u97F3\u52A9\u624B\u5B8C\u6574\u94FE\u8DEF\uFF08\u5F55\u97F3\u2192\u8BC6\u522B\u2192\u5BF9\u8BDD\u2192\u5408\u6210\uFF09\u5EF6\u8FDF < 5\u79D2", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u79BB\u7EBF\u8BED\u97F3\u547D\u4EE4\u8BC6\u522B 11 \u4E2A\u4E2D\u6587\u6307\u4EE4\uFF0C\u65E0\u9700\u7F51\u7EDC", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "SD \u5361\u97F3\u4E50\u64AD\u653E\u652F\u6301 WAV/MP3\uFF0C\u52A8\u6001\u91C7\u6837\u7387\u5207\u6362 8k-48kHz", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "BLE \u914D\u7F51 + \u8FDC\u7A0B\u97F3\u4E50\u63A7\u5236\uFF0C\u624B\u673A\u5373\u53EF\u64CD\u4F5C", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u5DF4\u6CD5\u4E91\u667A\u80FD\u8BBE\u5907\u63A7\u5236\uFF0C\u4E00\u952E\u5F00\u5173", size: 24 })] }),
        new Paragraph({ numbering: { reference: "bullets", level: 0 }, children: [new TextRun({ text: "\u7CFB\u7EDF\u7A33\u5B9A\u8FD0\u884C\uFF0C\u5185\u5B58\u65E0\u6CC4\u6F0F\uFF0C\u5065\u5EB7\u76D1\u63A7 60s \u5468\u671F\u62A5\u544A", size: 24 })] }),
        empty(),
        h2("7.2 \u5F00\u53D1\u7ECF\u9A8C\u603B\u7ED3"),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u5185\u5B58\u7BA1\u7406\u662F\u5D4C\u5165\u5F0F\u9879\u76EE\u7684\u6838\u5FC3\u6311\u6218\uFF0C\u5FC5\u987B\u4ECE\u67B6\u6784\u5C42\u9762\u89C4\u5212 DRAM/PSRAM \u5206\u914D\u7B56\u7565", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u591A\u4EFB\u52A1\u5E76\u53D1\u5FC5\u987B\u4E25\u683C\u9075\u5B88\u9501\u7684\u83B7\u53D6\u987A\u5E8F\uFF0C\u907F\u514D\u6B7B\u9501\uFF1B\u4F18\u5148\u4F7F\u7528\u534F\u4F5C\u5F0F\u9000\u51FA\u800C\u975E\u786C\u6740", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u97F3\u9891\u7CFB\u7EDF\u5BF9\u5B57\u8282\u5BF9\u9F50\u3001\u65F6\u5E8F\u3001\u76F4\u6D41\u504F\u7F6E\u6781\u5176\u654F\u611F\uFF0C\u9700\u8981\u591A\u5C42\u9632\u62A4", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u6A21\u5757\u521D\u59CB\u5316\u5E94\u5728\u5355\u7EBF\u7A0B\u9636\u6BB5\u5B8C\u6210\uFF0C\u6D88\u9664 lazy-init \u7ADE\u6001\u7A97\u53E3", size: 24 })] }),
        new Paragraph({ numbering: { reference: "numbers", level: 0 }, children: [new TextRun({ text: "\u8DE8\u7EBF\u7A0B\u5171\u4EAB\u6307\u9488\u5FC5\u987B\u5728\u9501\u5185\u62F7\u8D1D\u526F\u672C\uFF0C\u4E0D\u80FD\u4F9D\u8D56\u91CA\u653E\u9501\u540E\u6307\u9488\u4ECD\u6709\u6548", size: 24 })] }),
        empty(),
        h2("7.3 \u4EE3\u7801\u7EDF\u8BA1"),
        makeTable(
          ["\u7C7B\u522B", "\u884C\u6570"],
          [
            ["\u7EAF\u624B\u5199\u4EE3\u7801", "~6,400 \u884C"],
            ["\u5B57\u4F53\u6570\u636E", "~385,000 \u884C"],
            ["\u6A21\u62DF\u5668\u5E93", "~10,500 \u884C"],
            ["LVGL \u5E93", "\u672A\u8BA1\u5165"],
            ["Git \u63D0\u4EA4\u6B21\u6570", "80 \u6B21"],
          ],
          [5000, 4026]
        ),
      ]
    },
  ]
});

// Generate the file
Packer.toBuffer(doc).then(buffer => {
  fs.writeFileSync("/home/aliceykr/WORKSPACE/bot/design_report.docx", buffer);
  console.log("design_report.docx generated successfully!");
}).catch(err => {
  console.error("Error:", err);
  process.exit(1);
});
