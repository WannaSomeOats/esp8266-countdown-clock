/* ============================================================================
 *  Weather.ino —— 天气获取 + 天气页（Open-Meteo · HTTP/1.0 明文直连）
 * ----------------------------------------------------------------------------
 *  2026-10-15 讨论定案：
 *    数据源 api.open-meteo.com（免注册免 key）；HTTP 无 TLS、零堆开销。
 *    实测：该服务器对 HTTP/1.0 请求返回「连接关闭定界」的裸 JSON（无 chunked），
 *    可直接读进 1KB 静态缓冲 → 手写 strstr 解析，不引入 JSON 库。
 *  节奏：开机联网就绪 + 每 30 分钟 + 天气页手动；失败保留旧数据、60 秒后重试。
 *  阻塞控制：仅 connect 阶段最长 2.5s（一次性）；读取阶段跨 tick 推进，
 *    不阻塞按键扫描（吸取 NTP / 扫描卡死的教训）。
 *  城市：内置表，天气页操作态切换，存 EEPROM（cfg.cityIdx，默认 0 = 杭州）。
 * ==========================================================================*/

#define WX_HOST       "api.open-meteo.com"
#define WX_STEP_MS    (30UL * 60UL * 1000UL)   // 成功后的刷新间隔
#define WX_RETRY_MS   (60UL * 1000UL)          // 失败后的重试间隔（60s，快速自愈）
#define WX_TIMEOUT_MS 9000UL                   // 读取阶段总超时
#define WX_CONN_MS    2500                     // TCP 连接（含 DNS）超时

/* ============================ 城市表 ===================================== */
/* 纬度/经度 ×100 存整数；索引 0 = 默认城市（杭州） */

struct CityDef { const char *name; int16_t lat100; int16_t lon100; };

static const CityDef WX_CITIES[] = {
  { "杭州", 3025, 12017 },
  { "北京", 3990, 11641 },
  { "上海", 3123, 12147 },
  { "广州", 2313, 11326 },
  { "深圳", 2255, 11406 },
  { "成都", 3057, 10407 },
  { "重庆", 2956, 10655 },
  { "武汉", 3059, 11431 },
  { "西安", 3434, 10894 },
  { "南京", 3206, 11880 },
  { "天津", 3913, 11720 },
  { "苏州", 3130, 12058 },
  { "长沙", 2823, 11294 },
  { "郑州", 3475, 11363 },
  { "青岛", 3607, 12038 },
  { "大连", 3891, 12161 },
  { "厦门", 2448, 11809 },
  { "福州", 2607, 11930 },
  { "昆明", 2488, 10283 },
  { "贵阳", 2665, 10663 },
  { "海口", 2004, 11032 },
  { "哈尔滨", 4580, 12653 },
  { "长春", 4382, 12532 },
  { "沈阳", 4180, 12343 },
  { "济南", 3665, 11712 },
  { "合肥", 3182, 11723 },
  { "南昌", 2868, 11586 },
  { "南宁", 2282, 10832 },
  { "太原", 3787, 11255 },
  { "石家庄", 3804, 11451 },
};
#define WX_CITY_N (sizeof(WX_CITIES) / sizeof(WX_CITIES[0]))

const char *weatherCityName()  { return WX_CITIES[cfg.cityIdx].name; }
uint8_t     weatherCityCount() { return (uint8_t)WX_CITY_N; }

/* ============================ 数据 ======================================= */

struct WeatherData {
  bool    valid;
  uint8_t codeNow;                 // WMO 天气码（现在）
  int8_t  tempNow;                 // 现在温度 °C（四舍五入）
  uint8_t humNow;                  // 相对湿度 %
  uint8_t codeD[3];                // 今天/明天/后天 天气码
  int8_t  maxD[3], minD[3];        // 三天最高 / 最低
  uint8_t updHour, updMin;         // 获取时刻（RTC）
};

static WeatherData wx;

/* ============================ 网络状态 =================================== */

static WiFiClient wxCli;
static char     wxBuf[1200];               // 响应体静态缓冲（实测 ~700B）
static uint16_t wxLen = 0;
static uint8_t  wxState = 0;               // 0=空闲 1=读取中
static uint32_t wxT0 = 0, wxNextMs = 0;
static bool     wxWifiWas = false;
static bool     wxManual  = false;         // 手动刷新 → 结果给 toast
static bool     wxReFetch = false;         // 切换城市 → 本次结果作废、尽快重取
static uint8_t  wxHdr = 0, wxStatusN = 0;
static bool     wxHdrDone = false;
static char     wxStatus[13];              // 状态行前 12 字节（判 "200"）

