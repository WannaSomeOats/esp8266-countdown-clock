/* ============================================================================
 *  DisplayTest.ino —— 双层 ESP8266 倒数日
 *  屏幕 + 按键 联测程序（第一版点屏验证）
 * ----------------------------------------------------------------------------
 *  硬件接线（NodeMCU 丝印）：
 *    ST7735 屏： CS->D8  DC->D3  RST->D0  MOSI->D7  SCLK->D5  BLK->3.3V
 *    按键：      A -> D6 (GPIO12)      B -> D4 (GPIO2)    按下接地
 *
 *  本程序做什么：
 *    1. 开机先全屏快闪 红/绿/蓝/白/黑 —— 一眼判断屏是否点亮、有无坏区
 *    2. 彩条页  ：8 根纯色竖条（验证颜色对不对）
 *    3. 灰阶页  ：8 级灰阶横条（侧看不变序 = IPS；顺序颠倒 = TN）
 *    4. 边框页  ：四角定位块 + 边框 + 文字（验证偏移量和字体）
 *    5. 按键页  ：实时显示 A/B 电平、短按计数、长按进度条
 *
 *  操作方式：
 *    未按过任何键时，页面每 3.5 秒自动轮播（0->1->2）
 *    A 短按 = 上一页    B 短按 = 下一页
 *    A 长按 = 回彩条页  B 长按 = 进按键页（长按进度条会走满 0.8 秒）
 *
 *  串口 115200（只用于看日志，不需要输入）
 * ==========================================================================*/

#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

/* ---------------------------- 按键配置 ----------------------------------- */

const uint8_t  KEY_A_PIN   = 12;   // D6
const uint8_t  KEY_B_PIN   = 2;    // D4
const uint16_t DEBOUNCE_MS = 25;   // 消抖
const uint16_t LONG_MS     = 800;  // 长按阈值（与交互设计一致）

struct Key {
  const char *name;
  uint8_t  pin;
  bool     stable;        // 消抖后的状态，true = 按下
  bool     lastRaw;       // 上次原始读数
  uint32_t lastChange;    // 原始电平变化时间（消抖用）
  uint32_t pressStart;    // 按下开始时间（长按计时用）
  bool     longFired;     // 本次按压是否已触发长按
  uint16_t shortCnt;      // 短按计数
  uint16_t longCnt;       // 长按计数
};

Key keyA = { "A", KEY_A_PIN, false, false, 0, 0, false, 0, 0 };
Key keyB = { "B", KEY_B_PIN, false, false, 0, 0, false, 0, 0 };

// 事件返回值
enum { EV_NONE = 0, EV_SHORT = 1, EV_LONG = 2 };

/* ---------------------------- 页面管理 ----------------------------------- */

const uint8_t PAGE_COUNT = 4;
uint8_t  page       = 0;
bool     anyKeyEver = false;        // 一旦按过键，停止自动轮播
uint32_t lastAuto   = 0;
const uint32_t AUTO_MS = 3500;

/* ============================ 页面绘制 =================================== */

/* 页 0：彩条 */
void drawColorBars() {
  const uint16_t cols[8] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_CYAN,
                             TFT_MAGENTA, TFT_YELLOW, TFT_WHITE, TFT_BLACK };
  const char *nm[8]      = { "R", "G", "B", "C", "M", "Y", "W", "K" };
  tft.fillScreen(TFT_BLACK);
  for (uint8_t i = 0; i < 8; i++) {
    tft.fillRect(i * 16, 0, 16, 160, cols[i]);
    // 亮色底用黑字、暗色底用白字
    bool dark = (i == 0 || i == 2 || i == 4);
    tft.setTextColor(dark ? TFT_WHITE : TFT_BLACK, cols[i]);
    tft.setTextFont(1);
    tft.setCursor(i * 16 + 5, 148);
    tft.print(nm[i]);
  }
}

