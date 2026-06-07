// claude_usage_display.ino
//
// ESP8266 + SH1106 128x64 OLED. Polls the local usage server over WiFi and
// shows Claude's session/weekly limits across a few auto-rotating pages, with
// a small pixel-Claude that animates the page transitions.
//
// Needs secrets.h (copy secrets.h.example) for WiFi creds and the server URL.

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <math.h>

#include "secrets.h"
#ifndef WIFI_SSID
  #error "secrets.h missing or incomplete. Copy secrets.h.example to secrets.h and fill in."
#endif

// Forward-declared so Arduino's auto-generated prototypes know these types.
enum PageId      : int;
enum ClaudeFrame : int;

// ============================================================
// Config
// ============================================================
static const unsigned long POLL_MS          = 30000;   // full data fetch
static const unsigned long SERVER_CHECK_MS  = 10000;   // cheap alive ping
static const unsigned long PAGE_DURATION    = 7000;    // time on each page
static const unsigned long TRANSITION_MS    = 1500;    // page-flip animation
static const unsigned long BAR_ANIM_MS      = 700;     // bar grow/shrink
static const unsigned long RENDER_FAST_MS   = 22;      // ~45fps while animating
static const unsigned long RENDER_SLOW_MS   = 70;      // ~14fps when idle
static const unsigned long STALE_MS         = 10UL * 60UL * 1000UL;  // data age limit
static const unsigned long WAVE_DURATION    = 2500;    // session-90 celebration
static const unsigned long FRESH_PULSE_MS   = 1600;    // checkmark after a fetch
static const unsigned long WANDER_GAP_MS    = 14000;   // idle-walk interval
static const unsigned long WIFI_BACKOFF_CAP = 60000;   // max reconnect wait

#define MAX_MODELS 6
#define TREND_MAX  64

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// ============================================================
// Dynamic page list
// ============================================================
enum PageId : int {
  PAGE_SESSION = 0,
  PAGE_WEEK    = 1,
  PAGE_MODELS  = 2,
  PAGE_TREND   = 3,
  PAGE_EXTRA   = 4,
};

struct PageInfo { PageId id; const char* name; };

PageInfo activePages[5];
int activePagesN   = 0;
int currentPageIdx = 0;
int fromPageIdx    = 0;
int toPageIdx      = 1;

// ============================================================
// Runtime state
// ============================================================
unsigned long pageStartMs       = 0;
bool          inTransition      = false;
unsigned long transitionStartMs = 0;
unsigned long lastPoll          = 0;
unsigned long lastServerCheck   = 0;
unsigned long lastFetchMs       = 0;
unsigned long bootEntryMs       = 0;
bool          haveData          = false;
bool          serverAlive       = false;
String        lastError         = "boot";
unsigned long lastSuccessMs     = 0;
unsigned long wifiLostMs        = 0;
unsigned long wifiBackoff       = 5000;

// Fires once when the session percentage crosses 90.
int           prevSesPct        = -1;
unsigned long waveStartMs       = 0;

// Little Claude walking across the boot screen while we wait.
unsigned long lastWanderMs      = 0;
unsigned long wanderStartMs     = 0;
bool          wanderActive      = false;

// Hash of the key values, to tell whether anything actually changed.
uint32_t      lastDataHash      = 0;

char          bootStatusText[24] = "Starting...";

// ============================================================
// Parsed data
// ============================================================
int  sesPct = -1;   long sesReset = 0;   bool sesActive = false;
char sesRel[12]  = "";  char sesAbs[12]  = "";  char sesStatus[10] = "";

int  wkPct  = -1;   long wkReset  = 0;   int  wkProjected = -1;
char wkRel[12]   = "";  char wkAbs[12]   = "";  char wkStatus[10]  = "";

struct Model {
  char  label[12];
  int   pct;
  long  resetSec;
  char  resetRel[10];
};
Model models[MAX_MODELS];
int   modelsN = 0;

bool  extraEnabled = false;
int   extraPct     = -1;
float extraUsed    = 0;
float extraLimit   = 0;
char  extraCurrency[6] = "";
long  extraReset   = 0;
char  extraRel[10] = "";

int trend5h[TREND_MAX];  int trend5hN = 0;
int trend7d[TREND_MAX];  int trend7dN = 0;

float dispSesBarPct = 0, sesBarFrom = 0;  long sesBarTo = 0;
float dispWkBarPct  = 0, wkBarFrom  = 0;  long wkBarTo  = 0;
unsigned long sesBarAnimStart = 0, wkBarAnimStart = 0;

// ============================================================
// Easing
// ============================================================
inline float easeOutCubic(float t)   { float u = 1.0f - t; return 1.0f - u*u*u; }
inline float easeInCubic(float t)    { return t * t * t; }
inline float easeInOutCubic(float t) {
  return t < 0.5f ? 4.0f*t*t*t : 1.0f - powf(-2.0f*t + 2.0f, 3.0f) / 2.0f;
}
inline float easeOutBack(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  return 1.0f + c3 * powf(t - 1.0f, 3.0f) + c1 * powf(t - 1.0f, 2.0f);
}
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ============================================================
// Claude pixel art — drawn from primitives, parameterised by
// scale, animation frame and eye direction.
// ============================================================
enum ClaudeFrame : int {
  FRAME_STAND  = 0,
  FRAME_WALK_A = 1,
  FRAME_WALK_B = 2,
  FRAME_BLINK  = 3,
  FRAME_WAVE   = 4,
};

