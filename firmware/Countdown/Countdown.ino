/* ============================================================================
 *  Countdown.ino —— 双层 ESP8266 倒数日  v0.3（与 HTML 仿真 1:1 移植版）
 * ----------------------------------------------------------------------------
 *  本版本目标：界面布局 / 配色 / 文案 / 按键逻辑，全部严格对齐
 *             ST7735-128x160-仿真.html
 *
 *  字体：自带点阵字库（font_cn12.h 自动生成）
 *        CJK12：Fusion Pixel 12px 等宽（ASCII + 中文子集，OFL-1.1）
 *        NUM36 / NUM28 / NUM20：JetBrains Mono Bold 数字（OFL-1.1）
 *
 *  硬件：
 *    I2C : SDA=D2(4)  SCL=D1(5)      AHT30(0x38) + PCF8563(0x51)
 *    SPI : CS=D8(15) DC=D3(0) RST=D0(16) MOSI=D7(13) SCLK=D5(14)  BLK=3.3V
 *    按键: A=D6(12)   B=D4(2)   内部上拉，按下为低
 * ==========================================================================*/

#include <TFT_eSPI.h>
#include <Wire.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <qrcode.h>
#include "font_cn12.h"

// ---- 以下函数实现在 WifiConfig.ino ----
uint8_t     wifiBars();        // 真实 WiFi 信号格数 0~4（0=未连接）
const char *wifiStateText();   // 设置页「WiFi 设置」当前状态文字
void        wifiAutoConnect(); // 开机自动连接（有凭据时）
void        wifiEnterConfig(); // 进入配网模式（阻塞直到退出）

// ---- 以下函数实现在 NtpSync.ino ----
void        ntpInit();         // 初始化（注册 SNTP 校时回调，setup 调用一次）
void        ntpTick();         // 主循环推进（非阻塞）
void        ntpManualSync();   // 手动立即授时（设置页）
const char *ntpStateText();    // 设置页「网络授时」状态文字

// ---- 以下函数实现在 Weather.ino ----
void        weatherTick();         // 主循环推进（非阻塞）
void        weatherManualRefresh();// 手动立即刷新（天气页菜单）
void        weatherRefreshSoon();  // 尽快重新获取（切换城市后调用）
const char *weatherCityName();     // 当前城市名
uint8_t     weatherCityCount();    // 城市表长度
void        drawWeather();         // 天气页（含操作态菜单 / 选城市）
void        wxDrawHomeLines();     // 主页两行天气信息

TFT_eSPI tft = TFT_eSPI();

/* ============================ 配置常量 =================================== */

#define KEY_A_PIN   2           // D4（用户定案：D4 = A 键）
#define KEY_B_PIN   12          // D6（用户定案：D6 = B 键）
#define LONG_MS     800
#define DEBOUNCE_MS 25
#define IDLE_MS     30000UL

#define FW_VERSION  "v0.6.1"

#define AHT30_ADDR   0x38
#define PCF8563_ADDR 0x51

/* ============================ 数据结构 =================================== */

struct RtcTime {
  uint8_t sec, min, hour, day, weekday, month, year;
  bool    century;
  bool    vl;
};

struct Settings {
  uint32_t magic;
  uint8_t  tYear, tMonth, tDay;    // 目标日期
  uint8_t  nameIdx;                // 0=春节 1=生日
  uint8_t  pomoMin;                // 番茄分钟
  uint8_t  bright;                 // 1-5（本硬件暂不可调，仅保存）
  uint8_t  timeout;                // 背光超时秒
  uint8_t  buzzer;                 // 0/1
  uint8_t  wifiOK;                 // 是否有可用 WiFi 凭据
  char     ssid[33];               // 家庭 WiFi 名称
  char     pass[65];               // 家庭 WiFi 密码
  char     apPass[9];              // 配网热点密码（每设备固定随机 8 位）
  uint8_t  cityIdx;                // 天气城市索引（v0.6 启用：占用原 pad 字节，旧数据天然兼容 =0 杭州）
  uint8_t  pad[1];
};

#define SETTINGS_MAGIC 0x43444F33UL
#define EEPROM_ADDR    0

Settings cfg;

const char *TARGET_NAMES[2] = { "春节", "生日" };

/* ============================ 调色板（来自仿真）========================== */

uint16_t C_BG, C_BAR, C_FG, C_SUB, C_HINT, C_AMBER, C_ORANGE, C_GREEN,
         C_RED, C_BLUE, C_LINE, C_HL, C_DIMBAR, C_SOFT, C_DOTOFF, C_LBLUE;

/* ============================ 全局状态 =================================== */

enum { P_HOME = 0, P_POMO, P_CAL, P_WEATHER, P_SET, P_COUNT };

uint8_t page = P_HOME;
bool    op   = false;
uint8_t opLevel = 0;
uint8_t cursor  = 0;        // 通用光标（字段/菜单项/设置项）

struct Btn {
  uint8_t  pin;
  bool     lastRaw;
  bool     stable;
  uint32_t tChange;
  uint32_t tDown;
  bool     longSent;
};
Btn keyA = { KEY_A_PIN, false, false, 0, 0, false };
Btn keyB = { KEY_B_PIN, false, false, 0, 0, false };

uint32_t lastActivity = 0;

RtcTime now;

// 番茄钟
enum { POMO_IDLE = 0, POMO_RUN, POMO_PAUSE, POMO_DONE };
uint8_t  pomoState = POMO_IDLE;
int32_t  pomoSec   = 25 * 60;
uint32_t pomoTick  = 0;
uint8_t  pomoRound = 2;                 // 第几个番茄（与仿真初值一致）

// 日历
uint8_t calYear2 = 26, calMonth = 10;
uint8_t calCursor = 1;

// 天气页（城市索引存 cfg.cityIdx；切换城市时用于取消还原）
uint8_t wxCityBackup = 0;

// 温湿度
float curTempC = NAN;
float curHumPct = NAN;
uint32_t ahtTick = 0;

// 缓存（局部刷新用）
uint8_t lastMinuteDrawn = 255;
int32_t lastDaysShown = -1, lastHoursShown = -1;

// Toast
char     toastMsg[28] = "";
uint32_t toastUntil = 0;
bool     toastActive = false;

/* ============================ 字库渲染 =================================== */

