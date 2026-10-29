/* ============================================================================
 *  ESP8266 开发板功能自检程序  ——  空板测试（未接任何模块）
 *  项目：双层 ESP8266 倒数日   |   文件：Test.ino
 * ----------------------------------------------------------------------------
 *  适用：顶盖丝印 ESP8266MOD 的现成开发板（ESP-12F 级，NodeMCU / D1 mini 类）
 *  前提：板上不接任何外围模块，仅靠「串口 + 板载 LED」即可完成全部检测
 *
 *  ── Arduino IDE 设置 ──────────────────────────────────────────────────
 *    开发板        : NodeMCU 1.0 (ESP-12E Module)  或  Generic ESP8266 Module
 *                    （选 NodeMCU 时 Flash Size 默认就是 4MB，比较省事）
 *    Flash Size    : ★ 必须与实物芯片一致 ★
 *                    这类开发板绝大多数是 4MB(32Mb)。若选成 1MB，程序照样能跑，
 *                    但会白白浪费 3MB —— 后续放中文字库、OTA 升级都不够用。
 *                    拿不准就先按 4MB 编译，跑本程序第 1 项会告诉你真实容量。
 *    CPU Frequency : 80 MHz
 *    Upload Speed  : 115200
 *    Debug port    : Disabled（无）
 *    串口监视器    : 115200 baud + 换行符选「NL 或 CR」
 *
 *  ── 本项目后续引脚分配（本程序只读电平，不驱动，安全）─────────────────
 *    I2C  : GPIO4 = D2 (SDA)   GPIO5 = D1 (SCL)     AHT30 0x38 / PCF8563 0x51
 *    显示屏: GPIO14 = D5 (SCK)  GPIO13 = D7 (MOSI)   GPIO15 = D8 (CS)
 *            GPIO0  = D3 (DC)   GPIO16 = D0 (RES)    BLK 直接接 3.3V
 *    按键 : GPIO2  = D4          GPIO12 = D6
 *    空闲 : GPIO1 (TX) GPIO3 (RX)  —— 与板载 USB 转串口芯片共用，当应急备用
 *
 *  ── GPIO1 (TX) 为什么不在扫描列表里 ──────────────────────────────────
 *    它是串口发送脚，把它切成 GPIO 输入就再也打不出调试信息了。
 *    所以扫描列表刻意跳过它 —— 这是有意为之，不是漏了。
 * ========================================================================== */

#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <Wire.h>

/* ============================== 可调参数 ================================= */

#define SERIAL_BAUD       115200

// 板载 LED。绝大多数 ESP8266 开发板：LED 接在 GPIO2(D4)，低电平点亮
#define LED_PIN           2
#define LED_ACTIVE_LOW    1      // 若你的板子是高电平点亮，改成 0

// A0 电压换算基准：
//   3.3 = NodeMCU / D1 mini 等「板上已带分压」的开发板
//   1.0 = 裸 ESP-12F 模块（A0 量程只有 0~1.0V）
#define ADC_REF_VOLT      3.3

#define EEPROM_SIZE       64
#define EEPROM_MAGIC_ADDR 0
#define EEPROM_MAGIC_VAL  0x5A5AA5A5UL
#define EEPROM_PAT_ADDR   8      // 8..39 共 32 字节用于图案测试

// 引脚监视模式下的观察对象
#define WATCH_PIN_A       2
#define WATCH_PIN_B       12

/* ============================== 全局状态 ================================= */

uint16_t gPass = 0, gFail = 0, gWarn = 0;
bool     gMonitor = false;

// I2C 扫描结果，供后面的传感器功能测试判断用
bool     gHasAHT30 = false;
bool     gHasRTC   = false;

/* -------- 自定义类型（必须放在所有函数之前）------------------------------
 * Arduino IDE 的 .ino 预处理会把函数原型自动提升到文件顶部，
 * 若结构体定义在函数之后，提升出来的原型会因「RtcTime 未声明」而报错。
 * ---------------------------------------------------------------------- */
struct RtcTime {
  uint8_t sec, min, hour, day, weekday, month, year;
  bool    century;
  bool    vl;
};

/* ============================== 小工具 =================================== */

void rule(char c) {
  char buf[61];
  memset(buf, c, 60);
  buf[60] = '\0';
  Serial.println(buf);
}

void title(const char *t) {
  Serial.println();
  rule('=');
  Serial.printf("  %s\n", t);
  rule('=');
}

void ok(const char *w)   { gPass++; Serial.printf("  [ OK ] %s\n", w); }
void fail(const char *w) { gFail++; Serial.printf("  [FAIL] %s\n", w); }
void warn(const char *w) { gWarn++; Serial.printf("  [WARN] %s\n", w); }

/* ========================= 1. 芯片与 Flash 信息 ========================== */

