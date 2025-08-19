/*********************************************************************
 * Bambu A1 mini – GC9A01 240×240 圆屏 UI
 * ESP32-C3 + Arduino_GFX_Library
 *
 * 功能：
 * - 外圈圆周进度环（12点方向起始、顺时针）
 * - 中心：大百分比 + (压缩后的) current_stage · state + 层数
 * - 下方四徽章：剩余时间、温度、材料、速度/托盘
 * - 基础脏区刷新，降低闪烁
 * - 通过 MQTT 消息更新打印信息
 *********************************************************************/

#define USE_MQTT 1

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "config.h"
#if USE_MQTT
#include <WiFi.h>
#include <PubSubClient.h>
#endif

// ---------------- 屏幕引脚 ----------------
#define TFT_SCLK 4
#define TFT_MOSI 5
#define TFT_DC   7
#define TFT_CS   6
#define TFT_RST  8
#define TFT_BL   0

// ---------------- 颜色（RGB565） ----------------
#define C_BG      0x0000  // 黑
#define C_FG      0xE71C  // 灰白
#define C_MID     0x7BEF  // 中灰
#define C_DIM     0x528A  // 深灰
#define C_RING    0x07E0  // 进度绿
#define C_TRACK   0x5AD6  // 轨道灰
#define C_BADGE   0x2104  // 徽章底(深)
#define C_WARN    0xFD20  // 橙
#define C_ERR     0xF800  // 红

Arduino_DataBus *bus = new Arduino_SWSPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED /*MISO*/);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST /* rst */, 0 /* rotation */, true /* IPS */);

// ---------------- UI 几何 ----------------
const int W = 240, H = 240;
const int CX = 120, CY = 120;
const int R_OUT = 116;         // 外环外半径
const int R_IN  = 100;         // 外环内半径（环厚 ~16px）
const int BADGE_W = 94;
const int BADGE_H = 28;
const int BADGE_R = 10;
const int GAP = 6;

// ---------------- UI 数据 ----------------
struct UIState {
  float progress;           // 0.0~1.0
  char  stage[48];          // 原始 current_stage（可能很长）
  char  state[16];          // Normal / Pause ...
  int   layer_cur, layer_tot;
  char  time_left[12];      // "1h20m"
  int   nozzle, bed;        // 温度
  char  material[16];       // "PLA"
  char  color[16];          // "Orange"
  int   speed_pct;          // 120
  int   tray;               // 1~4
  char  job[24];            // 文件名
} ui, prev_ui;

// ===================== MQTT MODULE (non-blocking, no UI overlay) =====================
#if USE_MQTT
WiFiClient wifi;
PubSubClient mqtt(wifi);
unsigned long next_wifi_try  = 0;
unsigned long next_mqtt_try  = 0;
uint16_t      wifi_retry_ms  = 0;
uint16_t      mqtt_retry_ms  = 0;

static inline void copyTo(char* dst, size_t cap, const String& s) {
  if (cap == 0) return;
  size_t n = s.length();
  if (n >= cap) n = cap - 1;
  memcpy(dst, s.c_str(), n);
  dst[n] = '\0';
}
static inline float normProgress(const String& s) {
  float v = s.toFloat();
  if (v > 1.001f) v /= 100.0f;
  if (v < 0) v = 0; if (v > 1) v = 1;
  return v;
}
static inline bool topicEndsWith(const String& t, const char* suffix) {
  size_t lt = t.length(), ls = strlen(suffix);
  if (ls > lt) return false;
  return memcmp(t.c_str() + (lt - ls), suffix, ls) == 0;
}

