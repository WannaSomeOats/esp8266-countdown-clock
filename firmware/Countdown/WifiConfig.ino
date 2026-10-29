/* ============================================================================
 *  WifiConfig.ino —— WiFi 配网模式（与 Countdown.ino 属同一工程）
 * ----------------------------------------------------------------------------
 *  进入：设置页 →「WiFi 设置」→ B 长按
 *  过程：挂起应用层（主 loop 被本模块阻塞）→ AP+STA 双模 → 屏幕二维码
 *        → 手机扫码连热点 → 强制门户网页（扫描周边 WiFi / 输密码 / 保存）
 *        → 连接成功写 EEPROM 并恢复 UI；失败留在配网模式重试
 *  退出：A 或 B 长按随时退出 / 3 分钟无客户端自动退出
 *
 *  依赖：Countdown.ino 里的全局（cfg/tft/配色/ctxt/scanBtn/keyA/keyB/
 *        drawPage/toast/pomoTick 等），以及开头声明的跨文件函数原型。
 * ==========================================================================*/

/* ============================ 配置常量 =================================== */

#define CFG_AP_SSID      "Countdown-Setup"
#define CFG_TIMEOUT_MS   (3UL * 60UL * 1000UL)   // 3 分钟无客户端退出
#define CFG_CONNECT_MS   15000UL                 // 尝试连接家庭 WiFi 的最长等待

static DNSServer        cfgDns;
static ESP8266WebServer cfgServer(80);

static char     cfgApPass[9];          // 热点密码（每台设备固定随机，8 位）
static uint8_t  cfgQrBuf[160];         // QR v3 缓冲区（29×29 位 ≈ 106 字节）
static uint32_t cfgLastClientMs;       // 最近一次有手机连接的时刻
static uint8_t  cfgStatus = 0;         // 0 等待 / 1 有客户端 / 2 连接中 / 3 成功 / 4 失败
static int8_t   cfgStatusDrawn = -1;
static bool     cfgExitReq = false;    // 请求退出（由 /save 处理器置位，主循环处理）
static bool     cfgExitOk  = false;

static void cfgExitConfig(bool success);   // 前置声明（定义在文件后部）

/* ============================ WiFi 状态查询（主界面用）=================== */

// 真实信号强度：0=未连接（4格全暗） 1~4=按 RSSI 分档
uint8_t wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  long r = (long)WiFi.RSSI();
  if (r >= -55) return 4;
  if (r >= -65) return 3;
  if (r >= -75) return 2;
  return 1;
}

// 设置页"WiFi 设置"一栏显示的当前状态
const char *wifiStateText() {
  if (WiFi.status() == WL_CONNECTED) return "已连接";
  if (cfg.wifiOK) return "未连接";
  return "未配置";
}

// 开机自动连接（有凭据时，非阻塞）
void wifiAutoConnect() {
  if (cfg.wifiOK && cfg.ssid[0]) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.pass);
  }
}

/* ============================ 热点密码（每设备随机）====================== */

static bool cfgPassValid() {
  for (uint8_t i = 0; i < 8; i++) {
    char c = cfg.apPass[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '9'))) return false;
  }
  return true;
}

static void cfgEnsureApPass() {
  if (cfgPassValid()) {
    memcpy(cfgApPass, cfg.apPass, 8);
    cfgApPass[8] = 0;
    return;
  }
  // 去掉易混淆字符（0/o/1/l/i）
  const char *cs = "abcdefghjkmnpqrstuvwxyz23456789";
  randomSeed(micros() ^ ESP.getChipId());
  for (uint8_t i = 0; i < 8; i++) cfgApPass[i] = cs[random(31)];
  cfgApPass[8] = 0;
  memcpy(cfg.apPass, cfgApPass, 9);
  settingsSave();
}

/* ============================ 二维码绘制 ================================= */

// 把文本画成二维码；x,y 为主控坐标（左上角），scale 为模块放大倍数
static void drawQR(const char *text, int x, int y, uint8_t scale) {
  QRCode qr;
  if (qrcode_initText(&qr, cfgQrBuf, 3, ECC_LOW, text) != 0) {
    ctxt("二维码生成失败", 64, 60, C_RED, C_BG, MC_DATUM);
    return;
  }
  int px = qr.size * scale;
  tft.fillRect(x - 4, y - 4, px + 8, px + 8, TFT_WHITE);   // 白色静区（必需）
  for (uint8_t j = 0; j < qr.size; j++) {
    for (uint8_t i = 0; i < qr.size; i++) {
      if (qrcode_getModule(&qr, i, j)) {
        tft.fillRect(x + i * scale, y + j * scale, scale, scale, TFT_BLACK);
      }
    }
  }
}