void testChipInfo() {
  title("1. 芯片与 Flash 信息");

  Serial.printf("  芯片 ID        : %08X\n", ESP.getChipId());
  Serial.printf("  核心版本       : %s\n", ESP.getCoreVersion().c_str());
  Serial.printf("  SDK 版本       : %s\n", ESP.getSdkVersion());
  Serial.printf("  CPU 频率       : %u MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("  Flash 型号 ID  : %08X\n", ESP.getFlashChipId());
  Serial.printf("  Flash 实际容量 : %u KB\n", ESP.getFlashChipRealSize() / 1024);
  Serial.printf("  Flash 配置容量 : %u KB   <- 来自 IDE 的 Flash Size 选项\n",
                ESP.getFlashChipSize() / 1024);
  Serial.printf("  Flash 工作模式 : %u   (0=QIO 1=QOUT 2=DIO 3=DOUT)\n",
                ESP.getFlashChipMode());
  Serial.printf("  程序占用       : %u KB\n", ESP.getSketchSize() / 1024);
  Serial.printf("  剩余程序空间   : %u KB\n", ESP.getFreeSketchSpace() / 1024);

  // 关键检查：实测容量 vs IDE 设置容量，不一致说明 IDE 里 Flash Size 选错了
  if (ESP.getFlashChipRealSize() == ESP.getFlashChipSize()) {
    ok("Flash 容量：实测与 IDE 设置一致");
  } else {
    fail("Flash 容量不一致 —— 请在 IDE「工具 > Flash Size」里改成实测容量，否则 OTA / 文件系统会出错");
  }
}

/* ========================= 2. 复位原因 ================================== */

void testResetReason() {
  title("2. 复位原因");

  String r = ESP.getResetReason();
  Serial.printf("  上次复位原因   : %s\n", r.c_str());
  Serial.printf("  复位详细信息   : %s\n", ESP.getResetInfo().c_str());

  // 顺序不能反：先判异常，再判正常。
  // 因为 "Software Watchdog" 里同时含 Software，放前面会误判成正常。
  if (r.indexOf("Exception") >= 0 || r.indexOf("Watchdog") >= 0) {
    fail("异常复位（程序崩溃 / 看门狗）—— 空板自检阶段不该出现");
  } else if (r.indexOf("Power") >= 0) {
    ok("正常上电复位");
  } else if (r.indexOf("External") >= 0) {
    ok("外部复位（按了复位键 / 上传时串口自动复位）—— 正常");
  } else if (r.indexOf("Software") >= 0) {
    ok("软件复位（ESP.restart()）—— 正常");
  } else if (r.indexOf("Deep-Sleep") >= 0) {
    ok("深度睡眠唤醒 —— 正常");
  } else {
    warn("未识别的复位原因，把上面那两行贴给我看看");
  }
}

/* ========================= 3. 内存与堆完整性 ============================ */

void testMemory() {
  title("3. 内存与堆完整性");

  Serial.printf("  空闲堆         : %u 字节\n", ESP.getFreeHeap());
  Serial.printf("  最大连续块     : %u 字节\n", ESP.getMaxFreeBlockSize());
  Serial.printf("  堆碎片率       : %u %%\n", ESP.getHeapFragmentation());

  if (ESP.getFreeHeap() > 20000) ok("空闲堆充足");
  else                           warn("空闲堆偏低（<20KB）");

  // 分配 → 写入 → 回读 → 释放，验证 RAM 读写正确
  const int CHUNK = 512, N = 8;
  void *p[N];
  int got = 0;
  for (int i = 0; i < N; i++) {
    p[i] = malloc(CHUNK);
    if (p[i]) got++; else break;
  }
  for (int i = 0; i < got; i++) memset(p[i], (uint8_t)(0xA5 + i), CHUNK);

  bool pass = (got == N);
  for (int i = 0; i < got && pass; i++) {
    uint8_t *b = (uint8_t *)p[i];
    for (int j = 0; j < CHUNK; j++) {
      if (b[j] != (uint8_t)(0xA5 + i)) { pass = false; break; }
    }
  }
  for (int i = 0; i < got; i++) free(p[i]);

  Serial.printf("  分配压力测试   : 成功分配 %d/%d 块 × %d 字节\n", got, N, CHUNK);
  if (pass) ok("堆分配 + 读写校验全部通过（RAM 正常）");
  else      fail("堆分配或读写校验失败 —— RAM 可能有问题");
}

/* ========================= 4. 系统时钟 ================================== */

void testMillis() {
  title("4. 系统时钟（millis 走时）");

  unsigned long t0 = millis();
  delay(1000);
  unsigned long dt = millis() - t0;

  Serial.printf("  delay(1000) 实际耗时 : %lu ms\n", dt);
  Serial.printf("  micros() 当前值      : %lu\n", (unsigned long)micros());

  long dev = (long)dt - 1000;
  if (dev < 0) dev = -dev;
  if (dev <= 20) ok("时钟正常（偏差 ≤ 20ms）");
  else           warn("时钟偏差偏大 —— 若 >50ms 需留意晶振/负载电容");
}

/* ========================= 5. GPIO 电平自检 ============================= */

struct PinDef { uint8_t pin; const char *label; const char *note; };

// 注意：不含 GPIO1(TX)，原因见文件头注释
const PinDef PINS[] = {
  {  0, "D3", "屏 DC"       },
  {  2, "D4", "板载LED/按键A" },
  {  3, "RX", "串口 RX"     },
  {  4, "D2", "I2C SDA"     },
  {  5, "D1", "I2C SCL"     },
  { 12, "D6", "按键 B"      },
  { 13, "D7", "屏 MOSI"     },
  { 14, "D5", "屏 SCK"      },
  { 15, "D8", "屏 CS"       },
  { 16, "D0", "屏 RES"      },
};
const uint8_t PIN_COUNT = sizeof(PINS) / sizeof(PINS[0]);

void testGPIO() {
  title("5. GPIO 电平自检");
  Serial.println("  原理：把每个脚设为「输入 + 内部上拉」，再读电平。");
  Serial.println("        读到 HIGH = 该脚没被外部拉低，正常；");
  Serial.println("        读到 LOW  = 有东西在把它往 GND 拉（短路 / 板载下拉）。");
  Serial.println();

  Serial.println("  GPIO   丝印  上拉后   判定      用途");
  rule('-');

  uint8_t badCount = 0;

  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    uint8_t p = PINS[i].pin;
    int lvl;

    if (p == 16) {
      // GPIO16 没有内部上拉，只有 INPUT_PULLDOWN_16
      pinMode(16, INPUT_PULLDOWN_16);
      delay(3);
      lvl = digitalRead(16);
      Serial.printf("  GPIO%-2u %-4s %-8s %-8s %s\n",
                    p, PINS[i].label, lvl ? "HIGH" : "LOW",
                    "无上拉", PINS[i].note);
      Serial.println("         ^ GPIO16 无内部上拉，读到 LOW 属正常");
      pinMode(16, INPUT);
      continue;
    }

    pinMode(p, INPUT);
    delay(2);
    pinMode(p, INPUT_PULLUP);
    delay(5);
    lvl = digitalRead(p);

    const char *verdict;
    if (lvl == HIGH) {
      verdict = "正常";
    } else if (p == 15) {
      verdict = "正常*";        // GPIO15 板载下拉，见下方说明
    } else {
      verdict = "被拉低!";
      badCount++;
    }

    Serial.printf("  GPIO%-2u %-4s %-8s %-8s %s\n",
                  p, PINS[i].label, lvl ? "HIGH" : "LOW",
                  verdict, PINS[i].note);

    pinMode(p, INPUT);   // 恢复高阻，不留驱动
  }

  rule('-');
  Serial.println("  * GPIO15 恒为 LOW 是正常的：板上有个 10k 下拉电阻，");
  Serial.println("    这是为了满足「GPIO15 上电必须为低」的启动要求。");
  Serial.println("  * GPIO0 / GPIO2 应为 HIGH（板载上拉 + 启动要求）。");
  Serial.println("    若按下 FLASH 键时 GPIO0 变 LOW，也是正常的。");
  Serial.println();

  if (badCount == 0) ok("全部 GPIO 电平正常，无短路迹象");
  else               fail("有引脚被异常拉低，请检查是否短路到 GND");
}