void mqttCB(char* topic, byte* payload, unsigned len) {
  String v; v.reserve(len); for (unsigned i=0;i<len;++i) v += (char)payload[i];
  String t(topic);

  // —— 基础映射 ——
  if      (topicEndsWith(t, "print_progress/state"))    { ui.progress = normProgress(v); }
  else if (topicEndsWith(t, "print_status/state"))      { copyTo(ui.state, sizeof(ui.state), v); }
  else if (topicEndsWith(t, "current_stage/state"))     { copyTo(ui.stage, sizeof(ui.stage), v); }
  else if (topicEndsWith(t, "current_layer/state"))     { ui.layer_cur = v.toInt(); }
  else if (topicEndsWith(t, "total_layer_count/state") || topicEndsWith(t, "total_layers/state")) { ui.layer_tot = v.toInt(); }
  else if (topicEndsWith(t, "remaining_time/state") || topicEndsWith(t, "time_remaining/state")) {
    int m = v.toInt(); char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d", m/60, m%60);
    copyTo(ui.time_left, sizeof(ui.time_left), String(buf));
  }
  else if (topicEndsWith(t, "print_job/state") || topicEndsWith(t, "project/state") || topicEndsWith(t, "file/state")) {
    copyTo(ui.job, sizeof(ui.job), v);
  }
  // —— 徽章数据扩展 ——
  else if (topicEndsWith(t, "nozzle_temperature/state") || topicEndsWith(t, "hotend_temp/state")) { ui.nozzle = (int)roundf(v.toFloat()); }
  else if (topicEndsWith(t, "bed_temperature/state")    || topicEndsWith(t, "bed_temp/state"))    { ui.bed    = (int)roundf(v.toFloat()); }
  else if (topicEndsWith(t, "filament_type/state")      || topicEndsWith(t, "material/state"))    { copyTo(ui.material, sizeof(ui.material), v); }
  else if (topicEndsWith(t, "filament_color/state")     || topicEndsWith(t, "color/state"))       { copyTo(ui.color, sizeof(ui.color), v); }
  else if (topicEndsWith(t, "speed_percentage/state")   || topicEndsWith(t, "print_speed/state")) { ui.speed_pct = v.toInt(); }
  else if (topicEndsWith(t, "active_tray/state")        || topicEndsWith(t, "tray/state"))        { ui.tray = v.toInt(); }
}

void ensureWiFi() {
  unsigned long now = millis();
  if (WiFi.status() == WL_CONNECTED) return;
  if (now < next_wifi_try) return;
  if (wifi_retry_ms < 1000) wifi_retry_ms = 1000;
  else if (wifi_retry_ms < 5000) wifi_retry_ms += 1000;
  next_wifi_try = now + wifi_retry_ms;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void ensureMQTT() {
  unsigned long now = millis();
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (now < next_mqtt_try) return;

  if (mqtt_retry_ms == 0) mqtt_retry_ms = 1000;
  else { mqtt_retry_ms = mqtt_retry_ms * 2; if (mqtt_retry_ms > 30000) mqtt_retry_ms = 30000; }
  next_mqtt_try = now + mqtt_retry_ms;

  static bool inited = false;
  if (!inited) {
    mqtt.setServer(MQTT_SERVER, 1883);
    mqtt.setCallback(mqttCB);
    mqtt.setBufferSize(1024);
    inited = true;
  }
  bool ok;
  if (MQTT_USER && strlen(MQTT_USER)) ok = mqtt.connect("ESP32C3", MQTT_USER, MQTT_PASS);
  else ok = mqtt.connect("ESP32C3");
  if (ok) {
    mqtt_retry_ms = 1000;
    mqtt.subscribe("bambu/#");
  }
}
#endif
// ===================== END MQTT MODULE =====================

// ========== 文本工具：替换 / snake_case→Title / 压缩术语 ==========
String strReplaceAll(String s, const String &from, const String &to) {
  if (from.length() == 0) return s;
  int idx = 0;
  while ((idx = s.indexOf(from, idx)) != -1) {
    s = s.substring(0, idx) + to + s.substring(idx + from.length());
    idx += to.length();
  }
  return s;
}

// snake_case -> 单词，首字母大写
String snakeToTitleWords(String s) {
  s.toLowerCase();
  for (size_t i = 0; i < s.length(); ++i) if (s[i] == '_') s[i] = ' ';
  bool cap = true;
  for (size_t i = 0; i < s.length(); ++i) {
    if (cap && isalpha(s[i])) s[i] = toupper(s[i]);
    cap = (s[i] == ' ');
  }
  return s;
}

// 术语压缩：把超长词替换为短写
String compressTerms(String s) {
  struct R { const char* a; const char* b; } rules[] = {
    {"Temperature", "Temp"}, {"Calibration", "Calib"}, {"Calibrating", "Calib"},
    {"Absolute Accuracy", "Abs Acc"}, {"Accuracy", "Acc"},
    {"Identifying", "ID"}, {"Identification", "ID"},
    {"Checking", "Check"}, {"Inspection", "Inspect"}, {"Inspecting", "Inspect"},
    {"Extrusion", "Extr"}, {"Extruder", "Extr"},
    {"Chamber", "Chbr"}, {"Platform", "Plate"}, {"Build Plate", "Plate"},
    {"Filament", "Fil"}, {"Nozzle", "Noz"},
    {"Bed Level", "BedLvl"}, {"Bed Leveling", "BedLvl"},
    {"Cooling", "Cool"}, {"Heating", "Heat"}, {"Heated Bed", "Bed"},
    {"Homing", "Home"}, {"Laser", "Laser"},
    {"Skipped Step", "Skip Step"}, {"First Layer", "1st Layer"},
    {"User Gcode", "User Gcode"}, {"Quick Release", "Quick Rel"},
    {"Bird Eye", "Birdeye"}, {"Birdeye", "Birdeye"}
  };
  for (auto &r : rules) s = strReplaceAll(s, r.a, r.b);
  while (s.indexOf("  ") >= 0) s = strReplaceAll(s, "  ", " ");
  return s;
}

// 生成紧凑 stage（去前缀、术语压缩）
String makeCompactStage(const String &raw) {
  String s = raw; s.toLowerCase();

  const char* prefixes[] = {
    "paused_", "calibrating_", "calibrate_",
    "checking_", "check_", "heating_", "cooling_", "heatbed_", "heated_"
  };
  for (auto p : prefixes) {
    int L = strlen(p);
    if (s.startsWith(p)) { s.remove(0, L); break; }
  }

  s = snakeToTitleWords(s);
  s = compressTerms(s);
  return s;
}

// 在 maxWidth 内自适应字号（2→1）；仍太宽则尾部省略号裁切
void drawFitTextCentered(const String &sIn, int cx, int cy, int maxWidth, uint16_t color) {
  String s = sIn;
  int size = 2;
  int16_t bx, by; uint16_t bw, bh;

  for (;;) {
    gfx->setTextSize(size);
    gfx->getTextBounds((char*)s.c_str(), 0, 0, &bx, &by, &bw, &bh);
    if (bw <= maxWidth || size <= 1) break;
    size--;
  }
  if (bw > maxWidth) {
    String t = s + "...";
    do {
      if (t.length() <= 4) { s = "..."; break; }
      t.remove(t.length() - 4, 1);
      gfx->getTextBounds((char*)t.c_str(), 0, 0, &bx, &by, &bw, &bh);
      s = t;
    } while (bw > maxWidth);
  }

  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(cx - bw/2, cy + bh/2);
  gfx->print(s);
}

// 简单居中（固定字号）
void drawCenteredText(const String &s, int x, int y, int size, uint16_t color) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  int16_t bx, by; uint16_t bw, bh;
  gfx->getTextBounds((char*)s.c_str(), 0, 0, &bx, &by, &bw, &bh);
  gfx->setCursor(x - bw/2, y + bh/2);
  gfx->print(s);
}