/* ============================ 配网屏幕 =================================== */

static void cfgDrawStatus() {
  if ((int8_t)cfgStatus == cfgStatusDrawn) return;
  cfgStatusDrawn = cfgStatus;

  const char *t; uint16_t col;
  switch (cfgStatus) {
    case 1:  t = "手机已连接·打开网页"; col = C_GREEN;  break;
    case 2:  t = "正在连接 WiFi";       col = C_AMBER;  break;
    case 3:  t = "连接成功";            col = C_GREEN;  break;
    case 4:  t = "连接失败·请重试";     col = C_RED;    break;
    default: t = "等待手机连接";        col = C_HINT;   break;
  }
  tft.fillRect(0, 146, 128, 14, C_BG);
  ctxt(t, 64, 153, col, C_BG, MC_DATUM);
}

static void cfgDrawScreen() {
  tft.fillScreen(C_BG);

  // 顶部：模式提示
  tft.fillRect(0, 0, 128, 13, C_HL);
  ctxt("配网模式", 5, 7, C_BLUE, C_HL, ML_DATUM);
  ctxt("长按退出", 123, 7, C_LBLUE, C_HL, MR_DATUM);

  // 二维码（v3 = 29 模块 × 3 = 87px）
  char qr[72];
  snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", CFG_AP_SSID, cfgApPass);
  drawQR(qr, 20, 24, 3);

  // 热点信息（手动兜底）
  char b[40];
  snprintf(b, sizeof(b), "热点 %s", CFG_AP_SSID);
  ctxt(b, 64, 126, C_FG, C_BG, MC_DATUM);
  snprintf(b, sizeof(b), "密码 %s", cfgApPass);
  ctxt(b, 64, 138, C_SUB, C_BG, MC_DATUM);

  cfgStatusDrawn = -1;
  cfgDrawStatus();
}

/* ============================ 网页（PROGMEM，直接进 Flash 不占堆）======= */

