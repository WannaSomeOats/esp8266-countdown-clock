/* ============================================================================
 *  WireCheck.ino —— 双层 ESP8266 倒数日
 *  屏幕 5 根信号线 逐根慢速通断测试（配合万用表使用）
 * ----------------------------------------------------------------------------
 *  用途：屏幕白屏不显示时，判断是「哪一根信号线没通」还是「板子引脚没输出」
 *
 *  用法：
 *    1. 上传本程序
 *    2. 万用表拨到 直流电压档（2V 或 20V 量程），黑笔接 GND
 *    3. 按串口提示，红笔点住【屏幕端】对应引脚
 *    4. 正常线：提示 3.3V 时 ≈3.3V、提示 0V 时 ≈0V
 *       断线/接错：读数不动（≈0V 或乱飘）
 *    5. 按板上 RST 键可重跑一轮
 * ==========================================================================*/

struct PinDef {
  uint8_t     pin;        // ESP8266 GPIO
  const char *board;      // 开发板丝印
  const char *screen;     // 屏幕丝印
};

const PinDef PINS[] = {
  { 16, "D0", "RES" },
  { 0,  "D3", "DC"  },
  { 15, "D8", "CS"  },
  { 14, "D5", "SCL" },
  { 13, "D7", "SDA" },
};
const uint8_t N = sizeof(PINS) / sizeof(PINS[0]);
const uint16_t HOLD_MS = 4000;      // 每相电平保持时间（给万用表反应时间）

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=====================================================");
  Serial.println("  屏幕线路通断测试（配合万用表）");
  Serial.println("=====================================================");
  Serial.println("  先做两件事：");
  Serial.println("   1. 黑表笔接 GND，红表笔量屏幕 VDD 脚 → 应≈3.3V");
  Serial.println("      （背光常亮已说明电源正常，这步只是确认）");
  Serial.println("   2. 确认你接的是：SCL->D5  SDA->D7  CS->D8  DC->D3  RES->D0");
  Serial.println();
  Serial.println("  开始逐根测试，注意看串口提示 + 表笔读数：");
  Serial.println();

  for (uint8_t i = 0; i < N; i++) {
    pinMode(PINS[i].pin, OUTPUT);

    Serial.printf(">>> [%u/%u] 屏幕 %-3s 脚（板 %s）: 现在输出 3.3V <<<\n",
                  i + 1, N, PINS[i].screen, PINS[i].board);
    digitalWrite(PINS[i].pin, HIGH);
    delay(HOLD_MS);
    Serial.println("    数到 3.3V 了吗？下面切到 0V ——");
    Serial.println("    现在输出 0V <<<");
    digitalWrite(PINS[i].pin, LOW);
    delay(HOLD_MS);
    Serial.println("    读数变化了吗？");
    Serial.println();
  }

  // 收尾：恢复安全空闲电平（不驱动任何有效信号）
  pinMode(16, OUTPUT); digitalWrite(16, HIGH);   // RST 保持高
  pinMode(15, OUTPUT); digitalWrite(15, HIGH);   // CS 空闲高
  pinMode(0,  OUTPUT); digitalWrite(0,  HIGH);   // DC 高
  pinMode(14, OUTPUT); digitalWrite(14, LOW);    // SCK 低
  pinMode(13, OUTPUT); digitalWrite(13, LOW);    // MOSI 低

  Serial.println("=====================================================");
  Serial.println("  测试完毕。");
  Serial.println("  5 根全部能读到 3.3V/0V 变化 → 线路没问题，");
  Serial.println("     故障在屏幕本身或初始化配置（告诉我，我来查）");
  Serial.println("  有一根读数不动 → 那根线断了或接错，重插/换线");
  Serial.println("  想看芯片侧有没有输出 → 把红笔改点在开发板引脚上再跑一遍");
  Serial.println("  按板上 RST 键可再测一轮。");
  Serial.println("=====================================================");
}

void loop() {
  delay(1000);
}