/* 页 1：灰阶（IPS/TN 判定页） */
void drawGrayscale() {
  tft.fillScreen(TFT_BLACK);
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t g = i * 36;                      // 0,36,...,252
    uint16_t c = tft.color565(g, g, g);
    tft.fillRect(0, i * 20, 128, 20, c);
    tft.setTextFont(1);
    tft.setTextColor(g > 128 ? TFT_BLACK : TFT_WHITE, c);
    tft.setCursor(4, i * 20 + 6);
    tft.printf("%3d", g);
  }
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(4, 152);
  tft.print("tilt:same=IPS flip=TN");        // 提示：斜看，顺序不变=IPS
}

/* 页 2：边框 + 定位块 + 文字 */
void drawFramePage() {
  tft.fillScreen(TFT_BLACK);
  tft.drawRect(0, 0, 128, 160, TFT_WHITE);

  // 四角定位块：若某块被裁掉/移位，说明显示偏移量不对
  tft.fillRect(3, 3, 8, 8, TFT_RED);       // 左上
  tft.fillRect(117, 3, 8, 8, TFT_GREEN);   // 右上
  tft.fillRect(3, 149, 8, 8, TFT_BLUE);    // 左下
  tft.fillRect(117, 149, 8, 8, TFT_YELLOW);// 右下

  // 中心十字
  tft.drawFastHLine(44, 80, 40, TFT_DARKGREY);
  tft.drawFastVLine(64, 60, 40, TFT_DARKGREY);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.setTextFont(2);
  tft.drawString("128x160", 64, 38);

  tft.setTextFont(1);
  tft.drawString("ABCDEFGHIJKLMNOPQRST", 64, 60);   // 20 字符 = 120px
  tft.drawString("abcdefghijklmnopqrst", 64, 72);
  tft.drawString("0123456789 : . - +", 64, 84);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("TL=R TR=G BL=B BR=Y", 64, 106);    // 四角色块图例
  tft.drawString("edge line broken?", 64, 118);
}

/* 页 3：按键监视 */
void drawKeyBoxStatic(uint8_t y, const Key *k) {
  tft.drawRect(4, y, 120, 50, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, y + 6);
  tft.printf("%s  GPIO%u", k->name, k->pin);
}

void drawKeyPage(bool full) {
  if (full) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(6, 4);
    tft.print("KEY TEST");
    tft.setTextFont(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(6, 24);
    tft.print("A short=prev  B short=next");
    drawKeyBoxStatic(34, &keyA);
    drawKeyBoxStatic(90, &keyB);
  }
}