// ========== 绘制：圆角徽章 / 厚扇形环 ==========
void fillRoundRect(int x, int y, int w, int h, int r, uint16_t col) {
  gfx->fillRoundRect(x, y, w, h, r, col);
  gfx->drawRoundRect(x, y, w, h, r, C_DIM);
}

void fillThickArc(int cx, int cy, int r_in, int r_out, float start_deg, float sweep_deg, uint16_t col) {
  if (sweep_deg <= 0) return;
  int steps = max(8, (int)(fabs(sweep_deg)));
  float a0 = start_deg * DEG_TO_RAD;
  float da = (sweep_deg * DEG_TO_RAD) / steps;
  for (int i = 0; i < steps; ++i) {
    float a1 = a0 + da * i;
    float a2 = a1 + da;
    int x1o = cx + cosf(a1) * r_out;
    int y1o = cy + sinf(a1) * r_out;
    int x1i = cx + cosf(a1) * r_in;
    int y1i = cy + sinf(a1) * r_in;
    int x2o = cx + cosf(a2) * r_out;
    int y2o = cy + sinf(a2) * r_out;
    int x2i = cx + cosf(a2) * r_in;
    int y2i = cy + sinf(a2) * r_in;
    gfx->fillTriangle(x1o, y1o, x2o, y2o, x1i, y1i, col);
    gfx->fillTriangle(x2o, y2o, x2i, y2i, x1i, y1i, col);
  }
}