// Full body — head, arms, lower body, legs. Used in boot and transitions.
void drawClaude(int x, int y, int s, ClaudeFrame frame, int eyeShift = 0) {
  if (s < 1) s = 1;
  u8g2.drawBox(x + 1*s, y + 0*s, 14*s, 5*s);             // head

  int eyeOff = eyeShift * s;
  if (frame == FRAME_BLINK) {
    u8g2.setDrawColor(0);
    u8g2.drawBox(x + 3*s  + eyeOff, y + 2*s, 2*s, s);
    u8g2.drawBox(x + 11*s + eyeOff, y + 2*s, 2*s, s);
    u8g2.setDrawColor(1);
  } else {
    u8g2.setDrawColor(0);
    u8g2.drawBox(x + 3*s  + eyeOff, y + 1*s, 2*s, 3*s);
    u8g2.drawBox(x + 11*s + eyeOff, y + 1*s, 2*s, 3*s);
    u8g2.setDrawColor(1);
  }

  u8g2.drawBox(x + 0*s, y + 5*s, 16*s, 3*s);             // arms
  u8g2.drawBox(x + 1*s, y + 8*s, 14*s, 2*s);             // lower body

  if (frame == FRAME_WAVE) {
    u8g2.drawBox(x + 13*s, y - 3*s, 3*s, 4*s);
  }

  int legY = y + 10*s;
  int h[4] = {3, 3, 3, 3};
  if (frame == FRAME_WALK_A) { h[0]=4; h[1]=2; h[2]=4; h[3]=2; }
  if (frame == FRAME_WALK_B) { h[0]=2; h[1]=4; h[2]=2; h[3]=4; }
  int legX[4] = {2, 5, 9, 12};
  for (int i = 0; i < 4; i++) {
    u8g2.drawBox(x + legX[i]*s, legY, 2*s, h[i]*s);
  }
}

// Boot-time build-up. stage: 1=head, 2=+arms, 3=+legs, 4=+eyes.
void drawClaudeAssembling(int x, int y, int s, int stage) {
  if (stage >= 1) u8g2.drawBox(x + 1*s, y + 0*s, 14*s, 5*s);
  if (stage >= 2) u8g2.drawBox(x + 0*s, y + 5*s, 16*s, 3*s);
  if (stage >= 3) {
    u8g2.drawBox(x + 1*s, y + 8*s, 14*s, 2*s);
    int legX[4] = {2, 5, 9, 12};
    for (int i = 0; i < 4; i++) {
      u8g2.drawBox(x + legX[i]*s, y + 10*s, 2*s, 3*s);
    }
  }
  if (stage >= 4) {
    u8g2.setDrawColor(0);
    u8g2.drawBox(x + 3*s, y + 1*s, 2*s, 3*s);
    u8g2.drawBox(x + 11*s, y + 1*s, 2*s, 3*s);
    u8g2.setDrawColor(1);
  }
}

// Head with a raised, bouncing hand — the session >= 90 celebration.
void drawClaudeWavingHead(int x, int y, int s) {
  u8g2.drawBox(x + 1*s, y + 0*s, 14*s, 5*s);
  u8g2.setDrawColor(0);
  u8g2.drawBox(x + 3*s,  y + 2*s, 2*s, s);
  u8g2.drawBox(x + 11*s, y + 2*s, 2*s, s);
  u8g2.setDrawColor(1);
  bool up = (millis() / 200) % 2;
  int handY = y - 3*s - (up ? s : 0);
  u8g2.drawBox(x + 12*s, handY, 2*s, 3*s);
}

// ============================================================
// UI primitives
// ============================================================
void drawSpeechBubble(int cx, int topY, const char* text, float openness) {
  if (openness < 0.05f) return;
  u8g2.setFont(u8g2_font_5x7_tf);
  int tw     = u8g2.getStrWidth(text);
  int fullBw = tw + 6;
  int fullBh = 11;
  int bw = (int)(fullBw * openness);
  int bh = (int)(fullBh * openness);

  if (bw < 4 || bh < 4) {
    u8g2.drawDisc(cx, topY + fullBh/2, 1);
    return;
  }
  int bx = cx - bw / 2;
  int by = topY + (fullBh - bh) / 2;
  u8g2.drawRBox(bx, by, bw, bh, 2);

  if (openness > 0.85f) {
    u8g2.drawTriangle(cx-2, topY+fullBh-1, cx+2, topY+fullBh-1, cx, topY+fullBh+2);
  }
  if (bw > tw + 2 && bh > 8) {
    u8g2.setDrawColor(0);
    u8g2.drawStr(bx + (bw - tw)/2, by + 8, text);
    u8g2.setDrawColor(1);
  }
}

// Progress bar. urgent gives the fill a flickering dither so it reads as "hot".
void drawBar(int x, int y, int w, int h, float pct, bool urgent) {
  if (x < 0) { w += x; x = 0; }
  if (w <= 0) return;
  u8g2.drawFrame(x, y, w, h);
  int innerW = w - 2;
  int p100 = (int)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
  int bw = (innerW * p100) / 100;
  if (bw <= 0) return;
  if (urgent) {
    bool phase = (millis() / 300) % 2;
    for (int yy = 0; yy < h-2; yy++) {
      for (int xx = 0; xx < bw; xx++) {
        bool draw = phase ? ((xx + yy) % 2 == 0) : true;
        if (draw) u8g2.drawPixel(x+1+xx, y+1+yy);
      }
    }
  } else {
    u8g2.drawBox(x+1, y+1, bw, h-2);
  }
}