// UTF-8 解码：返回 unicode 码点，递增指针
uint16_t utf8Next(const char **p) {
  uint8_t c = (uint8_t)**p;
  if (c < 0x80) { (*p)++; return c; }
  if ((c & 0xE0) == 0xC0) {
    uint16_t cp = ((c & 0x1F) << 6) | ((uint8_t)(*p)[1] & 0x3F);
    *p += 2; return cp;
  }
  if ((c & 0xF0) == 0xE0) {
    uint16_t cp = ((uint16_t)(c & 0x0F) << 12) |
                  (((uint8_t)(*p)[1] & 0x3F) << 6) |
                  ((uint8_t)(*p)[2] & 0x3F);
    *p += 3; return cp;
  }
  (*p)++; return '?';
}

// CJK12 查表
const uint8_t *font12Find(uint16_t uni, uint8_t *w) {
  *w = 6;
  for (uint16_t i = 0; i < font12_count; i++) {
    if (pgm_read_word(&font12_uni[i]) == uni) {
      *w = pgm_read_byte(&font12_w[i]);
      return font12_blob + pgm_read_word(&font12_off[i]);
    }
  }
  return NULL;
}

// 绘制单个 12px 字形（透明或带底）
void blit12(int x, int y, const uint8_t *g, uint8_t w, uint16_t fg, int16_t bg) {
  if (bg < 0) tft.drawBitmap((int16_t)x, y, g, w, 12, fg);
  else        tft.drawBitmap((int16_t)x, y, g, w, 12, fg, (uint16_t)bg);
}

// 测量字符串宽度
uint16_t ctxtW(const char *s) {
  uint16_t x = 0; uint8_t w;
  const char *p = s;
  while (*p) { uint16_t u = utf8Next(&p); font12Find(u, &w); x += w; }
  return x;
}

// 绘制 12px 文本。y 为纵向中心；bg<0 = 透明底
uint16_t ctxt(const char *s, int x, int y, uint16_t fg, int16_t bg, uint8_t datum) {
  uint16_t tw = ctxtW(s);
  if (datum == MC_DATUM) x -= tw / 2;
  else if (datum == MR_DATUM) x -= tw;
  int xs = x;
  uint8_t w;
  const char *p = s;
  while (*p) {
    uint16_t u = utf8Next(&p);
    const uint8_t *g = font12Find(u, &w);
    if (g && u != 0x20) blit12(x, y - 6, g, w, fg, bg);   // 空格只占位不绘制
    x += w;
  }
  return (uint16_t)(x - xs);
}

// ---- 数字字体（36/28/20）----
struct NumFont {
  const uint16_t *uni;
  const uint8_t  *w;
  const uint8_t  *h;
  const uint8_t  *adv;
  const uint16_t *off;
  const uint8_t  *blob;
};
const NumFont NF[3] = {
  { num36_uni, num36_w, num36_h, num36_adv, num36_off, num36_blob },   // 0
  { num28_uni, num28_w, num28_h, num28_adv, num28_off, num28_blob },   // 1
  { num20_uni, num20_w, num20_h, num20_adv, num20_off, num20_blob },   // 2
};
#define NF36 0
#define NF28 1
#define NF20 2
#define NF_COUNT_CHARS 14    // "0123456789:-."

bool nfFind(uint8_t f, char c, uint8_t *w, uint8_t *h, uint8_t *adv, uint16_t *off) {
  for (uint8_t i = 0; i < NF_COUNT_CHARS; i++) {
    if ((char)pgm_read_word(&NF[f].uni[i]) == c) {
      *w   = pgm_read_byte(&NF[f].w[i]);
      *h   = pgm_read_byte(&NF[f].h[i]);
      *adv = pgm_read_byte(&NF[f].adv[i]);
      *off = pgm_read_word(&NF[f].off[i]);
      return true;
    }
  }
  return false;
}

uint16_t ntxtW(uint8_t f, const char *s) {
  uint16_t x = 0; uint8_t w, h, adv; uint16_t off;
  while (*s) {
    if (nfFind(f, *s, &w, &h, &adv, &off)) x += adv;
    s++;
  }
  return x;
}

// y 为纵向中心；bg<0 = 透明底
uint16_t ntxt(uint8_t f, const char *s, int x, int y, uint16_t fg, int16_t bg, uint8_t datum) {
  uint16_t tw = ntxtW(f, s);
  if (datum == MC_DATUM) x -= tw / 2;
  else if (datum == MR_DATUM) x -= tw;
  int xs = x;
  uint8_t w, h, adv; uint16_t off;
  while (*s) {
    if (nfFind(f, *s, &w, &h, &adv, &off)) {
      int left = x + (adv - w) / 2;
      int top  = y - h / 2;
      if (bg < 0) tft.drawBitmap((int16_t)left, top, NF[f].blob + off, w, h, fg);
      else        tft.drawBitmap((int16_t)left, top, NF[f].blob + off, w, h, fg, (uint16_t)bg);
      x += adv;
    }
    s++;
  }
  return (uint16_t)(x - xs);
}

/* ============================ RTC 驱动 =================================== */

uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

uint8_t dayOfWeek(int y, int m, int d) {
  static const int ofs[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  if (m < 3) y -= 1;
  return (uint8_t)((y + y / 4 - y / 100 + y / 400 + ofs[m - 1] + d) % 7);
}

bool isLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

uint8_t daysInMonth(int y, int m) {
  static const uint8_t dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m == 2 && isLeapYear(y)) return 29;
  return dm[m - 1];
}

uint32_t dateToDays(int y, int m, int d) {
  uint32_t days = 0;
  for (int yy = 2000; yy < y; yy++) days += isLeapYear(yy) ? 366 : 365;
  for (int mm = 1; mm < m; mm++) days += daysInMonth(y, mm);
  days += (d - 1);
  return days;
}

bool rtcReadRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(PCF8563_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)PCF8563_ADDR, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool rtcWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(PCF8563_ADDR);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool rtcRead(RtcTime *t) {
  uint8_t b[7];
  if (!rtcReadRegs(0x02, b, 7)) return false;
  t->vl      = (b[0] & 0x80) ? true : false;
  t->sec     = bcd2dec(b[0] & 0x7F);
  t->min     = bcd2dec(b[1] & 0x7F);
  t->hour    = bcd2dec(b[2] & 0x3F);
  t->day     = bcd2dec(b[3] & 0x3F);
  t->weekday = b[4] & 0x07;
  t->century = (b[5] & 0x80) ? true : false;
  t->month   = bcd2dec(b[5] & 0x1F);
  t->year    = bcd2dec(b[6]);
  return true;
}