// ========== Layer：外环 ==========
void drawRingLayer(float progress) {
  fillThickArc(CX, CY, R_IN, R_OUT, -90, 360, C_TRACK);
  float sweep = constrain(progress, 0.f, 1.f) * 360.0f;
  fillThickArc(CX, CY, R_IN, R_OUT, -90, sweep, C_RING);
}

// ========== Layer：中心（百分比 + stage·state + 层数） ==========
void drawCenterLayer() {
  gfx->fillCircle(CX, CY, R_IN - 6, C_BG);
  int pct = (int)round(ui.progress * 100.0f);
  pct = constrain(pct, 0, 100);
  drawCenteredText(String(pct) + "%", CX, CY - 16, 4, C_RING);

  String stageCompact = makeCompactStage(String(ui.stage));
  String line1 = stageCompact + " · " + String(ui.state);
  drawFitTextCentered(line1, CX, CY + 6, 180, C_FG);

  char buf[24]; snprintf(buf, sizeof(buf), "%d/%d", ui.layer_cur, ui.layer_tot);
  drawCenteredText(String(buf), CX, CY + 26, 2, C_MID);
}

// ========== Layer：四徽章 + 文件名 ==========
void drawBadgesAndFooter() {
  int top = CY + 42;
  gfx->fillRect(0, top - 6, W, H - top + 6, C_BG);

  int col1x = 16;
  int col2x = W - 16 - BADGE_W;
  int row1y = top;
  int row2y = top + BADGE_H + GAP;

  fillRoundRect(col1x, row1y, BADGE_W, BADGE_H, BADGE_R, C_BADGE);
  gfx->setTextSize(2); gfx->setTextColor(C_FG);
  gfx->setCursor(col1x + 10, row1y + 8);
  gfx->print("Time ");
  gfx->print(ui.time_left);

  fillRoundRect(col2x, row1y, BADGE_W, BADGE_H, BADGE_R, C_BADGE);
  gfx->setTextSize(2); gfx->setTextColor(C_FG);
  gfx->setCursor(col2x + 10, row1y + 8);
  gfx->print("Temp ");
  gfx->print(ui.nozzle);
  gfx->print("/");
  gfx->print(ui.bed);

  fillRoundRect(col1x, row2y, BADGE_W, BADGE_H, BADGE_R, C_BADGE);
  gfx->setTextSize(2); gfx->setTextColor(C_FG);
  gfx->setCursor(col1x + 10, row2y + 8);
  gfx->print(ui.material);
  gfx->print(" ");
  gfx->print(ui.color);

  fillRoundRect(col2x, row2y, BADGE_W, BADGE_H, BADGE_R, C_BADGE);
  gfx->setTextSize(2); gfx->setTextColor(C_FG);
  gfx->setCursor(col2x + 10, row2y + 8);
  gfx->print("Spd ");
  gfx->print(ui.speed_pct);
  gfx->print("% T");
  gfx->print(ui.tray);

  drawFitTextCentered(String(ui.job), CX, H - 16, 200, C_MID);
}

// ========== SETUP ==========
void setup() {
#if USE_MQTT
  ensureWiFi();
  ensureMQTT();
#endif
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  gfx->begin();
  gfx->fillScreen(C_BG);

  memset(&ui, 0, sizeof(ui));
  memset(&prev_ui, 0, sizeof(prev_ui));

  drawRingLayer(ui.progress);
  drawCenterLayer();
  drawBadgesAndFooter();

  prev_ui = ui;
}

// ========== LOOP ==========
void loop() {
#if USE_MQTT
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();
#endif

  bool ringDirty   = fabs(ui.progress - prev_ui.progress) > 0.005f;
  bool centerDirty = (ui.layer_cur != prev_ui.layer_cur)
                  || strcmp(ui.stage, prev_ui.stage)
                  || strcmp(ui.state, prev_ui.state);

  bool badgeDirty  = strcmp(ui.time_left, prev_ui.time_left)
                  || ui.nozzle != prev_ui.nozzle
                  || ui.bed    != prev_ui.bed
                  || strcmp(ui.material, prev_ui.material)
                  || strcmp(ui.color, prev_ui.color)
                  || ui.speed_pct != prev_ui.speed_pct
                  || ui.tray != prev_ui.tray
                  || strcmp(ui.job, prev_ui.job);

  if (ringDirty)   drawRingLayer(ui.progress);
  if (centerDirty) drawCenterLayer();
  if (badgeDirty)  drawBadgesAndFooter();

  prev_ui = ui;
}