/* ========================= 6. ADC / A0 ================================== */

void testADC() {
  title("6. ADC（A0 模拟输入）");

  uint16_t lo = 1023, hi = 0;
  uint32_t sum = 0;
  const int N = 32;
  for (int i = 0; i < N; i++) {
    uint16_t v = analogRead(A0);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    sum += v;
    delay(2);
  }
  uint16_t avg = sum / N;

  float v1 = avg / 1023.0f * 1.0f;              // 裸模块量程 0~1.0V
  float v2 = avg / 1023.0f * (float)ADC_REF_VOLT; // 带分压的开发板量程

  Serial.printf("  原始读数       : 平均 %u  (最小 %u / 最大 %u)\n", avg, lo, hi);
  Serial.printf("  换算电压 ①     : %.3f V   <- 按裸模块量程 0~1.0V\n", v1);
  Serial.printf("  换算电压 ②     : %.3f V   <- 按带分压开发板量程 0~%0.1fV\n",
                v2, ADC_REF_VOLT);

  if (hi - lo > 60) warn("读数波动较大 —— A0 悬空时属正常，接上传感器后会稳定");
  else              ok("ADC 读数稳定");

  Serial.println();
  Serial.println("  说明：A0 悬空时读数在 0~1023 之间漂移是正常现象。");
  Serial.println("        要判断量程，把一个 1.5V 干电池的正极接 A0、负极接 GND：");
  Serial.printf("          · 若换算出约 1.5V  -> 这是带分压的开发板，基准用 %.1f\n", ADC_REF_VOLT);
  Serial.println("          · 若换算出约 0.45V -> 这是裸模块，基准要用 1.0");
}

/* ========================= 7. EEPROM 读写 =============================== */