bool rtcWrite(const RtcTime *t) {
  Wire.beginTransmission(PCF8563_ADDR);
  Wire.write(0x02);
  Wire.write(dec2bcd(t->sec) & 0x7F);
  Wire.write(dec2bcd(t->min) & 0x7F);
  Wire.write(dec2bcd(t->hour) & 0x3F);
  Wire.write(dec2bcd(t->day) & 0x3F);
  Wire.write(t->weekday & 0x07);
  Wire.write((t->century ? 0x80 : 0x00) | (dec2bcd(t->month) & 0x1F));
  Wire.write(dec2bcd(t->year));
  return Wire.endTransmission() == 0;
}

void rtcSetFromBuildTime() {
  static const char *monNames = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char ms[4] = { __DATE__[0], __DATE__[1], __DATE__[2], '\0' };
  const char *p = strstr(monNames, ms);
  int mo = p ? (int)((p - monNames) / 3 + 1) : 1;

  RtcTime t;
  t.year    = (uint8_t)(atoi(__DATE__ + 7) % 100);
  t.month   = (uint8_t)mo;
  t.day     = (uint8_t)atoi(__DATE__ + 4);
  t.hour    = (uint8_t)atoi(__TIME__);
  t.min     = (uint8_t)atoi(__TIME__ + 3);
  t.sec     = (uint8_t)atoi(__TIME__ + 6);
  t.century = false;
  t.vl      = false;
  t.weekday = dayOfWeek(2000 + t.year, t.month, t.day);
  rtcWrite(&t);
}

void rtcSyncIfInvalid() {
  RtcTime t;
  bool bad = true;
  if (rtcRead(&t) && !t.vl &&
      t.year >= 25 && t.year <= 35 &&
      t.month >= 1 && t.month <= 12 &&
      t.day >= 1 && t.day <= 31) bad = false;
  if (bad) rtcSetFromBuildTime();
  rtcWriteReg(0x0D, 0x00);   // 关 CLKOUT
  rtcWriteReg(0x00, 0x00);   // 时钟运行
}

/* ============================ AHT30 驱动 ================================= */

// CRC-8：多项式 x^8+x^5+x^4+1 (0x31)，初值 0xFF —— AHT 系列专用，非标准 0x07
static uint8_t aht30Crc8(const uint8_t *d, uint8_t n) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

void aht30EnsureInit() {
  if (Wire.requestFrom((uint8_t)AHT30_ADDR, (uint8_t)1) != 1) return;
  uint8_t st = Wire.read();
  if (!(st & 0x08)) {
    Wire.beginTransmission(AHT30_ADDR);
    Wire.write(0xBE); Wire.write(0x08); Wire.write(0x00);
    Wire.endTransmission();
    delay(15);
  }
}

