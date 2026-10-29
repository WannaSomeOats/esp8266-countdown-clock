/* ============================================================================
 *  NtpSync.ino —— NTP 联网授时（与 Countdown.ino / WifiConfig.ino 同一工程）
 * ----------------------------------------------------------------------------
 *  同步策略：
 *    ① 开机后 WiFi 首次连上 → 立即授时（同时启动 SNTP 客户端）
 *    ② SNTP 客户端自身每小时自动校准【系统时钟】（核心库/lwIP 内置行为）
 *    ③ 每 24 小时把系统时间写入 PCF8563 一次 —— RTC 典型日漂移 1.7~3.5 秒，
 *       24h 内误差肉眼不可见；这是一次 NTP 交互几 KB 流量的零成本策略
 *    ④ 首次同步失败每 30 秒自动重试；成功过但 2 小时无回调 → 重启客户端自愈
 *    ⑤ 设置页「网络授时」B 长按 = 手动立即授时
 *
 *  实现要点：
 *    - configTime() 内部 sntp_stop()+sntp_init()，可反复调用强制重新查询
 *    - settimeofday_cb 在 SNTP 真正写入系统时钟时触发（核心库经 schedule
 *      派发到主循环上下文）；回调只置标志，I2C / 屏幕操作全部留在主循环
 *      （吸取配网扫描卡死的教训：任何外设操作都不放在回调里）
 *    - 北京时间 = UTC + 8h（无夏令时）；写 RTC 时清 VL、保持 STOP=0
 * ==========================================================================*/

#include <coredecls.h>     // settimeofday_cb()
#include <time.h>          // time(), gmtime_r()

#define NTP_TZ_SEC         (8L * 3600L)                 // UTC+8
#define NTP_SERVER1        "ntp.aliyun.com"
#define NTP_SERVER2        "cn.pool.ntp.org"
#define NTP_SERVER3        "pool.ntp.org"

#define NTP_WRITE_STEP_MS  (24UL * 3600UL * 1000UL)     // RTC 写入间隔：24h
#define NTP_FIRST_RETRY_MS (30UL * 1000UL)              // 首次同步失败重试间隔
#define NTP_STALE_MS       (2UL * 3600UL * 1000UL)      // 超过 2h 无回调 → 自愈重启
#define NTP_RESTART_MIN_MS (10UL * 60UL * 1000UL)       // 自愈重启冷却
#define NTP_MANUAL_WAIT_MS (20UL * 1000UL)              // 手动授时等待上限
#define NTP_VALID_EPOCH    1700000000L                  // 2023-11 之后视为有效时间

static bool     ntpWifiWas     = false;
static bool     ntpEverOk      = false;      // 本次开机是否成功写入过 RTC
static bool     ntpManual      = false;      // 手动授时进行中
static uint32_t ntpLastWriteMs = 0;          // 上次写 RTC（millis）
static uint32_t ntpLastCallMs  = 0;          // 上次 SNTP 回调（millis）
static uint32_t ntpLastStartMs = 0;          // 上次启动客户端（millis）
static uint32_t ntpManualT0    = 0;
static volatile bool ntpGotFlag = false;     // SNTP 已校时（回调只置此标志）

/* ---- SNTP 校时回调：只置标志（核心库 schedule 派发上下文，禁止外设操作）---- */
static void ntpOnTimeSet(bool fromSntp) {
  (void)fromSntp;
  ntpGotFlag    = true;
  ntpLastCallMs = millis();
}

void ntpInit() {
  settimeofday_cb(ntpOnTimeSet);
  Serial.println("[ntp] ready (RTC write every 24h)");
}