/* ============================ WMO 码映射 ================================= */
/* 7 类：0晴 1多云 2阴 3雾 4雨 5雪 6雷；文案统一 2 字（排版宽度约束） */

static uint8_t wmoCat(uint8_t code) {
  if (code <= 1) return 0;
  if (code == 2) return 1;
  if (code == 3) return 2;
  if (code == 45 || code == 48) return 3;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return 4;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 5;
  if (code >= 95) return 6;
  return 2;
}

static const char *wmoText(uint8_t code) {
  if (code <= 1) return "晴";
  if (code == 2)  return "多云";
  if (code == 3)  return "阴";
  if (code == 45 || code == 48) return "雾";
  if (code >= 51 && code <= 57) return "小雨";
  if (code == 61) return "小雨";
  if (code == 63) return "中雨";
  if (code == 65) return "大雨";
  if (code == 66 || code == 67) return "冻雨";
  if (code == 71 || code == 77) return "小雪";
  if (code == 73) return "中雪";
  if (code == 75) return "大雪";
  if (code >= 80 && code <= 82) return "阵雨";
  if (code == 85 || code == 86) return "阵雪";
  if (code >= 95) return "雷雨";
  return "阴";
}

/* ============================ 图标（圆/线段绘制）========================= */

static void wxSun(int cx, int cy, uint16_t col) {
  tft.fillCircle(cx, cy, 9, col);
  for (uint8_t i = 0; i < 8; i++) {
    float a = i * 0.7853982f;
    tft.drawLine(cx + (int)(cosf(a) * 11.5f), cy + (int)(sinf(a) * 11.5f),
                 cx + (int)(cosf(a) * 16.0f), cy + (int)(sinf(a) * 16.0f), col);
  }
}

static void wxCloud(int cx, int cy, uint16_t col) {
  tft.fillCircle(cx - 8, cy, 7, col);
  tft.fillCircle(cx + 2, cy - 4, 9, col);
  tft.fillCircle(cx + 12, cy, 6, col);
  tft.fillRect(cx - 8, cy, 21, 7, col);
}

// 以 (cx,cy) 为中心绘制天气图标；dim=true 用灰色（用于"尚无数据"占位）
static void wxDrawIcon(int cx, int cy, uint8_t cat, bool dim) {
  uint16_t cl = dim ? C_HINT : C_SOFT;
  uint16_t su = dim ? C_HINT : C_AMBER;
  switch (cat) {
    case 0:                                   // 晴
      wxSun(cx, cy, su);
      break;
    case 1:                                   // 多云（小太阳 + 云）
      wxSun(cx - 10, cy - 8, su);
      wxCloud(cx + 2, cy + 3, cl);
      break;
    case 2:                                   // 阴
      wxCloud(cx, cy, cl);
      break;
    case 3:                                   // 雾（云 + 横线）
      wxCloud(cx, cy - 4, cl);
      tft.fillRect(cx - 16, cy + 10, 32, 2, C_HINT);
      tft.fillRect(cx - 11, cy + 15, 22, 2, C_HINT);
      break;
    case 4:                                   // 雨（云 + 三条雨丝）
      wxCloud(cx, cy - 4, cl);
      for (int8_t i = -1; i <= 1; i++)
        tft.drawLine(cx + i * 11 - 2, cy + 9, cx + i * 11 + 2, cy + 15, C_BLUE);
      break;
    case 5:                                   // 雪（云 + 雪点）
      wxCloud(cx, cy - 4, cl);
      for (int8_t i = -1; i <= 1; i++) {
        tft.fillCircle(cx + i * 11, cy + 10, 2, C_LBLUE);
        tft.fillCircle(cx + i * 11 + 3, cy + 16, 1, C_LBLUE);
      }
      break;
    case 6:                                   // 雷雨（云 + 闪电）
      wxCloud(cx, cy - 4, cl);
      tft.drawLine(cx - 1, cy + 7, cx + 5, cy + 7, C_AMBER);
      tft.drawLine(cx + 5, cy + 7, cx, cy + 14, C_AMBER);
      tft.drawLine(cx, cy + 14, cx + 5, cy + 14, C_AMBER);
      tft.drawLine(cx + 5, cy + 14, cx - 2, cy + 21, C_AMBER);
      break;
    default:
      wxCloud(cx, cy, cl);
      break;
  }
}