// 动态部分：状态药丸 + 计数 + 长按进度条（只在变化时重绘，避免闪烁）
void updateKeyBox(uint8_t y, const Key *k, uint32_t now) {
  // --- 状态药丸 ---
  static bool pillA = false, pillB = false;
  bool *pill = (k->pin == KEY_A_PIN) ? &pillA : &pillB;
  if (k->stable != *pill) {
    *pill = k->stable;
    uint16_t bg = k->stable ? TFT_GREEN : TFT_DARKGREY;
    tft.fillRect(84, y + 6, 34, 16, bg);
    tft.setTextDatum(ML_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(k->stable ? TFT_BLACK : TFT_WHITE, bg);
    tft.setCursor(88, y + 14);
    tft.print(k->stable ? "DOWN" : " UP ");
  }

  // --- 计数行 ---
  static uint16_t lastS[2] = { 0xFFFF, 0xFFFF };
  static uint16_t lastL[2] = { 0xFFFF, 0xFFFF };
  uint8_t idx = (k->pin == KEY_A_PIN) ? 0 : 1;
  if (k->shortCnt != lastS[idx] || k->longCnt != lastL[idx]) {
    lastS[idx] = k->shortCnt;
    lastL[idx] = k->longCnt;
    tft.fillRect(10, y + 26, 110, 10, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(10, y + 27);
    tft.printf("short:%u  long:%u", k->shortCnt, k->longCnt);
  }

  // --- 长按进度条（按住时走满 0.8s 后触发长按）---
  static int8_t progA = -1, progB = -1;
  int8_t *prog = (k->pin == KEY_A_PIN) ? &progA : &progB;
  int8_t w = -1;
  if (k->stable && !k->longFired) {
    uint32_t dur = now - k->pressStart;
    w = (int8_t)((dur * 100UL) / LONG_MS);
    if (w > 100) w = 100;
  } else {
    w = 0;
  }
  if (w != *prog) {
    // 清底后画填充（宽度 0~100 映射到 108px）
    tft.fillRect(10, y + 40, 108, 6, TFT_DARKGREY);
    if (w > 0) tft.fillRect(10, y + 40, (w * 108) / 100, 6, TFT_ORANGE);
    *prog = w;
  }
}

void drawPage(uint8_t p) {
  switch (p) {
    case 0: drawColorBars();  break;
    case 1: drawGrayscale();  break;
    case 2: drawFramePage();  break;
    case 3: drawKeyPage(true); break;
  }
}

/* ============================ 按键扫描 =================================== */

// 返回 EV_NONE / EV_SHORT / EV_LONG（长按在按住期间触发一次，松手不再计短按）
uint8_t scanKey(Key *k, uint32_t now) {
  bool raw = (digitalRead(k->pin) == LOW);   // 内部上拉：按下 = LOW

  if (raw != k->lastRaw) {                   // 原始电平变化 → 重新计时
    k->lastRaw    = raw;
    k->lastChange = now;
    return EV_NONE;
  }

  if (now - k->lastChange < DEBOUNCE_MS) return EV_NONE;  // 还没稳定

  if (raw != k->stable) {                    // 消抖后确认变化
    k->stable = raw;
    if (raw) {                               // 按下
      k->pressStart = now;
      k->longFired  = false;
    } else {                                 // 松手
      if (!k->longFired) {                   // 没触发过长按 → 记短按
        k->shortCnt++;
        return EV_SHORT;
      }
    }
    return EV_NONE;
  }

  if (k->stable && !k->longFired && (now - k->pressStart >= LONG_MS)) {
    k->longFired = true;
    k->longCnt++;
    return EV_LONG;
  }
  return EV_NONE;
}

/* ============================ setup / loop =============================== */

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[DisplayTest] 屏幕 + 按键 联测程序启动");

  pinMode(KEY_A_PIN, INPUT_PULLUP);
  pinMode(KEY_B_PIN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(0);                 // 竖屏 128x160
  tft.fillScreen(TFT_BLACK);

  Serial.printf("[DisplayTest] 屏宽 %d 高 %d\n", tft.width(), tft.height());

  // 开机快闪：红→绿→蓝→白→黑（一眼验证点亮与色彩）
  const uint16_t flashCols[5] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_BLACK };
  for (uint8_t i = 0; i < 5; i++) {
    tft.fillScreen(flashCols[i]);
    delay(250);
  }

  lastAuto = millis();
  drawPage(page);
}

void loop() {
  uint32_t now = millis();

  // ---- 按键 ----
  uint8_t evA = scanKey(&keyA, now);
  uint8_t evB = scanKey(&keyB, now);

  if (evA || evB) anyKeyEver = true;

  if (evA == EV_SHORT) {
    page = (page + PAGE_COUNT - 1) % PAGE_COUNT;      // A 短按 = 上一页
    drawPage(page);
    Serial.printf("[key] A short -> page %u\n", page);
  }
  if (evB == EV_SHORT) {
    page = (page + 1) % PAGE_COUNT;                   // B 短按 = 下一页
    drawPage(page);
    Serial.printf("[key] B short -> page %u\n", page);
  }
  if (evA == EV_LONG) {                               // A 长按 = 回彩条页
    page = 0;
    drawPage(page);
    Serial.println("[key] A long -> page 0");
  }
  if (evB == EV_LONG) {                               // B 长按 = 进按键页
    page = 3;
    drawPage(page);
    Serial.println("[key] B long -> page 3");
  }

  // ---- 自动轮播（仅限从没按过键时，仅 0~2 页）----
  if (!anyKeyEver && page < 3 && (now - lastAuto >= AUTO_MS)) {
    lastAuto = now;
    page = (page + 1) % 3;
    drawPage(page);
  }

  // ---- 按键页动态刷新 ----
  if (page == 3) {
    updateKeyBox(34, &keyA, now);
    updateKeyBox(90, &keyB, now);
  }

  delay(5);
}