void testEEPROM() {
  title("7. EEPROM 读写（Flash 非易失存储）");

  EEPROM.begin(EEPROM_SIZE);

  // --- 魔数测试：只在值不对时才写，避免每次开机都擦写 Flash ---
  uint32_t magic = 0;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);

  if (magic != EEPROM_MAGIC_VAL) {
    Serial.printf("  首次运行（读到 0x%08X），正在写入魔数…\n", magic);
    EEPROM.put(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
    bool c = EEPROM.commit();
    Serial.printf("  提交结果       : %s\n", c ? "成功" : "失败");
  }

  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  if (magic == EEPROM_MAGIC_VAL) ok("魔数读写往返正确");
  else                           fail("魔数读写失败 —— Flash 或 EEPROM 模拟有问题");

  // --- 图案测试：32 字节逐字节校验 ---
  bool needWrite = false;
  for (int i = 0; i < 32; i++) {
    if (EEPROM.read(EEPROM_PAT_ADDR + i) != (uint8_t)(i * 7 + 3)) {
      needWrite = true;
      break;
    }
  }
  if (needWrite) {
    for (int i = 0; i < 32; i++) EEPROM.write(EEPROM_PAT_ADDR + i, (uint8_t)(i * 7 + 3));
    EEPROM.commit();
  }

  uint8_t errs = 0;
  for (int i = 0; i < 32; i++) {
    if (EEPROM.read(EEPROM_PAT_ADDR + i) != (uint8_t)(i * 7 + 3)) errs++;
  }
  Serial.printf("  32 字节图案校验: %u 个字节不匹配\n", errs);
  if (errs == 0) ok("32 字节图案往返全部正确");
  else           fail("图案校验失败 —— Flash 写入可能异常");

  EEPROM.end();
}

/* ========================= 8. 板载 LED + PWM ============================ */

void ledWrite(uint16_t duty) {
  // duty: 0=灭, 1023=最亮
#if LED_ACTIVE_LOW
  analogWrite(LED_PIN, 1023 - duty);
#else
  analogWrite(LED_PIN, duty);
#endif
}

void testLED() {
  title("8. 板载 LED 与 PWM");

  pinMode(LED_PIN, OUTPUT);

  Serial.println("  正在闪 5 次…（请盯着板子看）");
  for (int i = 0; i < 5; i++) {
    ledWrite(1023);
    delay(150);
    ledWrite(0);
    delay(150);
  }

  Serial.println("  正在做 PWM 呼吸灯 2 个来回…");
  for (int r = 0; r < 2; r++) {
    for (int d = 0; d <= 1023; d += 8) { ledWrite(d); delay(2); }
    for (int d = 1023; d >= 0; d -= 8) { ledWrite(d); delay(2); }
  }

  ledWrite(0);
  pinMode(LED_PIN, INPUT);

  Serial.printf("  LED 引脚       : GPIO%u（%s电平点亮）\n",
                LED_PIN, LED_ACTIVE_LOW ? "低" : "高");
  ok("LED 与 PWM 已执行 —— 若你看到了闪烁和呼吸，说明这一段完全正常");
  Serial.println("  提示：没看到灯？检查 LED_PIN / LED_ACTIVE_LOW 两个宏是否匹配你的板子。");
}

/* ========================= 9. WiFi 射频扫描 ============================= */

// ★ 注意：ESP8266 的加密枚举值来自 SDK 的 wl_enc_type，和 ESP32 完全不同！
//     直接写数字极易搞错（尤其 5 和 7），所以这里用枚举名，别改回数字。
//     ENC_TYPE_TKIP = 2 -> WPA
//     ENC_TYPE_CCMP = 4 -> WPA2
//     ENC_TYPE_WEP  = 5 -> WEP
//     ENC_TYPE_NONE = 7 -> 开放（明文）
//     ENC_TYPE_AUTO = 8 -> WPA/WPA2 混合
const char *encName(uint8_t t) {
  switch (t) {
    case ENC_TYPE_WEP:  return "WEP";
    case ENC_TYPE_TKIP: return "WPA";
    case ENC_TYPE_CCMP: return "WPA2";
    case ENC_TYPE_NONE: return "开放";
    case ENC_TYPE_AUTO: return "WPA/WPA2";
    default:            return "未知";
  }
}

void testWiFi() {
  title("9. WiFi 射频扫描");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.printf("  本机 MAC 地址  : %s\n", WiFi.macAddress().c_str());
  Serial.println("  正在扫描周边 AP…（约 2~4 秒）");

  int n = WiFi.scanNetworks();

  if (n <= 0) {
    fail("一个 AP 都没扫到 —— 射频部分可能有问题（或环境里确实没有 WiFi）");
    WiFi.scanDelete();
    return;
  }

  Serial.printf("  共发现 %d 个网络：\n\n", n);
  Serial.println("  序号  RSSI   信道  加密         SSID");
  rule('-');

  int shown = (n > 12) ? 12 : n;
  for (int i = 0; i < shown; i++) {
    Serial.printf("  %-4d  %-5d  %-4d  %-10s  %s\n",
                  i + 1,
                  WiFi.RSSI(i),
                  WiFi.channel(i),
                  encName((uint8_t)WiFi.encryptionType(i)),
                  WiFi.SSID(i).c_str());
  }
  if (n > shown) Serial.printf("  … 还有 %d 个未显示\n", n - shown);

  rule('-');
  int best = -127;
  for (int i = 0; i < n; i++) {
    int r = WiFi.RSSI(i);
    if (r > best) best = r;
  }
  Serial.printf("  最强信号 RSSI  : %d dBm\n", best);
  Serial.println("  （-30 极强 / -60 良好 / -80 可用 / -90 以下很差）");

  ok("WiFi 射频工作正常");

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);    // 测完关掉，省电、避免干扰 ADC
  delay(50);
}

/* ========================= 10. I2C 总线扫描 ============================= */

