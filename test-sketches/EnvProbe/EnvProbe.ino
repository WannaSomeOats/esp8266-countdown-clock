/* ============================================================================
 *  EnvProbe.ino —— AHT30 精度 / 自热诊断（自运行，无需串口输入）
 * ----------------------------------------------------------------------------
 *  用途：判断主界面显示的室内温度到底可不可信。
 *
 *  它会每 2 秒打印一行：
 *    [env] 00:12  raw 18 7C 3F E1 8A 2C 5A  st=0x18 busy=0 cal=1  crc=OK  T=28.51C  RH=53.01%
 *      raw  = 传感器原始 7 字节（状态 + 5 数据 + CRC）
 *      st   = 状态字   busy=bit7（1=转换中，数据无效）  cal=bit3（1=已校准）
 *      crc  = CRC-8 校验（多项式 0x31）结果
 *  每 60 秒打印一次统计：这一分钟的 最小 / 最大 / 平均。
 *
 *  怎么用它判断准确性：
 *    1) 断电 10 分钟以上（板子彻底凉下来）再上电，观察前 15~20 分钟：
 *       温度从冷机值持续爬升 1~3°C 后稳定  →  这是开发板自热，不是算法误差
 *    2) 拿一支可信的温度计放在传感器旁边，等 10 分钟，比读数
 *       AHT30 手册标称 ±0.5°C（@25°C），差 0.5 以内都正常
 *    3) 把传感器模块用杜邦线拉远 10cm 再测一次：
 *       数值明显下降  →  确认是板子热量影响，摆放位置要改
 *
 *  串口：115200
 * ==========================================================================*/

#include <Wire.h>

#define AHT30_ADDR 0x38
#define SAMPLE_MS  2000
#define SUM_N      30            // 统计窗口：30 × 2s = 60 秒

static uint32_t lastSample = 0;
static uint32_t bootMs     = 0;

static uint16_t cnt = 0;             // 当前窗口样本数
static float    tMin = 999,  tMax = -999;
static float    hMin = 999,  hMax = -999;
static double   tSum = 0,    hSum = 0;
static uint16_t crcFail = 0, readFail = 0;

static float    gTMin = 999, gTMax = -999;   // 开机以来
static float    gHMin = 999, gHMax = -999;

/* ---------------- CRC-8（多项式 0x31，初值 0xFF）---------------- */
uint8_t crc8(const uint8_t *d, uint8_t n) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

/* ---------------- 初始化 ---------------- */
bool ahtInit() {
  if (Wire.requestFrom((uint8_t)AHT30_ADDR, (uint8_t)1) != 1) {
    Serial.println("[env] no response at 0x38 — check SDA/SCL/power");
    return false;
  }
  uint8_t st = Wire.read();
  Serial.printf("[env] power-on status = 0x%02X  (bit7 busy / bit4 OTP-CRC / bit3 calibrated)\n", st);
  if (!(st & 0x08)) {
    Serial.println("[env] not calibrated -> sending 0xBE 0x08 0x00");
    Wire.beginTransmission(AHT30_ADDR);
    Wire.write(0xBE); Wire.write(0x08); Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    delay(15);
  }
  return true;
}

/* ---------------- 触发一次测量并读回 7 字节 ---------------- */
bool readOnce(uint8_t *buf, uint8_t *n) {
  Wire.beginTransmission(AHT30_ADDR);
  Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(85);                                  // 手册：转换 ≤80ms

  // 忙位轮询：bit7=1 表示还在转换，此时数据无效
  uint8_t st = 0x80;
  for (uint8_t i = 0; i < 4 && (st & 0x80); i++) {
    if (i) delay(20);
    if (Wire.requestFrom((uint8_t)AHT30_ADDR, (uint8_t)1) != 1) return false;
    st = Wire.read();
  }
  if (st & 0x80) return false;

  *n = Wire.requestFrom((uint8_t)AHT30_ADDR, (uint8_t)7);
  if (*n < 6) return false;
  for (uint8_t i = 0; i < *n && i < 7; i++) buf[i] = Wire.read();
  return true;
}