bool aht30Read(float *tempC, float *rhPct) {
  Wire.beginTransmission(AHT30_ADDR);
  Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(85);

  // ---- 忙位轮询：转换未完成时读到的数据无效（手册 bit7）----
  uint8_t st = 0x80;
  for (uint8_t i = 0; i < 4 && (st & 0x80); i++) {
    if (i) delay(20);
    if (Wire.requestFrom((uint8_t)AHT30_ADDR, (uint8_t)1) != 1) return false;
    st = Wire.read();
  }
  if (st & 0x80) return false;

  uint8_t buf[7] = { 0 };
  uint8_t n = Wire.requestFrom((uint8_t)AHT30_ADDR, (uint8_t)7);
  if (n < 6) return false;
  for (uint8_t i = 0; i < n && i < 7; i++) buf[i] = Wire.read();

  // ---- CRC-8（多项式 0x31 / 初值 0xFF，覆盖状态+5 数据字节）----
  // 有 CRC 字节就校验；个别兼容片只回 6 字节，此时跳过（不当错误）
  if (n >= 7 && aht30Crc8(buf, 6) != buf[6]) return false;

  uint32_t rhRaw = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | (buf[3] >> 4);
  uint32_t tRaw  = ((uint32_t)(buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];

  *rhPct = (float)rhRaw / 1048576.0f * 100.0f;
  *tempC = (float)tRaw  / 1048576.0f * 200.0f - 50.0f;
  return true;
}

/* ============================ EEPROM ===================================== */

void settingsLoad() {
  EEPROM.begin(256);
  EEPROM.get(EEPROM_ADDR, cfg);
  if (cfg.magic == 0x43444F32UL) {
    // v0.3 → v0.4 结构升级：前 13 字节布局兼容，保留个人设置、只清空 WiFi 字段
    cfg.magic  = SETTINGS_MAGIC;
    cfg.wifiOK = 0;
    memset(cfg.ssid, 0, sizeof(cfg.ssid));
    memset(cfg.pass, 0, sizeof(cfg.pass));
    memset(cfg.apPass, 0, sizeof(cfg.apPass));
    memset(cfg.pad, 0, sizeof(cfg.pad));
    EEPROM.put(EEPROM_ADDR, cfg);
    EEPROM.commit();
  }
  if (cfg.magic != SETTINGS_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic    = SETTINGS_MAGIC;
    cfg.tYear    = 27; cfg.tMonth = 2; cfg.tDay = 6;
    cfg.nameIdx  = 0;
    cfg.pomoMin  = 25;
    cfg.bright   = 3;
    cfg.timeout  = 60;
    cfg.buzzer   = 1;
    cfg.wifiOK   = 0;
    EEPROM.put(EEPROM_ADDR, cfg);
    EEPROM.commit();
  }
  if (cfg.pomoMin < 5 || cfg.pomoMin > 90) cfg.pomoMin = 25;
  if (cfg.nameIdx > 1) cfg.nameIdx = 0;
  if (cfg.cityIdx >= weatherCityCount()) cfg.cityIdx = 0;   // 天气城市越界 → 杭州
  cfg.ssid[sizeof(cfg.ssid) - 1] = 0;
  cfg.pass[sizeof(cfg.pass) - 1] = 0;
  cfg.apPass[sizeof(cfg.apPass) - 1] = 0;
}

void settingsSave() {
  EEPROM.put(EEPROM_ADDR, cfg);
  EEPROM.commit();
}

/* ============================ 倒数日计算 ================================= */

int64_t remainSeconds() {
  int64_t tgt  = (int64_t)dateToDays(2000 + cfg.tYear, cfg.tMonth, cfg.tDay) * 86400LL;
  int64_t nowS = (int64_t)dateToDays(2000 + now.year, now.month, now.day) * 86400LL
               + (int64_t)now.hour * 3600 + now.min * 60 + now.sec;
  return tgt - nowS;
}

/* ============================ 通用零件 =================================== */

void toast(const char *s, uint16_t ms) {
  strncpy(toastMsg, s, sizeof(toastMsg) - 1);
  toastMsg[sizeof(toastMsg) - 1] = '\0';
  toastUntil  = millis() + ms;
  toastActive = true;
  tft.fillRect(0, 146, 128, 14, C_HL);
  ctxt(s, 64, 153, C_BLUE, -1, MC_DATUM);
}

void drawStatusBar() {
  tft.fillRect(0, 0, 128, 13, C_BAR);
  char b[16];
  snprintf(b, sizeof(b), "%02u:%02u", now.hour, now.min);
  ctxt(b, 5, 7, C_SUB, C_BAR, ML_DATUM);
  snprintf(b, sizeof(b), "%u月%u日", now.month, now.day);
  ctxt(b, 42, 7, C_HINT, C_BAR, ML_DATUM);
  // WiFi 信号强度：真实 RSSI（未连接时 4 格全暗）
  const uint8_t hs[4] = { 3, 5, 7, 9 };
  uint8_t lit = wifiBars();
  for (uint8_t i = 0; i < 4; i++)
    tft.fillRect(106 + i * 4, 10 - hs[i], 3, hs[i], (i < lit) ? C_GREEN : C_DIMBAR);
}

void drawDots() {
  uint8_t w[5]; uint16_t total = 0;
  for (uint8_t i = 0; i < 5; i++) { w[i] = (i == page) ? 9 : 4; total += w[i] + 4; }
  total -= 4;
  int x = (128 - total) / 2;
  for (uint8_t i = 0; i < 5; i++) {
    tft.fillRect(x, 151, w[i], 4, (i == page) ? C_BLUE : C_DOTOFF);
    x += w[i] + 4;
  }
}

void drawBanner() {
  tft.fillRect(0, 0, 128, 13, C_HL);
  ctxt("编辑中", 5, 7, C_BLUE, C_HL, ML_DATUM);
  ctxt("A 长按返回", 123, 7, C_LBLUE, C_HL, MR_DATUM);
  tft.drawRect(0, 0, 128, 160, C_BLUE);
}

/* ============================ 页 0：倒数日 =============================== */

void countdownLine(int32_t d, int32_t h) {
  char ds[8], hs[8];
  snprintf(ds, sizeof(ds), "%ld", (long)d);
  snprintf(hs, sizeof(hs), "%ld", (long)h);

  const uint16_t avail = 122;
  uint8_t df = NF36, hf = NF20;
  uint16_t total = ntxtW(df, ds) + 2 + 12 + 7 + ntxtW(hf, hs) + 2 + 24;
  if (total > avail) { df = NF28; total = ntxtW(df, ds) + 2 + 12 + 7 + ntxtW(hf, hs) + 2 + 24; }
  if (total > avail) { df = NF20; total = ntxtW(df, ds) + 2 + 12 + 7 + ntxtW(hf, hs) + 2 + 24; }

  int x = 64 - total / 2;
  x += ntxt(df, ds, x, 54, C_AMBER, C_BG, ML_DATUM); x += 2;
  x += ctxt("天", x, 68, C_SUB, C_BG, ML_DATUM);     x += 7;
  x += ntxt(hf, hs, x, 63, C_SUB, C_BG, ML_DATUM);   x += 2;
  ctxt("小时", x, 68, C_SUB, C_BG, ML_DATUM);
}

void drawHomeOp() {   // dateEdit
  ctxt("修改目标日期", 64, 26, C_SUB, C_BG, MC_DATUM);

  char s[12];
  snprintf(s, sizeof(s), "%04u-%02u-%02u", 2000 + cfg.tYear, cfg.tMonth, cfg.tDay);

  uint8_t w, h, adv; uint16_t off;
  nfFind(NF20, '0', &w, &h, &adv, &off);
  uint8_t digitW = adv;
  nfFind(NF20, '-', &w, &h, &adv, &off);
  uint8_t dashW = adv;

  uint16_t tw = ntxtW(NF20, s);
  int xs = 64 - tw / 2;
  int segX[3]; uint16_t segW[3];
  segX[0] = xs;                            segW[0] = 4 * digitW;   // 年
  segX[1] = xs + segW[0] + dashW;          segW[1] = 2 * digitW;   // 月
  segX[2] = segX[1] + segW[1] + dashW;     segW[2] = 2 * digitW;   // 日

  if (cursor > 2) cursor = 2;
  tft.fillRect(segX[cursor] - 1, 46, segW[cursor] + 3, 24, C_HL);
  tft.drawRect(segX[cursor] - 1, 46, segW[cursor] + 3, 24, C_BLUE);

  ntxt(NF20, s, 64, 59, C_FG, -1, MC_DATUM);

  ctxt("▲", segX[cursor] + segW[cursor] / 2, 40, C_BLUE, C_BG, MC_DATUM);

  ctxt("A短按减 B短按加", 64, 92, C_SUB, C_BG, MC_DATUM);
  ctxt("B长按 下一字段/保存", 64, 110, C_HINT, C_BG, MC_DATUM);

  char b[24];
  const char *fl[3] = { "年", "月", "日" };
  snprintf(b, sizeof(b), "正在调「%s」", fl[cursor]);
  ctxt(b, 64, 131, C_LBLUE, C_BG, MC_DATUM);
}

void drawHome() {
  tft.fillScreen(C_BG);
  drawStatusBar();

  if (op) { drawHomeOp(); drawDots(); drawBanner(); return; }

  ctxt("距离", 10, 21, C_SUB, C_BG, ML_DATUM);
  ctxt(TARGET_NAMES[cfg.nameIdx], 44, 21, C_FG, C_BG, ML_DATUM);

  int64_t rem = remainSeconds();
  if (rem <= 0) {
    ntxt(NF20, "0", 64, 54, C_AMBER, C_BG, MC_DATUM);
    ctxt("天", 80, 68, C_SUB, C_BG, ML_DATUM);
    lastDaysShown = lastHoursShown = -1;
  } else {
    int32_t d = (int32_t)(rem / 86400LL);
    int32_t h = (int32_t)((rem % 86400LL) / 3600LL);
    countdownLine(d, h);
    lastDaysShown = d; lastHoursShown = h;
  }

  tft.fillRect(14, 88, 100, 1, C_LINE);

  char b[32];
  if (isnan(curTempC)) snprintf(b, sizeof(b), "室内 --.-°C · --%%RH");
  else snprintf(b, sizeof(b), "室内 %.1f°C · %ld%%RH", curTempC, (long)curHumPct);
  ctxt(b, 64, 102, C_GREEN, C_BG, MC_DATUM);

  wxDrawHomeLines();          // 室外天气两行（真实数据，见 Weather.ino）

  drawDots();
}

/* ============================ 页 1：番茄钟 =============================== */

void pomoMenu() {
  ctxt("番茄钟菜单", 64, 26, C_SUB, C_BG, MC_DATUM);
  const char *items[3] = { "开始/暂停", "重置计时", "设置时长" };
  for (uint8_t i = 0; i < 3; i++) {
    int y = 52 + i * 24;
    if (i == cursor) {
      tft.fillRect(12, y - 9, 104, 20, C_HL);
      ctxt(items[i], 24, y, C_FG, -1, ML_DATUM);
      ctxt("‹", 108, y, C_BLUE, -1, MC_DATUM);
    } else {
      ctxt(items[i], 24, y, C_SUB, C_BG, ML_DATUM);
    }
  }
}

void drawPomo() {
  tft.fillScreen(C_BG);
  drawStatusBar();

  if (op) { pomoMenu(); drawDots(); drawBanner(); return; }

  bool running = (pomoState == POMO_RUN);
  bool done    = (pomoState == POMO_DONE);

  if (done)        ctxt("已完成", 64, 25, C_GREEN, C_BG, MC_DATUM);
  else if (running) ctxt("专注中", 64, 25, C_ORANGE, C_BG, MC_DATUM);
  else              ctxt("已暂停", 64, 25, C_SUB, C_BG, MC_DATUM);

  char b[32];
  if (done) snprintf(b, sizeof(b), "第 %u 个番茄 · %u 分钟完成", pomoRound, cfg.pomoMin);
  else      snprintf(b, sizeof(b), "第 %u 个番茄 · %u 分钟", pomoRound, cfg.pomoMin);
  ctxt(b, 64, 42, C_HINT, C_BG, MC_DATUM);

  int32_t m = pomoSec / 60, s = pomoSec % 60;
  char tb[8];
  snprintf(tb, sizeof(tb), "%02ld:%02ld", (long)m, (long)s);
  ntxt(NF36, tb, 64, 74, running ? C_FG : C_SUB, C_BG, MC_DATUM);

  int32_t total = (int32_t)cfg.pomoMin * 60;
  float dn = (total > 0) ? (1.0f - (float)pomoSec / (float)total) : 0;
  if (dn < 0) dn = 0; if (dn > 1) dn = 1;
  tft.fillRect(14, 98, 100, 3, C_LINE);
  tft.fillRect(14, 98, (int)(100 * dn), 3, C_ORANGE);

  const int dw = 7, gap = 6, tot = 4 * dw + 3 * gap, sx = (128 - tot) / 2;
  for (uint8_t i = 0; i < 4; i++) {
    int x = sx + i * (dw + gap);
    if (i < pomoRound - 1)       tft.fillRect(x, 114, dw, dw, C_ORANGE);
    else if (i == pomoRound - 1) tft.drawRect(x, 114, dw, dw, C_ORANGE);
    else                         tft.drawRect(x, 114, dw, dw, C_DIMBAR);
  }

  ctxt("B 长按 打开菜单", 64, 136, C_HINT, C_BG, MC_DATUM);
  drawDots();
}

/* ============================ 页 2：日历 ================================= */

void drawCalGrid(uint8_t cursorDay) {
  uint32_t first = dayOfWeek(2000 + calYear2, calMonth, 1);
  uint8_t  nDays = daysInMonth(2000 + calYear2, calMonth);
  bool     isCurMonth = (now.year == calYear2 && now.month == calMonth);
  uint8_t  tgtDay = 0;
  if (cfg.tYear == calYear2 && cfg.tMonth == calMonth) tgtDay = cfg.tDay;

  const char *wd = "日一二三四五六";
  const char *p = wd;
  for (uint8_t i = 0; i < 7; i++) {
    uint16_t u = utf8Next(&p);
    uint8_t w; const uint8_t *g = font12Find(u, &w);
    if (g) blit12(2 + i * 18 + 9 - w / 2, 43 - 6, g, w, C_HINT, C_BG);
  }

  for (uint8_t d = 1; d <= nDays; d++) {
    uint8_t idx = (uint8_t)(first + d - 1);
    uint8_t r = idx / 7, c = idx % 7;
    int x = 2 + c * 18, y = 50 + r * 15;

    char b[4];
    snprintf(b, sizeof(b), "%u", d);

    if (isCurMonth && d == now.day) {
      tft.fillRect(x + 1, y, 16, 13, C_HL);
      ctxt(b, x + 9, y + 7, C_BLUE, -1, MC_DATUM);
    } else {
      ctxt(b, x + 9, y + 7, (tgtDay && d == tgtDay) ? C_RED : C_SOFT, C_BG, MC_DATUM);
    }
    if (tgtDay && d == tgtDay) tft.fillRect(x + 7, y + 12, 5, 2, C_RED);

    if (cursorDay && d == cursorDay) tft.drawRect(x - 1, y - 2, 18, 16, C_FG);
  }
}

void drawCal() {
  tft.fillScreen(C_BG);
  drawStatusBar();

  char b[16];
  snprintf(b, sizeof(b), "%u 年 %u 月", 2000 + calYear2, calMonth);
  ctxt(b, 64, 23, C_FG, C_BG, MC_DATUM);

  if (op && opLevel == 0) ctxt("▲ A/B短按 翻月", 64, 38, C_BLUE, C_BG, MC_DATUM);
  else if (op && opLevel == 1) ctxt("▲A/B选日 B长按设目标", 64, 38, C_BLUE, C_BG, MC_DATUM);

  drawCalGrid(op && opLevel == 1 ? calCursor : 0);

  snprintf(b, sizeof(b), "目标日 %04u-%02u-%02u", 2000 + cfg.tYear, cfg.tMonth, cfg.tDay);
  ctxt(b, 64, 144, C_HINT, C_BG, MC_DATUM);

  drawDots();
  if (op) drawBanner();
}

/* ============================ 页 3：天气 ================================= */
/* 天气页的完整实现（数据 + 图标 + 渲染 + 操作态菜单）已移至 Weather.ino */



/* ============================ 页 4：设置 ================================= */

void drawSet() {
  tft.fillScreen(C_BG);
  drawStatusBar();

  ctxt("设置", 10, 23, C_FG, C_BG, ML_DATUM);

  bool ed = (op && opLevel == 1);

  char v0[16], v5[16];
  snprintf(v0, sizeof(v0), "%02u-%02u-%02u", cfg.tYear, cfg.tMonth, cfg.tDay);
  snprintf(v5, sizeof(v5), "%u / 5", cfg.pomoMin);

  const char *keys[8] = { "目标日期", "目标名称", "屏幕亮度", "背光超时",
                          "蜂鸣器", "番茄时长", "WiFi 设置", "网络授时" };
  char vb[8][16];
  strncpy(vb[0], v0, 16);
  snprintf(vb[1], 16, "%s", TARGET_NAMES[cfg.nameIdx]);
  snprintf(vb[2], 16, "%u / 5", cfg.bright);
  snprintf(vb[3], 16, "%u 秒", cfg.timeout);
  snprintf(vb[4], 16, "%s", cfg.buzzer ? "开" : "关");
  strncpy(vb[5], v5, 16);
  snprintf(vb[6], 16, "%s", wifiStateText());
  snprintf(vb[7], 16, "%s", ntpStateText());

  for (uint8_t i = 0; i < 8; i++) {
    int y = 30 + i * 13;
    bool sel = (op && i == cursor);
    if (sel) {
      tft.fillRect(4, y - 1, 120, 13, C_HL);
      tft.fillRect(4, y - 1, 2, 13, ed ? C_ORANGE : C_BLUE);
      ctxt(keys[i], 11, y + 6, C_FG, -1, ML_DATUM);
      ctxt(vb[i], 119, y + 6, ed ? C_ORANGE : C_BLUE, -1, MR_DATUM);
    } else {
      ctxt(keys[i], 11, y + 6, C_SUB, C_BG, ML_DATUM);
      ctxt(vb[i], 119, y + 6, C_HINT, C_BG, MR_DATUM);
    }
  }

  if (!op)          ctxt("B长按 进入设置", 64, 143, C_HINT, C_BG, MC_DATUM);
  else if (ed)      ctxt("A/B 改值  B长按 保存", 64, 143, C_ORANGE, C_BG, MC_DATUM);
  else if (cursor == 6) ctxt("B长按 开始配网", 64, 143, C_BLUE, C_BG, MC_DATUM);
  else if (cursor == 7) ctxt("B长按 立即授时", 64, 143, C_BLUE, C_BG, MC_DATUM);
  else              ctxt("A/B 选择  B长按 编辑", 64, 143, C_BLUE, C_BG, MC_DATUM);

  drawDots();
  if (op) drawBanner();
}

/* ============================ 绘制调度 =================================== */

void drawPage() {
  switch (page) {
    case P_HOME:    drawHome();    break;
    case P_POMO:    drawPomo();    break;
    case P_CAL:     drawCal();     break;
    case P_WEATHER: drawWeather(); break;
    default:        drawSet();     break;
  }
}

/* ============================ 按键扫描 =================================== */

enum { EV_NONE = 0, EV_SHORT = 1, EV_LONG = 2 };

uint8_t scanBtn(Btn *b, uint32_t nowMs) {
  bool raw = (digitalRead(b->pin) == LOW);

  if (raw != b->lastRaw) { b->lastRaw = raw; b->tChange = nowMs; return EV_NONE; }
  if (nowMs - b->tChange < DEBOUNCE_MS) return EV_NONE;

  if (raw != b->stable) {
    b->stable = raw;
    if (raw) { b->tDown = nowMs; b->longSent = false; }
    else if (!b->longSent) return EV_SHORT;
    return EV_NONE;
  }

  if (b->stable && !b->longSent && (nowMs - b->tDown >= LONG_MS)) {
    b->longSent = true;
    return EV_LONG;
  }
  return EV_NONE;
}

/* ============================ 操作逻辑 =================================== */

void homeAdj(int8_t dd) {
  if (cursor == 0) {
    int v = (int)cfg.tYear + dd;
    if (v < 25) v = 35; if (v > 35) v = 25;
    cfg.tYear = (uint8_t)v;
  } else if (cursor == 1) {
    int v = (int)cfg.tMonth + dd;
    if (v < 1) v = 12; if (v > 12) v = 1;
    cfg.tMonth = (uint8_t)v;
  } else {
    int v = (int)cfg.tDay + dd;
    uint8_t dim = daysInMonth(2000 + cfg.tYear, cfg.tMonth);
    if (v < 1) v = dim; if (v > (int)dim) v = 1;
    cfg.tDay = (uint8_t)v;
  }
}

void calMonthStep(int8_t dd) {
  int m = (int)calMonth + dd;
  if (m < 1)  { m = 12; calYear2 = (calYear2 + 99) % 100; }
  if (m > 12) { m = 1;  calYear2 = (calYear2 + 1) % 100; }
  calMonth = (uint8_t)m;
}

void setAdj(uint8_t i, int8_t dd) {
  if (i == 0) {
    int v = (int)cfg.tDay + dd;
    uint8_t dim = daysInMonth(2000 + cfg.tYear, cfg.tMonth);
    if (v < 1) v = dim; if (v > (int)dim) v = 1;
    cfg.tDay = (uint8_t)v;
  } else if (i == 1) {
    cfg.nameIdx = cfg.nameIdx ? 0 : 1;
  } else if (i == 2) {
    int v = (int)cfg.bright + dd;
    if (v < 1) v = 5; if (v > 5) v = 1;
    cfg.bright = (uint8_t)v;
  } else if (i == 3) {
    int v = (int)cfg.timeout + dd * 10;
    if (v < 10) v = 120; if (v > 120) v = 10;
    cfg.timeout = (uint8_t)v;
  } else if (i == 4) {
    cfg.buzzer = cfg.buzzer ? 0 : 1;
  } else if (i == 5) {
    int v = (int)cfg.pomoMin + dd * 5;
    if (v < 5) v = 60; if (v > 60) v = 5;
    cfg.pomoMin = (uint8_t)v;
    if (pomoState == POMO_IDLE || pomoState == POMO_DONE) pomoSec = cfg.pomoMin * 60;
  } else {
    toast("WiFi 配网：后续版本", 1200);
  }
}

void pomoPresetNext() {
  const uint8_t presets[4] = { 15, 25, 45, 60 };
  uint8_t idx = 1;                     // 默认 25
  for (uint8_t i = 0; i < 4; i++) if (presets[i] == cfg.pomoMin) idx = i;
  idx = (idx + 1) % 4;
  cfg.pomoMin = presets[idx];
  if (pomoState == POMO_IDLE || pomoState == POMO_DONE) pomoSec = cfg.pomoMin * 60;
}

void openOp() { op = true; opLevel = 0; cursor = 0; lastActivity = millis(); drawPage(); }
void closeOp() { op = false; opLevel = 0; cursor = 0; lastActivity = millis(); drawPage(); }

void onKeyA(uint8_t ev) {
  lastActivity = millis();

  if (!op) {
    if (ev == EV_LONG) { page = P_HOME; cursor = 0; drawPage(); }
    else               { page = (page + P_COUNT - 1) % P_COUNT; cursor = 0; drawPage(); }
    return;
  }

  if (ev == EV_LONG) {
    if (page == P_SET && opLevel == 1) { opLevel = 0; drawPage(); }
    else if (page == P_WEATHER && opLevel == 1) {      // 取消切换城市：还原
      cfg.cityIdx = wxCityBackup;
      opLevel = 0; drawPage();
    }
    else closeOp();
    return;
  }

  switch (page) {
    case P_HOME:
      homeAdj(-1); drawPage(); break;
    case P_POMO:
      cursor = (cursor + 2) % 3; drawPage(); break;
    case P_CAL:
      if (opLevel == 0) calMonthStep(-1);
      else { if (calCursor > 1) calCursor--; }
      drawPage(); break;
    case P_WEATHER:
      if (opLevel == 0) cursor = (cursor + 1) % 2;                     // 菜单：上一项
      else cfg.cityIdx = (cfg.cityIdx + weatherCityCount() - 1) % weatherCityCount();
      drawPage(); break;
    default:
      if (opLevel == 1) { setAdj(cursor, -1); drawPage(); }
      else { cursor = (cursor + 7) % 8; drawPage(); }
      break;
  }
}

void onKeyB(uint8_t ev) {
  lastActivity = millis();

  if (!op) {
    if (ev == EV_LONG) {
      if (page == P_POMO && (pomoState == POMO_RUN || pomoState == POMO_PAUSE)) {
        pomoState = POMO_IDLE;                            // 菜单里也可以重置，这里直接进菜单
        pomoSec = (int32_t)cfg.pomoMin * 60;
      }
      openOp();
    } else {
      page = (page + 1) % P_COUNT; cursor = 0; drawPage();
    }
    return;
  }

  if (ev == EV_SHORT) {
    switch (page) {
      case P_HOME:
        homeAdj(+1); drawPage(); break;
      case P_POMO:
        cursor = (cursor + 1) % 3; drawPage(); break;
      case P_CAL:
        if (opLevel == 0) calMonthStep(+1);
        else {
          uint8_t dim = daysInMonth(2000 + calYear2, calMonth);
          if (calCursor < dim) calCursor++;
        }
        drawPage(); break;
      case P_WEATHER:
        if (opLevel == 0) cursor = (cursor + 1) % 2;                   // 菜单
        else cfg.cityIdx = (cfg.cityIdx + 1) % weatherCityCount();     // 下一城市
        drawPage(); break;
      default:
        if (opLevel == 1) { setAdj(cursor, +1); drawPage(); }
        else { cursor = (cursor + 1) % 8; drawPage(); }
        break;
    }
    return;
  }

  // ---- B 长按（操作态确认）----
  switch (page) {
    case P_HOME:
      if (cursor < 2) { cursor++; drawPage(); }
      else { settingsSave(); closeOp(); toast("目标日期已保存", 1000); }
      break;
    case P_POMO:
      if (cursor == 0) {
        if (pomoState == POMO_RUN) pomoState = POMO_PAUSE;
        else { if (pomoState != POMO_PAUSE) pomoSec = (int32_t)cfg.pomoMin * 60; pomoState = POMO_RUN; pomoTick = millis(); }
      } else if (cursor == 1) {
        pomoSec = (int32_t)cfg.pomoMin * 60; pomoState = POMO_IDLE;
      } else {
        pomoPresetNext(); settingsSave();
      }
      closeOp();
      break;
    case P_CAL:
      if (opLevel == 0) {
        calCursor = (cfg.tYear == calYear2 && cfg.tMonth == calMonth) ? cfg.tDay : 1;
        opLevel = 1; drawPage();
      } else {
        cfg.tYear = calYear2; cfg.tMonth = calMonth; cfg.tDay = calCursor;
        settingsSave();
        closeOp(); toast("已设为倒数目标日", 1200);
      }
      break;
    case P_WEATHER:
      if (opLevel == 1) {                        // 确认切换城市
        settingsSave();
        opLevel = 0;
        drawPage();
        toast("城市已切换", 1000);
        weatherRefreshSoon();
      } else if (cursor == 0) {                  // 立即刷新
        weatherManualRefresh();
      } else {                                   // 进入切换城市
        wxCityBackup = cfg.cityIdx;
        opLevel = 1;
        drawPage();
      }
      break;
    default:
      if (cursor == 6 && opLevel == 0) {
        // 「WiFi 设置」→ 进入配网模式（阻塞；应用层在此期间挂起）
        op = false; opLevel = 0;
        wifiEnterConfig();
        drawSet();                            // 回来后重画设置页（含最新 WiFi 状态）
      } else if (cursor == 7 && opLevel == 0) {
        // 「网络授时」→ 手动立即授时（结果通过 toast 提示）
        ntpManualSync();
      } else if (opLevel == 0) { opLevel = 1; drawPage(); }
      else { settingsSave(); opLevel = 0; drawPage(); toast("已保存", 800); }
      break;
  }
}

/* ============================ 定时任务 =================================== */

// 主页局部刷新：倒数行（天数/小时），只清一条横带，避免整页重绘闪烁
void updateHomeCountdown(int32_t d, int32_t h) {
  tft.fillRect(0, 40, 128, 38, C_BG);
  countdownLine(d, h);
}

// 主页局部刷新：温湿度行
void updateHomeEnv() {
  char b[32];
  if (isnan(curTempC)) snprintf(b, sizeof(b), "室内 --.-°C · --%%RH");
  else snprintf(b, sizeof(b), "室内 %.1f°C · %ld%%RH", curTempC, (long)curHumPct);
  tft.fillRect(0, 95, 128, 14, C_BG);
  ctxt(b, 64, 102, C_GREEN, C_BG, MC_DATUM);
}

void tickClock() {
  RtcTime t;
  if (!rtcRead(&t)) return;
  now = t;

  if (now.min != lastMinuteDrawn) {
    lastMinuteDrawn = now.min;
    if (!op && page != P_CAL) drawStatusBar();
    else if (!op) { drawCal(); }         // 日历页的"今天"标记也要跟着跨日
  }

  // WiFi 信号格数变化 → 刷新状态栏（真实 RSSI）
  static uint8_t lastBars = 0xFF;
  uint8_t bars = wifiBars();
  if (bars != lastBars) {
    lastBars = bars;
    if (!op && page != P_CAL) drawStatusBar();
  }

  // 主页大数字局部刷新
  if (page == P_HOME && !op && !toastActive) {
    int64_t rem = remainSeconds();
    if (rem > 0) {
      int32_t d = (int32_t)(rem / 86400LL);
      int32_t h = (int32_t)((rem % 86400LL) / 3600LL);
      if (d != lastDaysShown || h != lastHoursShown) {
        lastDaysShown = d;
        lastHoursShown = h;
        updateHomeCountdown(d, h);
      }
    }
  }

  // 番茄钟走秒
  if (pomoState == POMO_RUN) {
    uint32_t nowMs = millis();
    while (nowMs - pomoTick >= 1000) {
      pomoTick += 1000;
      if (pomoSec > 0) pomoSec--;
      if (pomoSec <= 0) {
        pomoState = POMO_DONE;
        pomoSec = 0;
        if (pomoRound < 4) pomoRound++;
        if (page == P_POMO && !op) drawPomo();
        break;
      }
    }
    if (pomoState == POMO_RUN && page == P_POMO && !op && !toastActive) {
      int32_t m = pomoSec / 60, s = pomoSec % 60;
      char tb[8];
      snprintf(tb, sizeof(tb), "%02ld:%02ld", (long)m, (long)s);
      ntxt(NF36, tb, 64, 74, C_FG, C_BG, MC_DATUM);
    }
  }
}

void tickEnv() {
  uint32_t nowMs = millis();
  if (nowMs - ahtTick < 2000) return;
  ahtTick = nowMs;
  if (page == P_HOME && !op) {
    float t, h;
    if (aht30Read(&t, &h)) {
      // 只有数值真的有变化才局部刷新（变化 <0.05°C / <0.5%RH 视为没变）
      bool changed = true;
      if (!isnan(curTempC)) {
        changed = (fabsf(t - curTempC) >= 0.05f) || (fabsf(h - curHumPct) >= 0.5f);
      }
      curTempC = t;
      curHumPct = h;
      if (changed && !toastActive) updateHomeEnv();
    }
  }
}

/* ============================ setup / loop =============================== */

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("[Countdown] keys: A=D4/GPIO2   B=D6/GPIO12");

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  // ⚠⚠ 按键 pinMode 必须放在 tft.init() 之后（2026-10-15 血泪教训）：
  // tft.init() 内部会调用 spi.begin()，而 ESP8266 的 SPI.begin() 会把
  // GPIO12（= 硬件 SPI 的 MISO / HMISO）重新配置成 SPECIAL 功能，
  // 并清除其内部上拉 —— 如果在那之前设置 INPUT_PULLUP，会被无声地覆盖掉，
  // 导致 GPIO12 浮空、按键事件永远读不到。
  // 这里的顺序：先让 SPI 抢走，再抢回来（TFT 只写不读，不需要 MISO）。
  pinMode(KEY_A_PIN, INPUT_PULLUP);
  pinMode(KEY_B_PIN, INPUT_PULLUP);

  C_BG     = tft.color565(0x0D, 0x15, 0x20);
  C_BAR    = tft.color565(0x16, 0x23, 0x2F);
  C_FG     = tft.color565(0xFF, 0xFF, 0xFF);
  C_SUB    = tft.color565(0x8A, 0xA3, 0xBC);
  C_HINT   = tft.color565(0x5C, 0x76, 0x91);
  C_AMBER  = tft.color565(0xFF, 0xB0, 0x20);
  C_ORANGE = tft.color565(0xFF, 0x7A, 0x45);
  C_GREEN  = tft.color565(0x3F, 0xD6, 0x8C);
  C_RED    = tft.color565(0xFF, 0x5A, 0x5A);
  C_BLUE   = tft.color565(0x4F, 0xA8, 0xFF);
  C_LINE   = tft.color565(0x1E, 0x30, 0x40);
  C_HL     = tft.color565(0x17, 0x34, 0x4F);
  C_DIMBAR = tft.color565(0x26, 0x40, 0x4F);
  C_SOFT   = tft.color565(0xC7, 0xD6, 0xE4);
  C_DOTOFF = tft.color565(0x26, 0x3D, 0x52);
  C_LBLUE  = tft.color565(0x7F, 0xB6, 0xE8);

  Wire.begin(4, 5);
  delay(20);

  settingsLoad();
  rtcSyncIfInvalid();
  aht30EnsureInit();
  wifiAutoConnect();     // 有凭据则后台自动连接（状态栏信号图标随真实 RSSI 变化）
  ntpInit();             // NTP 授时：注册校时回调；WiFi 连上后自动开始授时

  rtcRead(&now);
  lastMinuteDrawn = now.min;

  calYear2 = now.year;
  calMonth = now.month;
  pomoSec  = (int32_t)cfg.pomoMin * 60;

  lastActivity = millis();
  drawPage();

  Serial.printf("\n[Countdown] %s started, target %04u-%02u-%02u\n",
                FW_VERSION, 2000 + cfg.tYear, cfg.tMonth, cfg.tDay);
}