static const char CFG_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Countdown 配网</title><style>
body{font-family:system-ui,-apple-system,sans-serif;background:#0d1520;color:#e8f0f8;margin:0;padding:20px;max-width:430px}
h2{font-size:18px;margin:0 0 14px;font-weight:600}
.n{border:1px solid #263d52;border-radius:10px;padding:11px 13px;margin:8px 0;display:flex;justify-content:space-between;align-items:center}
.n:active{background:#17344f}
.s{color:#5c7691;font-size:12px}
input{width:100%;box-sizing:border-box;background:#16232f;border:1px solid #263d52;color:#e8f0f8;border-radius:8px;padding:11px;font-size:15px;margin:9px 0}
button{width:100%;background:#4fa8ff;border:0;border-radius:8px;padding:12px;font-size:15px;color:#0d1520;font-weight:600;margin-top:4px}
button.sec{background:#263d52;color:#e8f0f8}
.hide{display:none}#msg{margin-top:14px;font-size:14px;line-height:1.5}
.ok{color:#3fd68c}.er{color:#ff5a5a}
</style></head><body>
<h2>设备配网</h2>
<div class="s">选择你家的 WiFi 并输入密码，设备将自动连接</div>
<div id="list" class="s" style="margin-top:12px">正在读取 WiFi 列表...</div>
<button class="sec" onclick="rescan()" style="margin-top:10px">重新扫描</button>
<div id="form">
<div id="sel" style="margin-top:14px;color:#4fa8ff"></div>
<input id="pw" type="password" placeholder="WiFi 密码">
<input id="manual" placeholder="或手动输入 WiFi 名称">
<button onclick="save()">保存并连接</button>
</div>
<div id="msg"></div>
<script>
var cur="";
var render=function(nets){var h="";
nets.forEach(function(n){h+='<div class="n" onclick="sel(this)" data-s="'+n.s.replace(/"/g,'&quot;')+'"><span>'+n.s+'</span><span class="s">'+n.r+' dBm</span></div>'});
document.getElementById('list').innerHTML=h||"未发现网络";};
var poll=function(n){
fetch('/scan',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){
if(s.state=="done"){render(s.nets);return;}
if(n<=0){document.getElementById('list').textContent="未获取到列表，请手动输入 WiFi 名称和密码";return;}
if(s.state=="failed"){document.getElementById('list').textContent="扫描失败，请手动输入 WiFi 名称和密码";}
else{document.getElementById('list').textContent="正在扫描周围 WiFi...";}
setTimeout(function(){poll(n-1)},800);}).catch(function(){
document.getElementById('list').textContent="通信失败，请重试";});};
var doScan=function(url,n){document.getElementById('list').textContent="正在扫描周围 WiFi...";
fetch(url,{cache:'no-store'}).catch(function(){}).then(function(){poll(n);});};
var rescan=function(){doScan('/scan?r=1',30);};
var sel=function(el){cur=el.getAttribute('data-s');document.getElementById('sel').textContent="已选择："+cur;
document.getElementById('manual').value="";};
var save=function(){var ssid=document.getElementById('manual').value||cur;
if(!ssid){alert("请选择或输入 WiFi 名称");return;}
var pw=document.getElementById('pw').value;
document.getElementById('msg').className="";
document.getElementById('msg').textContent="正在连接，请稍候（最多 15 秒）...";
fetch('/save?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pw))
.then(function(r){return r.text()}).then(function(t){
if(t=="ok"){document.getElementById('msg').className="ok";document.getElementById('msg').textContent="连接成功！设备已保存，即将返回主界面。";}
else{document.getElementById('msg').className="er";document.getElementById('msg').textContent="连接失败：密码错误或信号太弱，请重试。";}
}).catch(function(){document.getElementById('msg').className="er";document.getElementById('msg').textContent="通信失败，请重试。";});};
doScan('/scan',30);
</script></body></html>)HTML";

/* =============== WiFi 扫描（非阻塞状态机 + 结果缓存）======================= */
/*
 * 为什么不用 WiFi.scanNetworks() 同步版：
 *   同步版内部用 esp_suspend 等待"扫描完成"回调；在 AP+STA 且手机已连热点的
 *   情况下该回调可能不触发 → HTTP 请求永久挂起（看门狗复位 / 网页显示扫描失败），
 *   且核心库的 _scanStarted 标志不会自动清除，之后每次调用都直接返回 -1。
 * 本实现：
 *   ① 异步启动 + 主循环轮询推进（绝不阻塞 HTTP 处理器）
 *   ② 进入配网立即预扫描一次（此时手机还没连热点，条件最干净）并缓存结果
 *   ③ 超时/失败自动重试；核心库 API 连续失败后降级为直接调 SDK wifi_station_scan
 *   ④ /scan 只读缓存、毫秒级应答
 */

#define CFG_SCAN_NETMAX      16
#define CFG_SCAN_TIMEOUT_MS  8000      // 单次扫描超时
#define CFG_SCAN_RETRY_MS    1500      // 失败后重试间隔
#define CFG_SCAN_MAXTRIES    6         // 自动尝试上限（1~2 走核心库，3~ 走 SDK 直调）
#define CFG_SCAN_PRESCAN_MS  700       // 进入配网后多久开始预扫描

enum { CFG_SCAN_IDLE = 0, CFG_SCAN_RUNNING, CFG_SCAN_DONE, CFG_SCAN_FAILED };

static uint8_t  cfgScanState   = CFG_SCAN_IDLE;
static uint8_t  cfgScanTries   = 0;       // 本轮已尝试次数
static uint8_t  cfgScanMode    = 0;       // 0=核心库 API  1=SDK 直调
static uint32_t cfgScanT0      = 0;       // 当前这次扫描的起点
static uint32_t cfgScanRetryAt = 0;       // 失败后的重试时间点
static uint32_t cfgEnterMs     = 0;       // 进入配网的时刻
static uint8_t  cfgScanCount   = 0;
static char     cfgScanSsid[CFG_SCAN_NETMAX][34];
static int16_t  cfgScanRssi[CFG_SCAN_NETMAX];
static volatile bool    cfgRawDone   = false;
static volatile uint8_t cfgRawCount  = 0;
static volatile uint8_t cfgRawStatus = 0xFF;

// 核心库路径：把结果拷进缓存后立即释放核心库内存
static void cfgScanStoreCore(int n) {
  uint8_t c = 0;
  for (int i = 0; i < n && c < CFG_SCAN_NETMAX; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || s == CFG_AP_SSID) continue;   // 跳过隐藏网络和本机热点
    strncpy(cfgScanSsid[c], s.c_str(), 33);
    cfgScanSsid[c][33] = 0;
    cfgScanRssi[c] = (int16_t)WiFi.RSSI(i);
    c++;
  }
  cfgScanCount = c;
  WiFi.scanDelete();                                 // 释放核心库侧结果（KB 级）
  cfgScanState = CFG_SCAN_DONE;
  Serial.printf("[cfg] scan done (api), %u nets\n", (unsigned)c);
}

// SDK 直调路径：回调里立即抄走数据（SDK 回调返回后链表即失效）
static void cfgRawScanCb(void *arg, STATUS status) {
  uint8_t c = 0;
  if (status == OK && arg) {
    struct bss_info *it = (struct bss_info *)arg;
    while (it && c < CFG_SCAN_NETMAX) {
      if (it->ssid[0] && strcmp((const char *)it->ssid, CFG_AP_SSID) != 0) {
        uint8_t l = 0;
        while (l < 32 && it->ssid[l]) l++;
        memcpy(cfgScanSsid[c], it->ssid, l);
        cfgScanSsid[c][l] = 0;
        cfgScanRssi[c] = (int16_t)it->rssi;
        c++;
      }
      it = STAILQ_NEXT(it, next);
    }
  }
  cfgRawStatus = (uint8_t)status;                    // 先填数据、后置标志
  cfgRawCount  = c;
  cfgRawDone   = true;
}

static void cfgScanFail(const char *why) {
  Serial.printf("[cfg] scan failed: %s (tries=%u heap=%u)\n",
                why, (unsigned)cfgScanTries, ESP.getFreeHeap());
  cfgScanState   = CFG_SCAN_FAILED;
  cfgScanRetryAt = millis() + CFG_SCAN_RETRY_MS;
}

static void cfgScanStart() {
  cfgScanTries++;
  cfgScanMode = (cfgScanTries >= 3) ? 1 : 0;
  cfgScanT0    = millis();
  cfgScanCount = 0;
  Serial.printf("[cfg] scan#%u start (%s) heap=%u\n",
                (unsigned)cfgScanTries, cfgScanMode ? "sdk" : "api", ESP.getFreeHeap());

  if (cfgScanMode == 0) {
    WiFi.scanDelete();
    int8_t r = WiFi.scanNetworks(true, true);          // 异步版，绝不阻塞
    if (r == WIFI_SCAN_RUNNING) { cfgScanState = CFG_SCAN_RUNNING; return; }
    if (r >= 0) { cfgScanStoreCore(r); return; }       // 极快返回的兜底分支
    cfgScanFail("api start rejected");
    return;
  }

  struct scan_config sc;                               // SDK 直调（绕过核心库卡死态）
  memset(&sc, 0, sizeof(sc));
  sc.show_hidden = 1;
  cfgRawDone = false; cfgRawCount = 0; cfgRawStatus = 0xFF;
  if (!wifi_station_scan(&sc, cfgRawScanCb)) { cfgScanFail("sdk start rejected"); return; }
  cfgScanState = CFG_SCAN_RUNNING;
}

// 主循环里持续调用：预扫描、轮询进度、超时与重试（全程非阻塞）
static void cfgScanPoll() {
  if (cfgScanState == CFG_SCAN_RUNNING) {
    if (cfgScanMode == 0) {
      int8_t r = WiFi.scanComplete();
      if (r >= 0) { cfgScanStoreCore(r); return; }
      if (r == WIFI_SCAN_FAILED) { cfgScanFail("api reported failed"); return; }
    } else if (cfgRawDone) {
      if (cfgRawStatus == OK) {
        cfgScanCount = cfgRawCount;
        cfgScanState = CFG_SCAN_DONE;
        Serial.printf("[cfg] scan done (sdk), %u nets\n", (unsigned)cfgScanCount);
      } else {
        cfgScanFail("sdk status != OK");
      }
      return;
    }
    if (millis() - cfgScanT0 > CFG_SCAN_TIMEOUT_MS) cfgScanFail("timeout");
    return;
  }

  if (cfgScanState == CFG_SCAN_IDLE) {
    if (millis() - cfgEnterMs > CFG_SCAN_PRESCAN_MS) cfgScanStart();
    return;
  }

  if (cfgScanState == CFG_SCAN_FAILED &&
      cfgScanTries < CFG_SCAN_MAXTRIES && millis() >= cfgScanRetryAt) {
    cfgScanStart();
  }
}

/* ============================ HTTP 处理 ================================== */

static void cfgHandleRoot() {
  cfgServer.send_P(200, "text/html; charset=utf-8", CFG_HTML);
}

// 给 /scan 应答加防缓存头（部分浏览器会缓存 GET 结果）
static void cfgScanSendJSON(const char *body) {
  cfgServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  cfgServer.send(200, "application/json", body);
}

// /scan：只读缓存（毫秒级应答）；带 ?r=1 时强制重新扫描
static void cfgHandleScan() {
  if (cfgServer.hasArg("r")) {
    cfgScanTries = 0;
    cfgScanMode  = 0;
    if (cfgScanState != CFG_SCAN_RUNNING) {
      cfgScanState = CFG_SCAN_IDLE;                     // 下一轮 poll 立即开始
      cfgEnterMs   = millis() - CFG_SCAN_PRESCAN_MS - 1;
    }
  }

  if (cfgScanState == CFG_SCAN_DONE) {
    static char buf[1150];
    int p = snprintf(buf, sizeof(buf), "{\"state\":\"done\",\"nets\":[");
    bool first = true;
    for (uint8_t i = 0; i < cfgScanCount; i++) {
      char ss[40]; uint8_t k = 0;
      const char *s = cfgScanSsid[i];
      for (; *s && k < 36; s++) {
        char c = *s;
        if (c == '"' || c == '\\') ss[k++] = '_';
        else if ((uint8_t)c >= 0x20) ss[k++] = c;
      }
      ss[k] = 0;
      if (k == 0) continue;
      if (p > (int)sizeof(buf) - 90) break;
      p += snprintf(buf + p, sizeof(buf) - p, "%s{\"s\":\"%s\",\"r\":%d}",
                    first ? "" : ",", ss, (int)cfgScanRssi[i]);
      first = false;
    }
    snprintf(buf + p, sizeof(buf) - p, "]}");
    cfgScanSendJSON(buf);
    return;
  }
  cfgScanSendJSON(cfgScanState == CFG_SCAN_FAILED
                  ? "{\"state\":\"failed\"}"
                  : "{\"state\":\"running\"}");
}

// 保存并尝试连接。注意：连接测试期间不调用 handleClient（避免重入），
// 浏览器端 fetch 本来就在等这个响应，暂停 15 秒是可接受的。
static void cfgHandleSave() {
  String ssid = cfgServer.arg("ssid");
  String pass = cfgServer.arg("pass");
  if (ssid.length() == 0 || ssid.length() > 32) {
    cfgServer.send(200, "text/plain", "fail");
    return;
  }

  cfgStatus = 2; cfgDrawStatus();          // 屏幕：正在连接

  WiFi.persistent(false);
  WiFi.begin(ssid.c_str(), pass.c_str());

  bool ok = false;
  uint32_t t0 = millis();
  while (millis() - t0 < CFG_CONNECT_MS) {
    if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
    delay(50);
  }

  if (ok) {
    strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
    cfg.ssid[sizeof(cfg.ssid) - 1] = 0;
    strncpy(cfg.pass, pass.c_str(), sizeof(cfg.pass) - 1);
    cfg.pass[sizeof(cfg.pass) - 1] = 0;
    cfg.wifiOK = 1;
    settingsSave();
    cfgStatus = 3; cfgDrawStatus();        // 屏幕：连接成功
    cfgServer.send(200, "text/plain", "ok");
    delay(2000);                            // 让用户看到"连接成功"
    cfgExitReq = true;                      // 不在处理器内直接停服务，交给主循环
    cfgExitOk  = true;
  } else {
    WiFi.disconnect();
    cfgStatus = 4; cfgDrawStatus();        // 屏幕：连接失败
    cfgServer.send(200, "text/plain", "fail");
    delay(3000);
    cfgStatus = (WiFi.softAPgetStationNum() > 0) ? 1 : 0;
    cfgDrawStatus();
  }
}

// 强制门户：一切未知路径 302 到配置页（触发手机"登录网络"提示）
static void cfgHandleNotFound() {
  char loc[40];
  snprintf(loc, sizeof(loc), "http://%s/", WiFi.softAPIP().toString().c_str());
  cfgServer.sendHeader("Location", loc, true);
  cfgServer.send(302, "text/plain", "");
}

/* ============================ 进入 / 退出 ================================ */

// 退出配网、恢复应用层（success=true 表示已连上家庭 WiFi）
static void cfgExitConfig(bool success) {
  cfgServer.stop();
  cfgDns.stop();
  WiFi.softAPdisconnect(true);

  if (success) {
    WiFi.mode(WIFI_STA);                    // 保持连接（未来 NTP/天气要用）
  } else if (cfg.wifiOK) {
    WiFi.mode(WIFI_STA);                    // 有旧凭据 → 保持 STA
    if (WiFi.status() != WL_CONNECTED) WiFi.begin(cfg.ssid, cfg.pass);
  } else {
    WiFi.mode(WIFI_OFF);                    // 无凭据 → 关射频省电
  }

  // ---- 恢复应用层 ----
  page = P_SET; op = false; opLevel = 0;
  pomoTick = millis();                      // ⚠ 防止番茄钟补扣配网期间的时间
  lastActivity = millis();
  lastMinuteDrawn = 255;                    // 强制下次刷新状态栏
  cfgStatusDrawn = -1;
  drawPage();
  toast(success ? "WiFi 已连接" : "已退出配网", 1200);
  Serial.printf("[wifi] config exit, success=%d\n", success ? 1 : 0);
}

// 进入配网模式：阻塞运行，直到退出（主 loop 在此期间不执行 = 应用层挂起）
void wifiEnterConfig() {
  Serial.println("[wifi] enter config mode");

  cfgEnsureApPass();
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);                   // 双模：AP 服务手机 + STA 扫描/连接
  WiFi.softAP(CFG_AP_SSID, cfgApPass);
  delay(150);

  cfgServer.on("/",     cfgHandleRoot);
  cfgServer.on("/scan", cfgHandleScan);
  cfgServer.on("/save", cfgHandleSave);
  cfgServer.onNotFound(cfgHandleNotFound);  // 强制门户兜底
  cfgServer.begin();
  cfgDns.start(53, "*", WiFi.softAPIP());   // DNS 劫持 → 自动弹配置页

  cfgDrawScreen();
  cfgStatus = 0;
  cfgStatusDrawn = -1;
  cfgDrawStatus();
  cfgExitReq = false;
  cfgExitOk  = false;
  cfgLastClientMs = millis();
  cfgScanState = CFG_SCAN_IDLE;             // 重置扫描状态机（进入即预扫描）
  cfgScanTries = 0;
  cfgScanMode  = 0;
  cfgScanCount = 0;
  cfgEnterMs   = millis();
  Serial.printf("[wifi] AP \"%s\" pass \"%s\" ip %s heap %u\n",
                CFG_AP_SSID, cfgApPass,
                WiFi.softAPIP().toString().c_str(), ESP.getFreeHeap());

  uint32_t lastHeapLog = millis();

  while (true) {
    cfgServer.handleClient();
    cfgDns.processNextRequest();
    cfgScanPoll();                          // 扫描状态机（异步推进，绝不阻塞）

    if (cfgExitReq) break;                  // /save 成功后请求退出

    // 手机连接状态
    if (WiFi.softAPgetStationNum() > 0) {
      cfgLastClientMs = millis();
      if (cfgStatus == 0) { cfgStatus = 1; cfgDrawStatus(); }
    } else if (cfgStatus == 1) {
      cfgStatus = 0; cfgDrawStatus();
    }

    // 长按任一键退出（保险起见 A/B 都支持）
    uint32_t nowMs = millis();
    if (scanBtn(&keyA, nowMs) == EV_LONG) break;
    if (scanBtn(&keyB, nowMs) == EV_LONG) break;

    // 3 分钟无客户端 → 超时退出
    if (millis() - cfgLastClientMs > CFG_TIMEOUT_MS) {
      Serial.println("[wifi] timeout");
      break;
    }

    // 每 10 秒打一次堆日志（诊断用）
    if (millis() - lastHeapLog > 10000) {
      lastHeapLog = millis();
      Serial.printf("[wifi] heap %u, clients %u\n",
                    ESP.getFreeHeap(), WiFi.softAPgetStationNum());
    }

    delay(3);
  }

  cfgExitConfig(cfgExitOk);
}