// Sparkline of recent samples. Caller positions it so labels/values sit outside.
void drawTrendBars(int x, int y, int w, int h, int* values, int count) {
  if (x < 0) { w += x; x = 0; }
  if (w <= 0 || count <= 0) return;

  // Dotted baseline along the bottom.
  for (int px = x; px < x + w; px += 2) u8g2.drawPixel(px, y + h - 1);

  for (int i = 0; i < count; i++) {
    int xi    = x + (i     * w) / count;
    int xiEnd = x + ((i+1) * w) / count;
    int bw    = xiEnd - xi - 1;
    if (bw < 1) bw = 1;
    if (xi < x) continue;

    int p = values[i];
    if (p < 0)   p = 0;
    if (p > 100) p = 100;

    int filled = (p * h) / 100;
    int top    = y + h - filled;

    if (filled > 0) {
      u8g2.drawBox(xi, top, bw, filled);
    }

    // Dotted outline above the fill.
    for (int yy = y; yy < top; yy += 2) {
      u8g2.drawPixel(xi, yy);
      if (bw > 1) u8g2.drawPixel(xi + bw - 1, yy);
    }

    // Mark the newest sample with a notch.
    if (i == count - 1 && bw >= 2 && filled > 0 && top - 2 >= y) {
      u8g2.drawPixel(xi + bw / 2, top - 2);
    }
  }
}

void drawPageDots() {
  if (activePagesN <= 1) return;
  int spacing = 4;
  int totalW  = (activePagesN - 1) * spacing + 2;
  int startX  = 128 - totalW - 1;
  int dotsY   = 63;
  for (int i = 0; i < activePagesN; i++) {
    if (i == currentPageIdx) u8g2.drawBox(startX + i*spacing, dotsY-1, 2, 2);
    else                     u8g2.drawPixel(startX + i*spacing + 1, dotsY);
  }
}

// Checkmark that drops in top-right, holds, then slides back up after a fetch.
void drawFreshPulse() {
  if (lastSuccessMs == 0) return;
  unsigned long age = millis() - lastSuccessMs;
  if (age > FRESH_PULSE_MS) return;
  float t = clamp01((float)age / FRESH_PULSE_MS);

  // Slide in, hold, slide out.
  int yOff;
  if      (t < 0.20f) yOff = (int)((1.0f - easeOutCubic(t / 0.20f)) * -8.0f);
  else if (t < 0.80f) yOff = 0;
  else                yOff = (int)(easeInCubic((t - 0.80f) / 0.20f) * -8.0f);

  int cx = 120, cy = 3 + yOff;
  u8g2.drawPixel(cx,     cy + 3);
  u8g2.drawPixel(cx + 1, cy + 4);
  u8g2.drawLine (cx + 2, cy + 2, cx + 5, cy - 1);
}

// Tiny status glyph in the bottom-left corner. Page text on the SESSION/WEEK/
// EXTRA pages is indented past x=8 to leave room for it.
void drawConnStatus() {
  bool stale   = haveData && (millis() - lastFetchMs > STALE_MS);
  bool nowifi  = WiFi.status() != WL_CONNECTED;
  bool err     = lastError.length() > 0;
  bool fresh   = lastSuccessMs > 0 && (millis() - lastSuccessMs < FRESH_PULSE_MS);
  bool polling = (millis() - lastPoll < 1500) && lastPoll > 0;

  int x = 2, y = 60;

  if (nowifi) {
    u8g2.drawHLine(x, y,     3);
    u8g2.drawHLine(x, y + 2, 3);
    if ((millis() / 450) % 2) u8g2.drawPixel(x + 4, y + 1);
    return;
  }

  if (!serverAlive && haveData) {
    // Server unreachable but we still have cached data — "!" glyph.
    u8g2.drawVLine(x + 1, y, 2);
    u8g2.drawPixel(x + 1, y + 3);
    return;
  }

  if (err && haveData) {
    u8g2.drawVLine(x + 1, y, 2);
    u8g2.drawPixel(x + 1, y + 3);
    return;
  }

  if (stale) {
    // Hourglass.
    u8g2.drawHLine(x, y,     4);
    u8g2.drawPixel(x + 1, y + 1);
    u8g2.drawPixel(x + 2, y + 1);
    u8g2.drawPixel(x + 1, y + 2);
    u8g2.drawPixel(x + 2, y + 2);
    u8g2.drawHLine(x, y + 3, 4);
    return;
  }

  if (polling && !fresh) {
    bool on = (millis() / 220) % 2;
    if (on) u8g2.drawDisc(x + 1, y + 1, 1);
    else    u8g2.drawCircle(x + 1, y + 1, 1);
    return;
  }

  if (!fresh) {
    // Healthy idle — a single faint pixel.
    u8g2.drawPixel(x, y + 3);
  }
}