/* ---------------- 60 秒统计输出 ---------------- */
void printStats() {
  if (cnt == 0) return;
  Serial.printf("[env] ---- last 60s: T %.2f~%.2f avg %.2f | RH %.1f~%.1f | boot %.2f~%.2f | crcFail=%u readFail=%u\n",
                tMin, tMax, (float)(tSum / cnt),
                hMin, hMax, gTMin, gTMax,
                (unsigned)crcFail, (unsigned)readFail);
  cnt = 0; tMin = 999; tMax = -999; hMin = 999; hMax = -999; tSum = 0; hSum = 0;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  bootMs = millis();

  Serial.println();
  Serial.println("=========================================================");
  Serial.println("  AHT30 诊断 / 自热观察");
  Serial.println("  1) 断电 10 分钟以上再上电，观察 15~20 分钟温升曲线");
  Serial.println("  2) 用参考温度计并排对比 10 分钟");
  Serial.println("  3) 把传感器模块拉远 10cm 再测一次做对照");
  Serial.println("  串口 115200，每 2 秒一行，每 60 秒一段统计");
  Serial.println("=========================================================");
  Serial.println();

  Wire.begin(4, 5);                     // SDA=D2(4)  SCL=D1(5)
  Wire.setClock(100000);
  if (!ahtInit()) {
    Serial.println("[env] init failed — 程序继续，稍后会自动重试");
  }
  Serial.println("[env] start sampling...");
}

void loop() {
  uint32_t ms = millis();
  if (ms - lastSample < SAMPLE_MS) return;
  lastSample = ms;

  uint8_t buf[7] = { 0 };
  uint8_t n = 0;
  if (!readOnce(buf, &n)) {
    readFail++;
    Serial.printf("[env] %02u:%02u  read FAILED  (crcFail=%u readFail=%u)\n",
                  (unsigned)((ms - bootMs) / 60000UL),
                  (unsigned)(((ms - bootMs) / 1000UL) % 60),
                  (unsigned)crcFail, (unsigned)readFail);
    return;
  }

  uint32_t rhRaw = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | (buf[3] >> 4);
  uint32_t tRaw  = ((uint32_t)(buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];
  float rh = (float)rhRaw / 1048576.0f * 100.0f;
  float tc = (float)tRaw  / 1048576.0f * 200.0f - 50.0f;

  bool crcOk = (n >= 7);
  if (n >= 7) {
    crcOk = (crc8(buf, 6) == buf[6]);
    if (!crcOk) crcFail++;
  }

  Serial.printf("[env] %02u:%02u  raw %02X %02X %02X %02X %02X %02X",
                (unsigned)((ms - bootMs) / 60000UL),
                (unsigned)(((ms - bootMs) / 1000UL) % 60),
                buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
  if (n >= 7) Serial.printf(" %02X", buf[6]);
  else        Serial.print(" --");
  Serial.printf("  st=0x%02X busy=%u cal=%u  crc=%s  T=%6.2f C  RH=%5.2f %%\n",
                buf[0], (buf[0] & 0x80) ? 1 : 0, (buf[0] & 0x08) ? 1 : 0,
                crcOk ? "OK " : "BAD", tc, rh);

  // ---- 统计 ----
  cnt++;
  if (tc < tMin) tMin = tc;
  if (tc > tMax) tMax = tc;
  if (rh < hMin) hMin = rh;
  if (rh > hMax) hMax = rh;
  if (tc < gTMin) gTMin = tc;
  if (tc > gTMax) gTMax = tc;
  if (rh < gHMin) gHMin = rh;
  if (rh > gHMax) gHMax = rh;
  tSum += tc;
  hSum += rh;

  if (cnt >= SUM_N) printStats();
}