void loop() {
  uint32_t nowMs = millis();

  // ---- 原始电平变化日志（诊断用，跨过消抖，任何毛刺都能看到）----
  static uint8_t lastRawBits = 0xFF;   // 首轮强制打印一次实际状态
  uint8_t rawBits = (digitalRead(KEY_A_PIN) == LOW ? 1 : 0) |
                    (digitalRead(KEY_B_PIN) == LOW ? 2 : 0);
  if (rawBits != lastRawBits) {
    Serial.printf("[pin] A(D4/G2)=%d  B(D6/G12)=%d\n", rawBits & 1, (rawBits >> 1) & 1);
    lastRawBits = rawBits;
  }

  uint8_t evA = scanBtn(&keyA, nowMs);
  uint8_t evB = scanBtn(&keyB, nowMs);

  if (evA) {
    Serial.printf("[key] A=D4(G2) %s\n", evA == EV_LONG ? "LONG" : "SHORT");
    onKeyA(evA);
  }
  if (evB) {
    Serial.printf("[key] B=D6(G12) %s\n", evB == EV_LONG ? "LONG" : "SHORT");
    onKeyB(evB);
  }

  static uint32_t lastSec = 0;
  if (nowMs - lastSec >= 1000) {
    lastSec = nowMs;
    tickClock();
  }

  tickEnv();
  ntpTick();             // NTP 授时状态机（非阻塞）
  weatherTick();         // 天气获取状态机（非阻塞）

  if (toastActive && nowMs > toastUntil) {
    toastActive = false;
    drawPage();
  }

  if (!op && (page != P_HOME) && (nowMs - lastActivity > IDLE_MS)) {
    page = P_HOME; cursor = 0;
    drawPage();
    lastActivity = nowMs;
  }

  delay(5);
}