void drawHeader(const char* pageName, const char* rightLbl) {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, pageName);
  if (rightLbl && rightLbl[0]) {
    int rw = u8g2.getStrWidth(rightLbl);
    u8g2.drawStr(128 - rw, 9, rightLbl);
  }
}

// Big centred percentage, no '%' sign.
void drawBigPct(int xCenter, int baselineY, int pct) {
  char num[6];
  if (pct < 0) snprintf(num, sizeof(num), "--");
  else         snprintf(num, sizeof(num), "%d", pct);
  u8g2.setFont(u8g2_font_logisoso28_tn);
  int w = u8g2.getStrWidth(num);
  u8g2.drawStr(xCenter - w/2, baselineY, num);
}

float currentBarVal(float &disp, float from, long to, unsigned long startMs) {
  unsigned long el = millis() - startMs;
  if (el < BAR_ANIM_MS) {
    float t = clamp01((float)el / BAR_ANIM_MS);
    disp = from + ((float)to - from) * easeOutCubic(t);
  } else {
    disp = (float)to;
  }
  return disp;
}

// ============================================================
// Network
// ============================================================
void rebuildPageList();   // forward decl

void checkWaveTrigger() {
  if (sesPct >= 90 && prevSesPct < 90 && prevSesPct >= 0) {
    waveStartMs = millis();
  }
  if (sesPct >= 0) prevSesPct = sesPct;
}

// Cheap reachability check: GET the endpoint, look at the status code, don't
// parse the body. Only updates serverAlive.
bool pingServer() {
  if (WiFi.status() != WL_CONNECTED) { serverAlive = false; return false; }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(4000);
  if (!http.begin(client, SERVER_URL)) { serverAlive = false; return false; }
  int code = http.GET();
  http.end();
  serverAlive = (code == 200);
  return serverAlive;
}

// Cheap hash over the values we care about, so we can spot a real change
// without keeping the whole JSON around.
uint32_t computeDataHash() {
  uint32_t h = (uint32_t)sesPct * 1000003u
             ^ (uint32_t)wkPct  * 999983u
             ^ (uint32_t)(sesReset & 0xFFFF) * 99991u
             ^ (uint32_t)modelsN * 997u
             ^ (uint32_t)extraPct * 991u;
  for (int i = 0; i < modelsN; i++) h ^= (uint32_t)models[i].pct * (uint32_t)(1000 + i);
  return h;
}

bool fetchUsage() {
  if (WiFi.status() != WL_CONNECTED) { lastError = "no wifi"; serverAlive = false; return false; }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, SERVER_URL)) { lastError = "begin"; serverAlive = false; return false; }
  int code = http.GET();
  if (code != 200) {
    lastError = "http " + String(code);
    serverAlive = (code > 0);   // any response means the server answered
    http.end();
    return false;
  }
  serverAlive = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { lastError = "json"; return false; }
  if (!doc["ok"].as<bool>()) {
    lastError = doc["error"].as<String>();
    return false;
  }

  // ---- session ----
  int  newSesPct = doc["session"]["pct"]       | -1;
  bool newSesAct = doc["session"]["active"]    | false;
  long newSesRst = doc["session"]["reset_sec"] |  0;
  strncpy(sesRel,    doc["session"]["reset_rel"] | "", sizeof(sesRel)-1);    sesRel[sizeof(sesRel)-1] = 0;
  strncpy(sesAbs,    doc["session"]["reset_abs"] | "", sizeof(sesAbs)-1);    sesAbs[sizeof(sesAbs)-1] = 0;
  strncpy(sesStatus, doc["session"]["status"]    | "", sizeof(sesStatus)-1); sesStatus[sizeof(sesStatus)-1] = 0;
  if (haveData && newSesPct != sesPct) {
    sesBarFrom = dispSesBarPct;  sesBarTo = newSesPct;  sesBarAnimStart = millis();
  } else if (!haveData) {
    dispSesBarPct = sesBarTo = newSesPct < 0 ? 0 : newSesPct;
  }
  sesPct = newSesPct;  sesActive = newSesAct;  sesReset = newSesRst;

  // ---- weekly ----
  int  newWkPct = doc["weekly"]["pct"]           | -1;
  long newWkRst = doc["weekly"]["reset_sec"]     |  0;
  wkProjected   = doc["weekly"]["projected_pct"] | -1;
  strncpy(wkRel,    doc["weekly"]["reset_rel"] | "", sizeof(wkRel)-1);   wkRel[sizeof(wkRel)-1] = 0;
  strncpy(wkAbs,    doc["weekly"]["reset_abs"] | "", sizeof(wkAbs)-1);   wkAbs[sizeof(wkAbs)-1] = 0;
  strncpy(wkStatus, doc["weekly"]["status"]    | "", sizeof(wkStatus)-1); wkStatus[sizeof(wkStatus)-1] = 0;
  if (haveData && newWkPct != wkPct) {
    wkBarFrom = dispWkBarPct;  wkBarTo = newWkPct;  wkBarAnimStart = millis();
  } else if (!haveData) {
    dispWkBarPct = wkBarTo = newWkPct < 0 ? 0 : newWkPct;
  }
  wkPct = newWkPct;  wkReset = newWkRst;

  // ---- models ----
  modelsN = 0;
  JsonArray ms = doc["models"];
  for (JsonObject mo : ms) {
    if (modelsN >= MAX_MODELS) break;
    Model& m = models[modelsN];
    strncpy(m.label, mo["label"] | "?", sizeof(m.label)-1);  m.label[sizeof(m.label)-1] = 0;
    m.pct      = mo["pct"]       | -1;
    m.resetSec = mo["reset_sec"] |  0;
    strncpy(m.resetRel, mo["reset_rel"] | "", sizeof(m.resetRel)-1);
    m.resetRel[sizeof(m.resetRel)-1] = 0;
    modelsN++;
  }

  // ---- extra ----
  extraEnabled = doc["extra"]["enabled"] | false;
  extraPct     = doc["extra"]["pct"]     | -1;
  extraUsed    = doc["extra"]["used"]    | 0.0;
  extraLimit   = doc["extra"]["limit"]   | 0.0;
  extraReset   = doc["extra"]["reset_sec"] | 0;
  strncpy(extraCurrency, doc["extra"]["currency"]  | "", sizeof(extraCurrency)-1);
  extraCurrency[sizeof(extraCurrency)-1] = 0;
  strncpy(extraRel,      doc["extra"]["reset_rel"] | "", sizeof(extraRel)-1);
  extraRel[sizeof(extraRel)-1] = 0;

  // ---- trends ----
  trend5hN = 0;
  for (JsonVariant v : doc["trend_5h"].as<JsonArray>()) {
    if (trend5hN >= TREND_MAX) break;
    trend5h[trend5hN++] = v.as<int>();
  }
  trend7dN = 0;
  for (JsonVariant v : doc["trend_7d"].as<JsonArray>()) {
    if (trend7dN >= TREND_MAX) break;
    trend7d[trend7dN++] = v.as<int>();
  }

  lastError     = "";
  lastFetchMs   = millis();
  lastSuccessMs = millis();
  haveData      = true;

  // Bar animations are already started per-field above; the hash just records
  // whether this fetch changed anything.
  lastDataHash = computeDataHash();

  checkWaveTrigger();
  rebuildPageList();
  return true;
}