/* ============================ JSON 解析（strstr）========================= */

static const char *wxScalar(const char *start, const char *key) {
  const char *p = strstr(start, key);
  if (!p) return NULL;
  p += strlen(key);
  while (*p == ' ' || *p == ':') p++;
  return p;
}

static const char *wxArr(const char *start, const char *key) {
  const char *p = strstr(start, key);
  if (!p) return NULL;
  p += strlen(key);
  while (*p == ' ' || *p == ':') p++;
  return (*p == '[') ? p + 1 : NULL;
}

static uint8_t wxParseInts(const char *p, uint8_t *out, uint8_t maxn) {
  uint8_t n = 0;
  while (n < maxn && p && *p && *p != ']') {
    out[n++] = (uint8_t)atoi(p);
    p = strchr(p, ',');
    if (!p) break;
    p++;
  }
  return n;
}

static uint8_t wxParseFloats(const char *p, float *out, uint8_t maxn) {
  uint8_t n = 0;
  while (n < maxn && p && *p && *p != ']') {
    out[n++] = atof(p);
    p = strchr(p, ',');
    if (!p) break;
    p++;
  }
  return n;
}

static bool wxParse() {
  WeatherData w;
  memset(&w, 0, sizeof(w));

  const char *p = strstr(wxBuf, "\"current\":{");
  if (!p) return false;
  const char *v;
  v = wxScalar(p, "\"temperature_2m\"");       if (!v) return false;
  w.tempNow = (int8_t)lroundf(atof(v));
  v = wxScalar(p, "\"weather_code\"");         if (!v) return false;
  w.codeNow = (uint8_t)atoi(v);
  v = wxScalar(p, "\"relative_humidity_2m\""); if (!v) return false;
  w.humNow  = (uint8_t)atoi(v);

  p = strstr(wxBuf, "\"daily\":{");
  if (!p) return false;
  v = wxArr(p, "\"weather_code\"");            if (!v) return false;
  if (wxParseInts(v, w.codeD, 3) != 3) return false;
  float f[3];
  v = wxArr(p, "\"temperature_2m_max\"");      if (!v) return false;
  if (wxParseFloats(v, f, 3) != 3) return false;
  for (uint8_t i = 0; i < 3; i++) w.maxD[i] = (int8_t)lroundf(f[i]);
  v = wxArr(p, "\"temperature_2m_min\"");      if (!v) return false;
  if (wxParseFloats(v, f, 3) != 3) return false;
  for (uint8_t i = 0; i < 3; i++) w.minD[i] = (int8_t)lroundf(f[i]);

  w.updHour = now.hour;
  w.updMin  = now.min;
  w.valid   = true;
  memcpy(&wx, &w, sizeof(w));
  return true;
}

/* ============================ 网络任务 =================================== */

static void wxSendRequest() {
  const CityDef &ct = WX_CITIES[cfg.cityIdx];
  // ⚠ 本请求实测长 242 字节（含结尾 \r\n\r\n）——
  //   缓冲区必须留足，否则 snprintf 截断会砍掉 HTTP 头部的结束空行，
  //   服务器将一直等待后续字节、永不响应（症状：http=0 hdr=0 len=0）。
  char req[320];
  int n = snprintf(req, sizeof(req),
                   "GET /v1/forecast?latitude=%d.%02d&longitude=%d.%02d"
                   "&current=temperature_2m,weather_code,relative_humidity_2m"
                   "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                   "&forecast_days=3&timezone=Asia%%2FShanghai HTTP/1.0\r\n"
                   "Host: %s\r\n\r\n",
                   ct.lat100 / 100, ct.lat100 % 100,
                   ct.lon100 / 100, ct.lon100 % 100, WX_HOST);
  if (n >= (int)sizeof(req)) {              // 防御：绝不允许再发生截断
    Serial.printf("[wx] FATAL req truncated %d >= %u\n", n, (unsigned)sizeof(req));
  }
  wxCli.print(req);
}