void testI2C() {
  title("10. I2C 总线扫描（GPIO4=SDA / GPIO5=SCL）");

  Wire.begin(4, 5);           // 沿用项目定案引脚
  delay(50);

  Serial.println("  正在扫描 0x01 ~ 0x7E …");
  uint8_t found = 0;
  gHasAHT30 = false;
  gHasRTC   = false;

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("    发现设备: 0x%02X", addr);
      if (addr == 0x38) { Serial.print("   <- AHT30 温湿度"); gHasAHT30 = true; }
      if (addr == 0x51) { Serial.print("   <- PCF8563 RTC");  gHasRTC   = true; }
      if (addr == 0x57) Serial.print("   <- AT24C32 EEPROM");
      if (addr == 0x68) Serial.print("   <- DS3231 / DS1307 RTC");
      Serial.println();
      found++;
    }
    yield();
  }

  // 说明：ESP8266 的 Wire 库没有 end() 方法，扫描完保持总线使能即可，
  //       后续要接模块时直接 beginTransmission() / requestFrom() 就能用。

  if (found == 0) {
    Serial.println("    未发现任何设备");
    warn("当前是空板测试，扫不到设备属正常预期");
    Serial.println("         接线后重跑本项，应能扫到 0x38(AHT30) 和 0x51(PCF8563)。");
  } else {
    Serial.printf("  共发现 %u 个 I2C 设备\n", found);
    ok("I2C 总线通信正常");
  }
}

/* ========================= 11. 引脚监视（接按键后可用）=================== */

void watchPins(uint32_t ms) {
  title("11. 引脚实时监视（10 秒）");
  Serial.printf("  监视对象：GPIO%d 和 GPIO%d\n", WATCH_PIN_A, WATCH_PIN_B);
  Serial.println("  接上按键后，按下去这里就会打印变化。");

  pinMode(WATCH_PIN_A, INPUT_PULLUP);
  pinMode(WATCH_PIN_B, INPUT_PULLUP);

  int lastA = digitalRead(WATCH_PIN_A);
  int lastB = digitalRead(WATCH_PIN_B);
  Serial.printf("  初始状态：GPIO%d=%s  GPIO%d=%s\n\n",
                WATCH_PIN_A, lastA ? "HIGH" : "LOW",
                WATCH_PIN_B, lastB ? "HIGH" : "LOW");

  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    int a = digitalRead(WATCH_PIN_A);
    int b = digitalRead(WATCH_PIN_B);
    if (a != lastA) {
      Serial.printf("  [%6lu ms] GPIO%-2d  %s -> %s\n", millis() - t0,
                    WATCH_PIN_A, lastA ? "HIGH" : "LOW", a ? "HIGH" : "LOW");
      lastA = a;
    }
    if (b != lastB) {
      Serial.printf("  [%6lu ms] GPIO%-2d  %s -> %s\n", millis() - t0,
                    WATCH_PIN_B, lastB ? "HIGH" : "LOW", b ? "HIGH" : "LOW");
      lastB = b;
    }
    delay(5);
    yield();
  }
  pinMode(WATCH_PIN_A, INPUT);
  pinMode(WATCH_PIN_B, INPUT);
  Serial.println("  监视结束。");
}

/* ========================= 12. AHT30 温湿度传感器 ======================== */
/*  AHT20 / AHT21 / AHT30 是同一套协议，可以直接互换。
 *  协议要点（踩过的坑都标出来了）：
 *    1. 命令 0xBE 0x08 0x00 = 初始化/校准，必须发，否则读出来全是 0xFF
 *    2. 命令 0xAC 0x33 0x00 = 触发测量，之后要等 80ms
 *    3. 读 7 字节：状态 + 5 字节数据 + 1 字节 CRC
 *       湿度 20bit = B1<<12 | B2<<4 | B3>>4
 *       温度 20bit = (B3&0x0F)<<16 | B4<<8 | B5
 *       RH% = 原始值 / 2^20 * 100
 *       T℃ = 原始值 / 2^20 * 200 - 50
 *    4. CRC 多项式是 0x31（不是标准的 0x07！），初值 0xFF
 */

#define AHT30_ADDR 0x38

uint8_t aht30Crc(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0xFF;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
    }
  }
  return crc;
}