void rebuildPageList() {
  int prevCurId = (activePagesN > 0) ? (int)activePages[currentPageIdx].id : -1;
  activePagesN = 0;
  activePages[activePagesN++] = { PAGE_SESSION, "SESSION" };
  activePages[activePagesN++] = { PAGE_WEEK,    "WEEK" };
  if (modelsN > 0)   activePages[activePagesN++] = { PAGE_MODELS, "MODELS" };
                     activePages[activePagesN++] = { PAGE_TREND,  "TREND" };
  if (extraEnabled)  activePages[activePagesN++] = { PAGE_EXTRA,  "EXTRA" };

  int newCur = -1;
  for (int i = 0; i < activePagesN; i++) {
    if ((int)activePages[i].id == prevCurId) { newCur = i; break; }
  }
  currentPageIdx = (newCur >= 0) ? newCur : 0;

  if (inTransition &&
      (fromPageIdx >= activePagesN || toPageIdx >= activePagesN)) {
    inTransition = false;
    pageStartMs  = millis();
  }
}

// ============================================================
// Pages
// ============================================================
void renderSession(int xo) {
  const char* right = (sesPct >= 0 && sesReset > 0) ? sesRel : (sesActive ? "" : "idle");
  drawHeader("SESSION", right);

  if (sesPct < 0) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(xo + 40, 38, "no data");
    return;
  }

  drawBigPct(xo + 64, 44, sesPct);

  currentBarVal(dispSesBarPct, sesBarFrom, sesBarTo, sesBarAnimStart);
  drawBar(xo + 4, 48, 120, 7, dispSesBarPct, sesPct >= 90);

  // Status word (chill / steady / watch / ...), clear of the corner glyph.
  if (sesStatus[0]) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(xo + 9, 62, sesStatus);
  }

  // Celebrate when we're near the cap.
  bool waving = (waveStartMs > 0) && (millis() - waveStartMs < WAVE_DURATION);
  if (waving) {
    drawClaudeWavingHead(xo + 50, 55, 2);
  }
}

void renderWeek(int xo) {
  drawHeader("WEEK", (wkPct >= 0 && wkReset > 0) ? wkRel : "");

  if (wkPct < 0) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(xo + 40, 38, "no data");
    return;
  }

  drawBigPct(xo + 64, 44, wkPct);

  currentBarVal(dispWkBarPct, wkBarFrom, wkBarTo, wkBarAnimStart);
  drawBar(xo + 4, 48, 120, 7, dispWkBarPct, wkPct >= 90);

  // Projected end-of-window value, small, clear of the corner glyph.
  if (wkProjected >= 0) {
    char buf[14];
    snprintf(buf, sizeof(buf), "end~%d%%", wkProjected);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(xo + 9, 62, buf);
  }
}

void renderModels(int xo) {
  drawHeader("MODELS", "");

  if (modelsN == 0) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(xo + 24, 38, "no models");
    return;
  }

  u8g2.setFont(u8g2_font_5x7_tf);
  int rowH = 11;
  int n    = modelsN > 4 ? 4 : modelsN;
  int startY = 14;

  for (int i = 0; i < n; i++) {
    int yy = startY + i * rowH;
    u8g2.drawStr(xo + 0, yy + 6, models[i].label);

    char num[6];
    if (models[i].pct < 0) snprintf(num, sizeof(num), "--");
    else                   snprintf(num, sizeof(num), "%d", models[i].pct);
    int nw = u8g2.getStrWidth(num);
    u8g2.drawStr(xo + 42 - nw, yy + 6, num);

    int barX = xo + 46;
    int barW = 78;
    u8g2.drawFrame(barX, yy, barW, 7);
    int p = models[i].pct < 0 ? 0 : (models[i].pct > 100 ? 100 : models[i].pct);
    int filled = (p * (barW - 2)) / 100;
    if (filled > 0) u8g2.drawBox(barX + 1, yy + 1, filled, 5);
  }

  if (modelsN > 4) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(xo + 0, 63, "...");
  }
}

