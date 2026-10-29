// =====================================================================
//   双层 ESP8266 倒数日项目 —— 1.8" ST7735 128x160 专用配置
//   （本文件由项目定制，被 User_Setup_Select.h 引用）
// ---------------------------------------------------------------------
//   开发板：NodeMCU / ESP-12E 类（顶盖 ESP8266MOD）
//
//   屏接线（NodeMCU 丝印）：
//     TFT_CS   -> D8 (GPIO15)
//     TFT_DC   -> D3 (GPIO0)
//     TFT_RST  -> D0 (GPIO16)
//     TFT_MOSI -> D7 (GPIO13)
//     TFT_SCLK -> D5 (GPIO14)
//     TFT_MISO 不接（屏无 MISO 脚，8 针模块是"只写"屏）
//     BLK      -> 3.3V（常亮，不占引脚）
//
//   按键（与本文件无关，记录备查）：
//     A -> D6 (GPIO12)      B -> D4 (GPIO2)      均内部上拉、按下接地
// =====================================================================

// ----------------- 驱动与初始化 -----------------
#define ST7735_DRIVER
// ✅ 2026-10-15 实测定案：本模块**不吃 INITB 序列**（白屏无反应），
//    换 7735R 序列后正常显示（DisplayProbe 四档探测结论）。
//    REDTAB = Rcmd1 + Rcmd2red + Rcmd3，偏移量 (0,0)，MADCTL=0xC8。
#define ST7735_REDTAB
// 备选（一次只开一个）：
// #define ST7735_INITB
// #define ST7735_GREENTAB         // 若发现右缘/下缘有 2px/1px 杂线再换它（偏移 2,1）

// ✅ 实测定案：REDTAB 默认发 BGR 顺序，本模块实为 RGB 顺序（红蓝互换过一次），
//    显式指定 RGB 修正颜色（MADCTL 从 0xC8 变 0xC0）。
#define TFT_RGB_ORDER TFT_RGB

#define TFT_WIDTH  128
#define TFT_HEIGHT 160

// ----------------- 引脚 -------------------------
#define TFT_CS   15             // D8
#define TFT_DC   0              // D3
#define TFT_RST  16             // D0
#define TFT_MOSI 13             // D7
#define TFT_SCLK 14             // D5
#define TFT_MISO -1             // 不使用（不读屏）

// ----------------- 字体（按需精简以省 Flash）----
#define LOAD_GLCD               // 8px 原始字体（约 1.8KB）
#define LOAD_FONT2              // 16px 字体（约 3.5KB）
#define LOAD_FONT4              // 26px 字体（约 5.8KB）
#define LOAD_FONT6              // 48px 数字专用（约 2.7KB）
#define LOAD_FONT7              // 7段数码管风格 48px（约 2.4KB）
#define LOAD_FONT8              // 75px 大数字（约 3.3KB）

// ----------------- SPI 速率 ---------------------
// ⚠ 2026-10-15：白屏排查期间从 27MHz 降到 10MHz（排除杜邦线信号完整性问题）。
// 屏点亮、系统稳定后，可逐级升回 27MHz / 40MHz（焊到洞洞板后再升最稳）。
#define SPI_FREQUENCY  10000000