uint8_t aht30Status() {
  if (Wire.requestFrom((uint8_t)AHT30_ADDR, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

bool aht30Init() {
  Wire.beginTransmission(AHT30_ADDR);
  Wire.write(0xBE); Wire.write(0x08); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(15);                       // 手册：初始化 ≤10ms
  return true;
}

bool aht30Read(float *tempC, float *rhPct, uint8_t *statusOut, bool *crcOk) {
  // 触发一次测量
  Wire.beginTransmission(AHT30_ADDR);
  Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(85);                       // 手册：转换 ≤80ms

  uint8_t buf[7] = {0};
  uint8_t n = Wire.requestFrom((uint8_t)AHT30_ADDR, (uint8_t)7);
  if (n < 6) return false;
  for (uint8_t i = 0; i < n && i < 7; i++) buf[i] = Wire.read();

  *statusOut = buf[0];

  uint32_t rhRaw = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | (buf[3] >> 4);
  uint32_t tRaw  = ((uint32_t)(buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];

  *rhPct = (float)rhRaw / 1048576.0f * 100.0f;
  *tempC = (float)tRaw  / 1048576.0f * 200.0f - 50.0f;

  // 有些兼容片只回 6 字节、没有 CRC 字节，所以校验失败不当作错误
  *crcOk = (n >= 7) && (aht30Crc(buf, 6) == buf[6]);
  return true;
}

void testAHT30() {
  title("12. AHT30 温湿度传感器");
  Wire.begin(4, 5);
  delay(20);

  uint8_t st = aht30Status();
  if (st == 0xFF) { fail("读状态字节失败 —— I2C 无应答"); return; }

  Serial.printf("  状态字节       : 0x%02X   (bit7 忙=%d, bit3 已校准=%d)\n",
                st, (st >> 7) & 1, (st >> 3) & 1);

  if (!(st & 0x08)) {
    Serial.println("  未校准 → 发送初始化命令 0xBE 0x08 0x00 …");
    if (!aht30Init()) { fail("初始化命令发送失败"); return; }
    Serial.printf("  初始化后状态   : 0x%02X\n", aht30Status());
  } else {
    Serial.println("  已处于校准状态，无需初始化");
  }

  float t = 0, rh = 0; uint8_t s2 = 0; bool crcOk = false;
  if (!aht30Read(&t, &rh, &s2, &crcOk)) { fail("读数失败（I2C 无应答或字节数不足）"); return; }

  Serial.printf("  读回状态字节   : 0x%02X\n", s2);
  Serial.printf("  温度           : %.2f °C\n", t);
  Serial.printf("  湿度           : %.2f %%RH\n", rh);
  Serial.printf("  CRC-8 校验     : %s\n",
                crcOk ? "通过" : "不通过（部分兼容片无 CRC 字节，不影响读数）");

  if (t > -40 && t < 85 && rh >= 0 && rh <= 100) ok("温湿度数值在合理量程内");
  else warn("数值超出 AHT30 正常量程，检查接线 / 初始化");
}

void aht30Loop(uint8_t times) {
  title("AHT30 连续读数");
  Wire.begin(4, 5);
  Serial.println("  序号   温度(°C)   湿度(%RH)   状态");
  rule('-');
  float sumT = 0, sumH = 0; uint8_t good = 0, st = 0;
  for (uint8_t i = 1; i <= times; i++) {
    float t = 0, rh = 0; bool crcOk = false;
    if (aht30Read(&t, &rh, &st, &crcOk)) {
      Serial.printf("  %-4u   %-9.2f  %-9.2f  0x%02X\n", i, t, rh, st);
      sumT += t; sumH += rh; good++;
    } else {
      Serial.printf("  %-4u   读取失败\n", i);
    }
    if (i < times) delay(1000);
  }
  rule('-');
  if (good) {
    Serial.printf("  平均：%.2f °C  /  %.2f %%RH   （成功 %u/%u 次）\n",
                  sumT / good, sumH / good, good, times);
    ok("AHT30 读数正常");
  } else {
    fail("全部读取失败");
  }
}

/* ========================= 13. PCF8563 实时时钟 ========================== */
/*  寄存器：0x00 控制1 / 0x01 控制2 / 0x02~0x08 时间(秒分时日月周年, BCD)
 *          0x0D CLKOUT控制 / 0x0E 定时器控制 / 0x0F 定时器
 *  注意：0x02 的 bit7 是 VL 标志 —— 为 1 说明振荡器停过，时间不可信
 */

#define PCF8563_ADDR 0x51

uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

// 0=周日 … 6=周六（坂本算法）
uint8_t dayOfWeek(int y, int m, int d) {
  static const int ofs[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (uint8_t)((y + y / 4 - y / 100 + y / 400 + ofs[m - 1] + d) % 7);
}

bool rtcReadRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(PCF8563_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;   // 重复起始，不加停止位
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
  Wire.write(0x02);                       // 从秒寄存器开始，地址自动递增
  Wire.write(dec2bcd(t->sec) & 0x7F);     // 顺便把 VL 位清零
  Wire.write(dec2bcd(t->min) & 0x7F);
  Wire.write(dec2bcd(t->hour) & 0x3F);
  Wire.write(dec2bcd(t->day) & 0x3F);
  Wire.write(t->weekday & 0x07);
  Wire.write((t->century ? 0x80 : 0x00) | (dec2bcd(t->month) & 0x1F));
  Wire.write(dec2bcd(t->year));
  return Wire.endTransmission() == 0;
}

const char *wdName(uint8_t w) {
  static const char *n[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
  return (w < 7) ? n[w] : "??";
}

void printRtc(const RtcTime *t) {
  Serial.printf("  当前时间       : 20%02u-%02u-%02u %s %02u:%02u:%02u\n",
                t->year, t->month, t->day, wdName(t->weekday),
                t->hour, t->min, t->sec);
}

void testRTC() {
  title("13. PCF8563 实时时钟");
  Wire.begin(4, 5);
  delay(20);

  RtcTime t;
  if (!rtcRead(&t)) { fail("读时间寄存器失败 —— I2C 无应答"); return; }

  printRtc(&t);
  Serial.printf("  VL 标志        : %d   %s\n", t.vl ? 1 : 0,
                t.vl ? "(振荡器停过，时间不可信 → 需要重设)" : "(正常，时间可信)");

  // 关掉 CLKOUT：手册说它上电默认启用，关掉让行为确定
  if (rtcWriteReg(0x0D, 0x00)) Serial.println("  CLKOUT         : 已关闭（写 0x0D = 0x00）");
  else                         warn("关闭 CLKOUT 失败");

  // 确保 STOP=0，时钟在走
  if (rtcWriteReg(0x00, 0x00)) Serial.println("  STOP 位        : 已清零（时钟运行中）");
  else                         warn("清 STOP 位失败");

  if (t.vl) {
    warn("时间是脏的，请用命令 t 重新校准");
  } else {
    ok("RTC 通信正常且时间有效");
  }
}

// 用固件编译时间给 RTC 对时。误差 = 从编译到执行命令的时间，通常几十秒内。
// 正式产品应该改成用 WiFi NTP 授时。
void rtcSetFromBuildTime() {
  title("用固件编译时间校准 RTC");
  Wire.begin(4, 5);

  static const char *monNames = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char ms[4] = { __DATE__[0], __DATE__[1], __DATE__[2], '\0' };
  const char *p = strstr(monNames, ms);
  int mo = p ? (int)((p - monNames) / 3 + 1) : 1;

  int yr  = atoi(__DATE__ + 7) % 100;
  int day = atoi(__DATE__ + 4);

  RtcTime t;
  t.hour    = (uint8_t)atoi(__TIME__);
  t.min     = (uint8_t)atoi(__TIME__ + 3);
  t.sec     = (uint8_t)atoi(__TIME__ + 6);
  t.month   = (uint8_t)mo;
  t.day     = (uint8_t)day;
  t.year    = (uint8_t)yr;
  t.century = false;
  t.weekday = dayOfWeek(2000 + yr, mo, day);
  t.vl      = false;

  Serial.printf("  固件编译于     : %s %s\n", __DATE__, __TIME__);
  Serial.printf("  即将写入       : 20%02u-%02u-%02u %s %02u:%02u:%02u\n",
                t.year, t.month, t.day, wdName(t.weekday), t.hour, t.min, t.sec);

  if (!rtcWrite(&t)) { fail("写入失败"); return; }
  delay(10);

  RtcTime chk;
  if (!rtcRead(&chk)) { fail("回读失败"); return; }
  printRtc(&chk);

  if (chk.year == t.year && chk.month == t.month && chk.day == t.day &&
      chk.hour == t.hour && chk.min == t.min) {
    ok("写入并回读一致，RTC 已校准");
  } else {
    fail("回读与写入不一致，检查晶振是否起振（看 VL 标志）");
  }
  Serial.println("  说明：编译时间的误差 = 从编译到执行本命令的间隔。");
  Serial.println("        正式产品应改用 WiFi NTP 授时，精度可达毫秒级。");
}

/* ========================= 一键完整自检 ================================= */

void fullSelfTest() {
  gPass = gFail = gWarn = 0;

  title("ESP8266 空板功能自检");
  Serial.println("  项目：双层 ESP8266 倒数日");
  Serial.printf("  固件编译时间：%s %s\n", __DATE__, __TIME__);
  Serial.println("  接线状态：空板（未连接任何模块）");

  testChipInfo();
  testResetReason();
  testMemory();
  testMillis();
  testGPIO();
  testADC();
  testEEPROM();
  testLED();
  testWiFi();
  testI2C();

  // 扫到设备就顺手把数据也读一遍 —— 只扫到地址不能证明"读得出来"
  if (gHasAHT30) {
    testAHT30();
  } else {
    Serial.println("\n  (未检测到 0x38，跳过第 12 项 AHT30 温湿度读数)");
  }
  if (gHasRTC) {
    testRTC();
  } else {
    Serial.println("\n  (未检测到 0x51，跳过第 13 项 PCF8563 读数)");
  }

  title("自检汇总");
  Serial.printf("  通过 %u 项 ｜ 警告 %u 项 ｜ 失败 %u 项\n\n", gPass, gWarn, gFail);

  if (gFail == 0 && gWarn == 0) {
    Serial.println("  >>> 结果：全部正常，开发板状态良好 <<<");
  } else if (gFail == 0) {
    Serial.println("  >>> 结果：无失败项，警告项多为空板状态下的正常现象 <<<");
  } else {
    Serial.println("  >>> 结果：存在失败项，请按上面的提示逐条排查 <<<");
  }

  Serial.println();
  Serial.println("  提示：上面第 8 项（LED）需要你用眼睛确认，程序无法自动判断。");
  printHelp();
}

/* ========================= 交互菜单 ===================================== */

void printHelp() {
  Serial.println();
  rule('-');
  Serial.println("  串口命令（在监视器里输入字符后回车）");
  rule('-');
  Serial.println("    ?  显示本帮助");
  Serial.println("    1  重跑完整自检");
  Serial.println("    2  芯片与 Flash 信息");
  Serial.println("    3  内存与堆完整性");
  Serial.println("    4  GPIO 电平自检");
  Serial.println("    5  ADC (A0) 读数");
  Serial.println("    6  EEPROM 读写测试");
  Serial.println("    7  LED 闪烁 + PWM 呼吸");
  Serial.println("    8  WiFi 扫描");
  Serial.println("    9  I2C 总线扫描");
  Serial.println("    a  AHT30 连续读 5 次温湿度");
  Serial.println("    c  读 PCF8563 时间");
  Serial.println("    t  用固件编译时间校准 RTC");
  Serial.println("    w  引脚监视 10 秒（接按键后验证用）");
  Serial.println("    r  重启开发板");
  Serial.println("    d  进入深度睡眠（见下方警告）");
  rule('-');
  Serial.println("  ⚠ 关于 'd' 深度睡眠：ESP8266 要用 GPIO16(D0) 接 RST 才能自动唤醒。");
  Serial.println("     开发板默认没接这根线，所以执行后会一直睡着，需手动断电重上电。");
  Serial.println();
}

void handleCommand(char c) {
  switch (c) {
    case '?': case 'h': case 'H':
      printHelp();
      break;

    case '1':
      fullSelfTest();
      break;

    case '2':
      gPass = gFail = gWarn = 0;
      testChipInfo();
      break;

    case '3':
      gPass = gFail = gWarn = 0;
      testMemory();
      break;

    case '4':
      gPass = gFail = gWarn = 0;
      testGPIO();
      break;

    case '5':
      gPass = gFail = gWarn = 0;
      testADC();
      break;

    case '6':
      gPass = gFail = gWarn = 0;
      testEEPROM();
      break;

    case '7':
      gPass = gFail = gWarn = 0;
      testLED();
      break;

    case '8':
      gPass = gFail = gWarn = 0;
      testWiFi();
      break;

    case '9':
      gPass = gFail = gWarn = 0;
      testI2C();
      break;

    case 'a': case 'A':
      aht30Loop(5);
      break;

    case 'c': case 'C': {
      RtcTime t;
      if (rtcRead(&t)) {
        printRtc(&t);
        Serial.printf("  VL 标志        : %s\n",
                      t.vl ? "1（振荡器停过，时间不可信，用 t 命令重设）" : "0（正常）");
      } else {
        Serial.println("\n  读 RTC 失败 —— 检查 I2C 接线\n");
      }
      break;
    }

    case 't': case 'T':
      rtcSetFromBuildTime();
      break;

    case 'w': case 'W':
      watchPins(10000);
      break;

    case 'r': case 'R':
      Serial.println("\n  正在重启…\n");
      Serial.flush();
      delay(100);
      ESP.restart();
      break;

    case 'd': case 'D':
      Serial.println("\n  ⚠ 即将进入深度睡眠 10 秒。");
      Serial.println("     若未把 GPIO16(D0) 接到 RST，板子会一直睡着不再醒来。");
      Serial.println("     要恢复请拔电重插。\n");
      Serial.flush();
      delay(500);
      ESP.deepSleep(10e6);
      break;

    case '\r': case '\n': case ' ':
      break;   // 忽略空白

    default:
      // 只对「可打印字符」报错。
      // 串口工具刚连上时，USB-TTL 常会吐 0x00 / 0xFF 这类杂散字节，
      // 直接忽略即可，否则每次连接都会莫名其妙冒出一行"未知命令"。
      if (c >= 0x20 && c < 0x7F) {
        Serial.printf("\n  未知命令 '%c'  ——  输入 ? 查看帮助\n", c);
      }
      break;
  }
}

/* ========================= setup / loop ================================= */

// 开机自动校时：RTC 时间是脏的（VL=1 或年月日明显不合理）就用固件编译时间写入。
// 用户决定：串口 RX 线不通，放弃串口交互校时；本功能让设备每次冷启动自愈。
// 误差 = 编译到上电的间隔（几十秒~几分钟），对倒数日无感；正式固件将改用 WiFi NTP 授时。
void rtcAutoSyncIfInvalid() {
  Wire.begin(4, 5);
  delay(20);

  RtcTime t;
  bool bad = true;
  if (rtcRead(&t) && !t.vl &&
      t.year >= 25 && t.year <= 35 &&      // 2025~2035 之间视为合理
      t.month >= 1 && t.month <= 12 &&
      t.day   >= 1 && t.day   <= 31) {
    bad = false;                           // 时间可信，不动它
  }

  if (bad) {
    Serial.println("  [RTC] 检测到时间无效（VL 或年月日异常），自动用固件编译时间校准 …");
    rtcSetFromBuildTime();
  } else {
    Serial.print("  [RTC] 时间有效，无需校准：");
    printRtc(&t);
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(600);                 // 等串口稳定，避免开头几个字符丢失

  // 串口工具刚连上的一瞬间，USB-TTL 常会吐出 1~2 个杂散字节。
  // 先冲掉，免得它们被当成命令处理。
  delay(50);
  while (Serial.available()) Serial.read();

  Serial.println();
  Serial.println();

  rtcAutoSyncIfInvalid();     // 先校时，让后面的自检直接报时间正常
  fullSelfTest();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    while (Serial.available()) Serial.read();   // 丢弃多余字符
    // 回显收到的字符 —— 能看到回显 = 板子收到了输入；
    // 看不到回显 = 输入根本没到板子（检查接线 / 监视器设置）
    if (c >= 0x20 && c < 0x7F) Serial.printf("> %c\n", c);
    handleCommand(c);
  }
  delay(10);
}