// Two stacked sparkline rows. Layout per row:
//   [0..17] label, [18..103] bars, [104..127] current value (right-aligned).
void renderTrend(int xo) {
  drawHeader("TREND", "");

  u8g2.setFont(u8g2_font_5x7_tf);

  // 5h row
  u8g2.drawStr(xo + 0, 20, "5h");
  drawTrendBars(xo + 18, 12, 86, 10, trend5h, trend5hN);
  char val[8];
  if (sesPct < 0) snprintf(val, sizeof(val), "--");
  else            snprintf(val, sizeof(val), "%d%%", sesPct);
  int vw = u8g2.getStrWidth(val);
  u8g2.drawStr(xo + 127 - vw, 20, val);

  // 7d row
  u8g2.drawStr(xo + 0, 41, "7d");
  drawTrendBars(xo + 18, 33, 86, 10, trend7d, trend7dN);
  if (wkPct < 0) snprintf(val, sizeof(val), "--");
  else           snprintf(val, sizeof(val), "%d%%", wkPct);
  vw = u8g2.getStrWidth(val);
  u8g2.drawStr(xo + 127 - vw, 41, val);
}

void renderExtra(int xo) {
  drawHeader("EXTRA", (extraReset > 0) ? extraRel : "");

  if (extraPct < 0) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(xo + 24, 38, "off");
    return;
  }

  drawBigPct(xo + 64, 44, extraPct);
  drawBar(xo + 4, 48, 120, 7, (float)extraPct, extraPct >= 90);

  char buf[24];
  if (extraLimit > 0.5f) {
    snprintf(buf, sizeof(buf), "%.2f/%.0f%s", extraUsed, extraLimit, extraCurrency);
  } else {
    snprintf(buf, sizeof(buf), "%.2f%s", extraUsed, extraCurrency);
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(xo + 9, 62, buf);
}

void renderPage(PageId p, int xo) {
  switch (p) {
    case PAGE_SESSION: renderSession(xo); break;
    case PAGE_WEEK:    renderWeek(xo);    break;
    case PAGE_MODELS:  renderModels(xo);  break;
    case PAGE_TREND:   renderTrend(xo);   break;
    case PAGE_EXTRA:   renderExtra(xo);   break;
  }
}

// ============================================================
// Boot / connecting screen.
// statusGlyph: -1=error, 1=wifi pending, 2=server pending, 3=ready.
// ============================================================
void renderBoot(int statusGlyph) {
  u8g2.clearBuffer();

  if (bootEntryMs == 0) bootEntryMs = millis();
  unsigned long age = millis() - bootEntryMs;

  int stage = 4;
  bool assembling = age < 1600;
  if (assembling) {
    if      (age < 400)  stage = 1;
    else if (age < 800)  stage = 2;
    else if (age < 1200) stage = 3;
    else                 stage = 4;
  }

  int scale = 3;
  int cw    = 16 * scale;
  int cx    = (128 - cw) / 2;
  int cy    = 9;

  // Gentle breathing bob.
  float breathPhase = (float)(millis() % 2600) / 2600.0f;
  float breathSine  = sinf(breathPhase * 2.0f * 3.14159f);
  int   breath      = assembling ? 0 : (int)(breathSine * 1.2f);

  // Eyes drift left/right.
  float eyePhase = (float)(millis() % 4200) / 4200.0f;
  float eyeSine  = sinf(eyePhase * 2.0f * 3.14159f);
  int   eyeShift = assembling ? 0 : (int)(eyeSine * 1.3f);

  bool blink = !assembling && (millis() % 3800 < 140);

  drawClaudeAssembling(cx, cy + breath, scale, stage);

  if (!assembling && stage >= 4) {
    // Erase the default eyes and redraw them looking/blinking.
    u8g2.setDrawColor(0);
    u8g2.drawBox(cx + 3*scale,  cy + breath + 1*scale, 2*scale, 3*scale);
    u8g2.drawBox(cx + 11*scale, cy + breath + 1*scale, 2*scale, 3*scale);
    u8g2.setDrawColor(1);

    if (blink) {
      u8g2.setDrawColor(0);
      u8g2.drawBox(cx + 3*scale,  cy + breath + 2*scale, 2*scale, scale);
      u8g2.drawBox(cx + 11*scale, cy + breath + 2*scale, 2*scale, scale);
      u8g2.setDrawColor(1);
    } else {
      u8g2.setDrawColor(0);
      u8g2.drawBox(cx + 3*scale  + eyeShift*scale, cy + breath + 1*scale, 2*scale, 3*scale);
      u8g2.drawBox(cx + 11*scale + eyeShift*scale, cy + breath + 1*scale, 2*scale, 3*scale);
      u8g2.setDrawColor(1);
    }
  }

  // Tiny status line, top-left.
  if (!assembling) {
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(0, 6, bootStatusText);
  }

  // Three dots: wifi, server, data.
  if (!assembling) {
    int gy = 60;
    int gxs[3] = { 56, 64, 72 };
    int states[3] = {0, 0, 0};
    if      (statusGlyph == -1) { states[0] = -1; }
    else if (statusGlyph == 1)  { states[0] =  0; }
    else if (statusGlyph == 2)  { states[0] =  1; states[1] = 0; }
    else if (statusGlyph == 3)  { states[0] =  1; states[1] = 1; states[2] = 1; }

    float pulseSine = sinf((float)(millis() % 800) / 800.0f * 2.0f * 3.14159f);
    bool  pulseOn   = pulseSine > 0.0f;

    for (int i = 0; i < 3; i++) {
      int gx = gxs[i];
      if (states[i] == 1) {
        u8g2.drawDisc(gx, gy, 1);
      } else if (states[i] == -1) {
        u8g2.drawLine(gx-1, gy-1, gx+1, gy+1);
        u8g2.drawLine(gx-1, gy+1, gx+1, gy-1);
      } else {
        u8g2.drawCircle(gx, gy, 1);
        if (pulseOn) u8g2.drawPixel(gx, gy);
      }
    }
  }

  // While we're stuck waiting, send Claude on a walk across the screen.
  if (!assembling) {
    if (wanderActive) {
      unsigned long el  = millis() - wanderStartMs;
      unsigned long dur = 4500;
      if (el >= dur) {
        wanderActive = false;
        lastWanderMs = millis();
      } else {
        float t = easeInOutCubic(clamp01((float)el / dur));
        int wX  = (int)(-20 + t * 168);
        ClaudeFrame f = ((millis() / 140) % 2) ? FRAME_WALK_A : FRAME_WALK_B;
        drawClaude(wX, 50, 1, f, 1);
      }
    } else if (millis() - lastWanderMs > WANDER_GAP_MS) {
      wanderActive  = true;
      wanderStartMs = millis();
    }
  }

  u8g2.sendBuffer();
}