static void wxStart() {
  wxLen = 0; wxHdr = 0; wxHdrDone = false; wxStatusN = 0;
  wxCli.stop();
  wxCli.setTimeout(WX_CONN_MS);              // 同时约束 DNS 与 TCP 连接等待
  Serial.printf("[wx] fetch %s (%d.%02d,%d.%02d)\n",
                weatherCityName(),
                WX_CITIES[cfg.cityIdx].lat100 / 100, WX_CITIES[cfg.cityIdx].lat100 % 100,
                WX_CITIES[cfg.cityIdx].lon100 / 100, WX_CITIES[cfg.cityIdx].lon100 % 100);
  if (!wxCli.connect(WX_HOST, 80)) {
    Serial.println("[wx] connect failed");
    wxNextMs = millis() + WX_RETRY_MS;
    if (wxManual) { wxManual = false; toast("天气更新失败", 1200); }
    return;
  }
  wxSendRequest();
  wxT0    = millis();
  wxState = 1;
}

static void wxStoreResult(bool bodyOk) {
  wxCli.stop();
  wxState = 0;
  bool httpOK = (wxStatusN >= 12 && wxStatus[9] == '2' && wxStatus[10] == '0' && wxStatus[11] == '0');
  bool ok = bodyOk && httpOK && wxHdrDone && (wxLen > 0) && wxParse();

  if (ok) {
    wxNextMs = millis() + WX_STEP_MS;
    Serial.printf("[wx] ok %s: now %dC %s hum %u%% today %d~%d\n",
                  weatherCityName(), (int)wx.tempNow, wmoText(wx.codeNow),
                  (unsigned)wx.humNow, (int)wx.minD[0], (int)wx.maxD[0]);
    if (!op && (page == P_HOME || page == P_WEATHER)) drawPage();
    if (wxManual) { wxManual = false; toast("天气已更新", 1000); }
  } else {
    wxNextMs = millis() + WX_RETRY_MS;
    Serial.printf("[wx] failed (http=%d hdr=%d len=%u)\n",
                  httpOK ? 1 : 0, wxHdrDone ? 1 : 0, (unsigned)wxLen);
    if (wxStatusN > 0) {                    // 附上实际收到的状态行（若有）
      wxStatus[wxStatusN] = 0;
      Serial.printf("[wx] status line: '%s'\n", wxStatus);
    }
    if (wxManual) { wxManual = false; toast("天气更新失败", 1200); }
  }
  if (wxReFetch) { wxReFetch = false; wxNextMs = millis(); }   // 切城市 → 立即重取
}

/* ============================ 对外接口 =================================== */

void weatherManualRefresh() {
  if (WiFi.status() != WL_CONNECTED) { toast("WiFi 未连接", 1200); return; }
  wxManual = true;
  wxNextMs = millis();
  toast("正在刷新", 900);
}

void weatherRefreshSoon() {
  wxNextMs = millis();
  if (wxState == 1) wxReFetch = true;
}

// 主循环推进（非阻塞；100ms 节拍）
void weatherTick() {
  static uint32_t lastCheck = 0;
  uint32_t ms = millis();
  if (ms - lastCheck < 100) return;
  lastCheck = ms;

  bool wifiNow = (WiFi.status() == WL_CONNECTED);

  if (wifiNow && !wxWifiWas) {               // WiFi 首连/重连 → 立即安排一次
    wxWifiWas = true;
    wxNextMs  = ms;
    Serial.println("[wx] wifi up, fetch scheduled");
  }
  if (!wifiNow) {
    wxWifiWas = false;
    if (wxState == 1) { wxCli.stop(); wxState = 0; }
    return;
  }

  if (wxState == 0) {
    if ((int32_t)(ms - wxNextMs) >= 0) wxStart();
    return;
  }

  // ---- 读取阶段：跨 tick 推进，不阻塞按键 ----
  while (wxCli.available() && wxLen < sizeof(wxBuf) - 1) {
    char ch = (char)wxCli.read();
    if (!wxHdrDone) {
      if (wxStatusN < 12) wxStatus[wxStatusN++] = ch;   // 收集状态行判 200
      if (wxHdr == 0)      wxHdr = (ch == '\r') ? 1 : 0;
      else if (wxHdr == 1) wxHdr = (ch == '\n') ? 2 : 0;
      else if (wxHdr == 2) wxHdr = (ch == '\r') ? 3 : 0;
      else                 { if (ch == '\n') wxHdrDone = true; wxHdr = 0; }
    } else {
      wxBuf[wxLen++] = ch;
    }
  }

  if (!wxCli.available() && !wxCli.connected()) {
    wxStoreResult(true);                     // 连接关闭定界 → 读完
    return;
  }
  if (ms - wxT0 > WX_TIMEOUT_MS) wxStoreResult(false);
}