/* ---- 启动/重启 SNTP 客户端（连上 WiFi、重试、自愈时调用）---- */
static void ntpStartClient(const char *why) {
  ntpGotFlag     = false;               // 丢弃旧会话的残留标志，只认新鲜回调
  ntpLastStartMs = millis();
  configTime(NTP_TZ_SEC, 0, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
  Serial.printf("[ntp] client start (%s)\n", why);
}

/* ---- 把当前系统时间写入 RTC（主循环上下文）---- */
static bool ntpWriteRtc() {
  time_t t = time(nullptr);
  if (t < NTP_VALID_EPOCH) return false;      // 系统时间未同步，拒写

  time_t bj = t + NTP_TZ_SEC;                 // 北京时间
  struct tm tmv;
  gmtime_r(&bj, &tmv);

  RtcTime rt;
  rt.year    = (uint8_t)((tmv.tm_year + 1900) % 100);
  rt.month   = (uint8_t)(tmv.tm_mon + 1);
  rt.day     = (uint8_t)tmv.tm_mday;
  rt.hour    = (uint8_t)tmv.tm_hour;
  rt.min     = (uint8_t)tmv.tm_min;
  rt.sec     = (uint8_t)tmv.tm_sec;
  rt.century = false;
  rt.vl      = false;
  rt.weekday = dayOfWeek(2000 + rt.year, rt.month, rt.day);

  if (!rtcWrite(&rt)) return false;
  rtcWriteReg(0x00, 0x00);                    // 确保 STOP=0（时钟继续走）

  now = rt;                                   // 同步显示用时间
  lastMinuteDrawn = 255;                      // 强制下一 tick 重画状态栏
  Serial.printf("[ntp] RTC <- %04u-%02u-%02u %02u:%02u:%02u\n",
                2000 + rt.year, rt.month, rt.day, rt.hour, rt.min, rt.sec);
  return true;
}

/* ---- 设置页：手动立即授时 ---- */
void ntpManualSync() {
  if (WiFi.status() != WL_CONNECTED) {
    toast("WiFi 未连接", 1200);
    return;
  }
  ntpManual   = true;
  ntpManualT0 = millis();
  ntpStartClient("manual");                   // 重启客户端 = 强制立即查询
  toast("正在授时", 900);
  Serial.println("[ntp] manual sync");
}

/* ---- 设置页「网络授时」状态文字 ---- */
const char *ntpStateText() {
  if (ntpManual)  return "同步中";
  if (ntpEverOk)  return "已同步";
  return "未同步";
}

/* ---- 主循环推进（非阻塞；每 200ms 检查一次）---- */
void ntpTick() {
  static uint32_t lastCheck = 0;
  uint32_t ms = millis();
  if (ms - lastCheck < 200) return;
  lastCheck = ms;

  bool wifiNow = (WiFi.status() == WL_CONNECTED);

  if (wifiNow && !ntpWifiWas) {               // WiFi 首次连上（含重连）→ 启动客户端
    ntpWifiWas = true;
    ntpStartClient("wifi up");
  }
  if (!wifiNow) { ntpWifiWas = false; return; }

  // ---- SNTP 回调到达：按策略决定是否写 RTC ----
  if (ntpGotFlag) {
    ntpGotFlag = false;
    bool want = ntpManual || !ntpEverOk ||
                (uint32_t)(ms - ntpLastWriteMs) >= NTP_WRITE_STEP_MS;
    if (want) {
      if (ntpWriteRtc()) {
        ntpEverOk      = true;
        ntpLastWriteMs = ms;
        if (ntpManual) {
          ntpManual = false;
          toast("授时完成", 1000);
          drawPage();
        }
      } else {
        Serial.println("[ntp] rtc write failed");
        if (ntpManual) {
          ntpManual = false;
          toast("时间写入失败", 1200);
          drawPage();
        }
      }
    }
  }

  // ---- 手动授时超时 ----
  if (ntpManual && ms - ntpManualT0 > NTP_MANUAL_WAIT_MS) {
    ntpManual = false;
    toast("授时失败", 1200);
    drawPage();
    Serial.println("[ntp] manual timeout");
  }

  // ---- 尚未成功过：每 30 秒重启客户端重试 ----
  if (!ntpEverOk && ms - ntpLastStartMs > NTP_FIRST_RETRY_MS) {
    ntpStartClient("retry");
  }

  // ---- 成功过但 2 小时无回调：自愈重启（10 分钟冷却）----
  if (ntpEverOk && ms - ntpLastCallMs > NTP_STALE_MS &&
      ms - ntpLastStartMs > NTP_RESTART_MIN_MS) {
    ntpStartClient("stale");
  }
}