// ============================================================
// Page transition: little Claude walks the new page name in, then off.
// ============================================================
void renderTransition() {
  unsigned long elapsed = millis() - transitionStartMs;
  float t = clamp01((float)elapsed / TRANSITION_MS);

  PageId fromP       = activePages[fromPageIdx].id;
  PageId toP         = activePages[toPageIdx].id;
  const char* toName = activePages[toPageIdx].name;

  if (t < 0.13f) {
    float p = easeInCubic(t / 0.13f);
    int xo = (int)(-128.0f * p);
    u8g2.clearBuffer();
    renderPage(fromP, xo);
  } else if (t < 0.40f) {
    float p = (t - 0.13f) / 0.27f;
    float e = easeInOutCubic(p);
    u8g2.clearBuffer();
    int cX  = (int)(140 - e * 84);
    ClaudeFrame f = ((millis() / 130) % 2) ? FRAME_WALK_A : FRAME_WALK_B;
    drawClaude(cX, 26, 1, f, -1);
  } else if (t < 0.62f) {
    u8g2.clearBuffer();
    int   cX   = 56;
    float subp = (t - 0.40f) / 0.22f;
    int bounce = (int)(sinf(subp * 3.14159f) * -1.5f);
    ClaudeFrame f = FRAME_STAND;
    if (subp > 0.45f && subp < 0.55f) f = FRAME_BLINK;
    drawClaude(cX, 26 + bounce, 1, f, 0);
    drawSpeechBubble(cX + 8, 9, toName, easeOutCubic(subp));
  } else if (t < 0.80f) {
    float p = (t - 0.62f) / 0.18f;
    float e = easeInOutCubic(p);
    u8g2.clearBuffer();
    int cX  = (int)(56 - e * 84);
    ClaudeFrame f = ((millis() / 130) % 2) ? FRAME_WALK_A : FRAME_WALK_B;
    drawClaude(cX, 26, 1, f, -1);
    float bub = clamp01(1.0f - easeInCubic(p));
    if (bub > 0.05f) drawSpeechBubble(cX + 8, 9, toName, bub);
  } else {
    float p = easeOutBack(clamp01((t - 0.80f) / 0.20f));
    int xo  = (int)(128.0f * (1.0f - p));
    u8g2.clearBuffer();
    renderPage(toP, xo);
  }
}

// ============================================================
// Master render
// ============================================================
void render() {
  bool stale = haveData && (millis() - lastFetchMs > STALE_MS);

  if (!haveData || stale) {
    int glyph = -1;
    if (WiFi.status() != WL_CONNECTED)                                   glyph = 1;
    else if (lastError.indexOf("token") >= 0 || lastError.indexOf("401") >= 0) glyph = -1;
    else if (lastError.length() > 0)                                     glyph = -1;
    else                                                                  glyph = 2;
    renderBoot(glyph);
    return;
  }

  u8g2.clearBuffer();

  if (!inTransition) {
    renderPage(activePages[currentPageIdx].id, 0);
    drawPageDots();
    drawConnStatus();
    drawFreshPulse();
    u8g2.sendBuffer();
    return;
  }

  unsigned long elapsed = millis() - transitionStartMs;
  if (elapsed >= TRANSITION_MS) {
    inTransition   = false;
    currentPageIdx = toPageIdx;
    pageStartMs    = millis();
    u8g2.clearBuffer();
    renderPage(activePages[currentPageIdx].id, 0);
    drawPageDots();
    drawConnStatus();
    drawFreshPulse();
    u8g2.sendBuffer();
    return;
  }

  renderTransition();
  drawPageDots();
  drawConnStatus();
  drawFreshPulse();
  u8g2.sendBuffer();
}