/* ============================ 天气页（含操作态）========================== */

void drawWeather() {
  tft.fillScreen(C_BG);
  drawStatusBar();

  // ---- 操作态菜单（level 0）：立即刷新 / 切换城市 ----
  if (op && opLevel == 0) {
    ctxt("天气菜单", 64, 26, C_SUB, C_BG, MC_DATUM);
    const char *items[2] = { "立即刷新", "切换城市" };
    for (uint8_t i = 0; i < 2; i++) {
      int y = 56 + i * 26;
      if (i == cursor) {
        tft.fillRect(14, y - 9, 100, 20, C_HL);
        ctxt(items[i], 26, y, C_FG, -1, ML_DATUM);
        ctxt("‹", 106, y, C_BLUE, -1, MC_DATUM);
      } else {
        ctxt(items[i], 26, y, C_SUB, C_BG, ML_DATUM);
      }
    }
    drawDots();
    drawBanner();
    return;
  }

  bool citySel = (op && opLevel == 1);

  // ---- 顶行：城市 + 更新时间 ----
  if (citySel) {
    char b[26];
    snprintf(b, sizeof(b), "‹ %s ›", weatherCityName());
    ctxt(b, 64, 23, C_ORANGE, C_BG, MC_DATUM);
  } else {
    ctxt(weatherCityName(), 10, 23, C_FG, C_BG, ML_DATUM);
    char b[16];
    if (wx.valid) snprintf(b, sizeof(b), "%02u:%02u 更新", wx.updHour, wx.updMin);
    else          snprintf(b, sizeof(b), "--:-- 更新");
    ctxt(b, 118, 23, C_HINT, C_BG, MR_DATUM);
  }

  // ---- 中部：图标 + 现在 + 今日范围 ----
  char b[26];
  if (wx.valid) {
    wxDrawIcon(64, 50, wmoCat(wx.codeNow), false);
    snprintf(b, sizeof(b), "%s %d°", wmoText(wx.codeNow), (int)wx.tempNow);
    ctxt(b, 64, 82, C_FG, C_BG, MC_DATUM);
    snprintf(b, sizeof(b), "%d° ~ %d°", (int)wx.minD[0], (int)wx.maxD[0]);
    ctxt(b, 64, 98, C_SUB, C_BG, MC_DATUM);
  } else {
    wxDrawIcon(64, 50, 2, true);
    ctxt("尚未获取天气", 64, 82, C_HINT, C_BG, MC_DATUM);
    ctxt("B长按 可立即刷新", 64, 98, C_HINT, C_BG, MC_DATUM);
  }

  tft.fillRect(14, 106, 100, 1, C_LINE);

  // ---- 三天预报 ----
  const char *dayNames[3] = { "今天", "明天", "后天" };
  for (uint8_t i = 0; i < 3; i++) {
    int y = 116 + i * 14;
    ctxt(dayNames[i], 10, y, C_HINT, C_BG, ML_DATUM);
    char t1[10], t2[14];
    if (wx.valid) {
      snprintf(t1, sizeof(t1), "%s", wmoText(wx.codeD[i]));
      snprintf(t2, sizeof(t2), "%d~%d°", (int)wx.minD[i], (int)wx.maxD[i]);
    } else {
      snprintf(t1, sizeof(t1), "--");
      snprintf(t2, sizeof(t2), "--");
    }
    ctxt(t1, 48, y, C_SOFT, C_BG, ML_DATUM);
    ctxt(t2, 118, y, C_SOFT, C_BG, MR_DATUM);
  }

  drawDots();
  if (op) drawBanner();
}

/* ============================ 主页天气两行 =============================== */

void wxDrawHomeLines() {
  char b[32];
  if (wx.valid) {
    snprintf(b, sizeof(b), "%s %d°~%d°", wmoText(wx.codeNow), (int)wx.minD[0], (int)wx.maxD[0]);
    ctxt(b, 64, 122, C_SUB, C_BG, MC_DATUM);
    snprintf(b, sizeof(b), "室外天气 %02u:%02u 更新", wx.updHour, wx.updMin);
    ctxt(b, 64, 138, C_HINT, C_BG, MC_DATUM);
  } else {
    ctxt("室外天气 --", 64, 122, C_HINT, C_BG, MC_DATUM);
    ctxt("等待联网获取", 64, 138, C_HINT, C_BG, MC_DATUM);
  }
}