void maybeStartTransition() {
  if (inTransition) return;
  if (!haveData) return;
  if (activePagesN < 2) return;
  if (millis() - pageStartMs < PAGE_DURATION) return;
  fromPageIdx       = currentPageIdx;
  toPageIdx         = (currentPageIdx + 1) % activePagesN;
  inTransition      = true;
  transitionStartMs = millis();
}

// ============================================================
// Setup with animated boot
// ============================================================
void connectWiFiAnimated() {
  snprintf(bootStatusText, sizeof(bootStatusText), "WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  unsigned long lastFrame = 0;
  int dot = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
    if (millis() - lastFrame >= 28) {
      lastFrame = millis();
      // Cycle the trailing dots so the screen shows it's still working.
      dot = (dot + 1) % 4;
      char buf[24];
      snprintf(buf, sizeof(buf), "WiFi%.*s", dot, "...");
      strncpy(bootStatusText, buf, sizeof(bootStatusText)-1);
      renderBoot(1);
    }
    delay(5);
    yield();
  }
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(bootStatusText, sizeof(bootStatusText), "WiFi ok");
  } else {
    snprintf(bootStatusText, sizeof(bootStatusText), "WiFi fail");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Wire.begin(D2, D1);
  u8g2.begin();

  bootEntryMs = millis();

  // Let the boot animation assemble before we do anything else.
  unsigned long t0 = millis();
  while (millis() - t0 < 1800) {
    renderBoot(0);
    delay(28);
    yield();
  }

  connectWiFiAnimated();

  // First contact with the server — keep retrying for a while.
  snprintf(bootStatusText, sizeof(bootStatusText), "Fetching...");
  unsigned long t1 = millis();
  while (!haveData && millis() - t1 < 15000) {
    renderBoot(2);
    if (fetchUsage()) {
      snprintf(bootStatusText, sizeof(bootStatusText), "Ready!");
      renderBoot(3);
      delay(400);
    } else {
      snprintf(bootStatusText, sizeof(bootStatusText), "Retrying...");
      delay(700);
    }
    yield();
  }

  if (!haveData) {
    snprintf(bootStatusText, sizeof(bootStatusText), "No data");
  }

  // Fall back to a minimal page set if we never got data.
  if (activePagesN == 0) {
    activePages[0] = { PAGE_SESSION, "SESSION" };
    activePages[1] = { PAGE_WEEK,    "WEEK" };
    activePages[2] = { PAGE_TREND,   "TREND" };
    activePagesN = 3;
  }

  pageStartMs = millis();
  lastPoll    = millis();
  lastServerCheck = millis();
}

// ============================================================
// Loop
// ============================================================
void loop() {
  unsigned long now = millis();

  // ── WiFi auto-reconnect with exponential backoff ──────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostMs == 0) wifiLostMs = now;
    if (now - wifiLostMs > wifiBackoff) {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      wifiLostMs  = now;
      wifiBackoff = min(wifiBackoff * 2, WIFI_BACKOFF_CAP);
    }
  } else {
    if (wifiLostMs != 0) {
      // Just reconnected — reset backoff
      wifiBackoff = 5000;
    }
    wifiLostMs = 0;
  }

  // ── Lightweight server-alive ping every SERVER_CHECK_MS ───────────────────
  if (WiFi.status() == WL_CONNECTED && now - lastServerCheck >= SERVER_CHECK_MS) {
    lastServerCheck = now;
    // Only do a cheap ping if it's not time for a full fetch yet
    if (now - lastPoll < POLL_MS - 2000) {
      pingServer();
    }
  }

  // ── Full data fetch every POLL_MS ─────────────────────────────────────────
  if (now - lastPoll >= POLL_MS) {
    lastPoll = now;
    fetchUsage();
  }

  // ── Tick down reset counters once per second ──────────────────────────────
  static unsigned long lastSecTick = 0;
  if (haveData && now - lastSecTick >= 1000) {
    lastSecTick = now;
    if (sesReset   > 0) sesReset--;
    if (wkReset    > 0) wkReset--;
    if (extraReset > 0) extraReset--;
    for (int i = 0; i < modelsN; i++) {
      if (models[i].resetSec > 0) models[i].resetSec--;
    }
  }

  maybeStartTransition();

  // ── Adaptive render rate ──────────────────────────────────────────────────
  static unsigned long lastRenderMs = 0;
  bool needFast = inTransition
                || !haveData
                || wanderActive
                || (lastSuccessMs > 0 && now - lastSuccessMs < FRESH_PULSE_MS)
                || (waveStartMs   > 0 && now - waveStartMs   < WAVE_DURATION);
  unsigned long interval = needFast ? RENDER_FAST_MS : RENDER_SLOW_MS;
  if (now - lastRenderMs >= interval) {
    lastRenderMs = now;
    render();
  }

  yield();
  delay(2);
}
