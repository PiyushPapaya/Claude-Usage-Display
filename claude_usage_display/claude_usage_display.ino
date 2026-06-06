// claude_usage_display.ino — V7
//
// World-class Claude Usage Display for ESP8266 + SH1106 128x64 OLED.
//
// V7 highlights:
//   - secrets.h for WiFi/server (no plaintext creds in source)
//   - HUGE percentage (logisoso28), no '%' sign — pixels are precious
//   - Claude mascot is the status indicator (mood faces, not words)
//   - 5 dynamic pages: SESSION / WEEK / MODELS / TREND / EXTRA (auto-shown)
//   - Per-model page (Opus, Sonnet, future codenames) with dynamic rows
//   - Sparkline: fixed Y 0..100, 2px thick, ref lines at 25/50/75, endpoint dot
//   - Boot screen: Claude assembles himself (head → arms → legs → eyes)
//   - Boot status = glyph row (no text leaks: SSID/URL never shown)
//   - Mini "fresh data" tick slides in/out after each successful fetch
//   - Idle wander: tiny Claude strolls across the screen during long stalls
//   - Wave celebration when SESSION crosses 90% (one-shot)
//   - WiFi auto-reconnect; snappier transitions (1.5s, small Claude)

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

// Forward-declare enums so Arduino's auto-prototype generator sees the types.
enum PageId      : int;
enum ClaudeFrame : int;
enum Mood        : int;

// ============================================================
// Config
// ============================================================
static const unsigned long POLL_MS         = 60000;
static const unsigned long PAGE_DURATION   = 7000;
static const unsigned long TRANSITION_MS   = 1500;
static const unsigned long BAR_ANIM_MS     = 600;
static const unsigned long RENDER_FAST_MS  = 25;     // ~40fps
static const unsigned long RENDER_SLOW_MS  = 80;     // ~12fps idle
static const unsigned long STALE_MS        = 10UL * 60UL * 1000UL;
static const unsigned long WAVE_DURATION   = 2500;
static const unsigned long FRESH_PULSE_MS  = 1400;
static const unsigned long WANDER_GAP_MS   = 12000;

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
int activePagesN  = 0;
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
unsigned long lastFetchMs       = 0;
unsigned long bootEntryMs       = 0;
bool          haveData          = false;
String        lastError         = "boot";
unsigned long lastSuccessMs     = 0;
unsigned long wifiLostMs        = 0;

// Wave trigger (one-shot when SESSION crosses 90)
int           prevSesPct        = -1;
unsigned long waveStartMs       = 0;

// Idle wander (during error / stale screens)
unsigned long lastWanderMs      = 0;
unsigned long wanderStartMs     = 0;
bool          wanderActive      = false;

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

// ============================================================
// Claude pixel art (parameterised: scale + frame + eye direction)
// Body = 16 wide x 13 tall at scale 1 (with legs); head only = 16x5.
// ============================================================
enum ClaudeFrame : int {
  FRAME_STAND  = 0,
  FRAME_WALK_A = 1,
  FRAME_WALK_B = 2,
  FRAME_BLINK  = 3,
  FRAME_WAVE   = 4,
};

enum Mood : int{
  MOOD_HAPPY  = 0,   //   <50%  (closed/smile)
  MOOD_STEADY = 1,   // 50-74%  (normal)
  MOOD_WATCH  = 2,   // 75-89%  (squinted)
  MOOD_ALERT  = 3,   // 90-99%  (wide)
  MOOD_LIMIT  = 4,   // 100%+   (X eyes)
};

Mood moodFromPct(int p) {
  if (p < 0)     return MOOD_STEADY;
  if (p >= 100)  return MOOD_LIMIT;
  if (p >= 90)   return MOOD_ALERT;
  if (p >= 75)   return MOOD_WATCH;
  if (p >= 50)   return MOOD_STEADY;
  return MOOD_HAPPY;
}

// Just the head (16x5 at scale=1) with mood-driven eyes — fits anywhere on a page.
void drawClaudeFace(int x, int y, int s, Mood m) {
  u8g2.drawBox(x + 1*s, y + 0*s, 14*s, 5*s);
  u8g2.setDrawColor(0);
  switch (m) {
    case MOOD_HAPPY:  // happy slits
      u8g2.drawBox(x + 3*s,  y + 2*s, 2*s, s);
      u8g2.drawBox(x + 11*s, y + 2*s, 2*s, s);
      break;
    case MOOD_STEADY:
      u8g2.drawBox(x + 3*s,  y + 1*s, 2*s, 3*s);
      u8g2.drawBox(x + 11*s, y + 1*s, 2*s, 3*s);
      break;
    case MOOD_WATCH:  // squinted, shifted down
      u8g2.drawBox(x + 3*s,  y + 2*s, 2*s, 2*s);
      u8g2.drawBox(x + 11*s, y + 2*s, 2*s, 2*s);
      break;
    case MOOD_ALERT:  // wide open
      u8g2.drawBox(x + 3*s,  y + 1*s, 3*s, 3*s);
      u8g2.drawBox(x + 10*s, y + 1*s, 3*s, 3*s);
      break;
    case MOOD_LIMIT:  // X eyes
      for (int i = 0; i < 3*s; i++) {
        u8g2.drawPixel(x + 3*s  + i, y + 1*s + i);
        u8g2.drawPixel(x + 5*s  - i, y + 1*s + i);
        u8g2.drawPixel(x + 11*s + i, y + 1*s + i);
        u8g2.drawPixel(x + 13*s - i, y + 1*s + i);
      }
      break;
  }
  u8g2.setDrawColor(1);
}

// Waving variant — head + hand raised above. Used as celebration on SESSION >=90.
void drawClaudeWavingHead(int x, int y, int s) {
  drawClaudeFace(x, y, s, MOOD_HAPPY);
  bool up = (millis() / 180) % 2;
  int handY = y - 3*s - (up ? s : 0);
  u8g2.drawBox(x + 12*s, handY, 2*s, 3*s);
}

// Full Claude (head + arms + body + legs). Used in boot and transitions.
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

// Assembly animation — used once on boot. stage: 1=head, 2=+arms, 3=+legs, 4=+eyes.
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
    u8g2.drawBox(x + 3*s,  y + 1*s, 2*s, 3*s);
    u8g2.drawBox(x + 11*s, y + 1*s, 2*s, 3*s);
    u8g2.setDrawColor(1);
  }
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

// Progress bar. urgent=true -> subtle dither pulse.
void drawBar(int x, int y, int w, int h, float pct, bool urgent) {
  u8g2.drawFrame(x, y, w, h);
  int innerW = w - 2;
  int p100 = (int)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
  int bw = (innerW * p100) / 100;
  if (bw <= 0) return;
  if (urgent) {
    bool phase = (millis() / 280) % 2;
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

// Trend bars: each sample is a vertical column.
//   - Solid filled bar from baseline up to (h * pct/100)
//   - Dotted "ceiling" pixels above that, up to top (shows where 100% would be)
// This gives crystal-clear visual difference between 5%, 30%, 70%, 100%.
// The last (newest) bar gets a 1px solid frame around it as an endpoint marker.
void drawTrendBars(int x, int y, int w, int h, int* values, int count) {
  // Baseline (always visible, dotted)
  for (int px = x; px < x + w; px += 2) u8g2.drawPixel(px, y + h - 1);

  if (count <= 0) return;

  for (int i = 0; i < count; i++) {
    // Each sample gets a slot; widths self-balance so bars never overlap.
    int xi    = x + (i     * w) / count;
    int xiEnd = x + ((i+1) * w) / count;
    int bw    = xiEnd - xi - 1;        // 1px gap between bars
    if (bw < 1) bw = 1;

    int p = values[i];
    if (p < 0)   p = 0;
    if (p > 100) p = 100;

    int filled = (p * h) / 100;
    int top    = y + h - filled;       // top of solid fill

    // Solid filled portion (the "used" amount)
    if (filled > 0) {
      u8g2.drawBox(xi, top, bw, filled);
    }

    // Dotted ceiling above the bar — left + right edge dots every 2 rows.
    // This is the key fix: 5% and 30% bars now look visibly different
    // because the *dotted* unfilled portion is clearly tall vs short.
    for (int yy = y; yy < top; yy += 2) {
      u8g2.drawPixel(xi, yy);
      if (bw > 1) u8g2.drawPixel(xi + bw - 1, yy);
    }

    // Endpoint marker on the newest sample — outline its bar so eye finds it
    if (i == count - 1 && bw >= 2 && filled > 0) {
      // tiny notch above the bar so the latest reading pops
      if (top - 2 >= y) {
        u8g2.drawPixel(xi + bw / 2, top - 2);
      }
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

// Slide-down tick top-right after a fresh fetch.
void drawFreshPulse() {
  if (lastSuccessMs == 0) return;
  unsigned long age = millis() - lastSuccessMs;
  if (age > FRESH_PULSE_MS) return;
  float t = (float)age / FRESH_PULSE_MS;
  int yOff;
  if      (t < 0.18f) yOff = (int)((1.0f - t/0.18f) * -8.0f);
  else if (t < 0.78f) yOff = 0;
  else                yOff = (int)(((t - 0.78f) / 0.22f) * -8.0f);
  int cx = 119, cy = 2 + yOff;
  // small check mark
  u8g2.drawLine(cx,   cy+2, cx+1, cy+3);
  u8g2.drawLine(cx+1, cy+3, cx+4, cy);
}

// Tiny persistent corner status — always-visible health glyph.
// Bottom-left, 5x4 px, never overlaps page content or page dots.
//   - all OK + fresh:   nothing (clean screen)
//   - all OK + idle:    single faint baseline pixel
//   - polling now:      pulsing dot
//   - stale data:       small clock-tick (hourglass-ish)
//   - server error:     "!" mark
//   - no wifi:          two stacked dashes (radio-out)
void drawConnStatus() {
  bool stale  = haveData && (millis() - lastFetchMs > STALE_MS);
  bool nowifi = WiFi.status() != WL_CONNECTED;
  bool err    = lastError.length() > 0;
  bool fresh  = lastSuccessMs > 0 && (millis() - lastSuccessMs < FRESH_PULSE_MS);
  bool polling = (millis() - lastPoll < 1500) && lastPoll > 0;

  int x = 0, y = 60;   // bottom-left, just above page-dots line (y=63)

  if (nowifi) {
    // two stacked dashes — "no signal"
    u8g2.drawHLine(x, y,     3);
    u8g2.drawHLine(x, y + 2, 3);
    bool blink = (millis() / 400) % 2;
    if (blink) u8g2.drawPixel(x + 4, y + 1);
    return;
  }

  if (err && haveData) {
    // "!" — server problem but we still have cached data
    u8g2.drawVLine(x + 1, y, 2);
    u8g2.drawPixel(x + 1, y + 3);
    return;
  }

  if (stale) {
    // hourglass-ish: top wedge + bottom wedge
    u8g2.drawHLine(x, y,     4);
    u8g2.drawPixel(x + 1, y + 1);
    u8g2.drawPixel(x + 2, y + 1);
    u8g2.drawPixel(x + 1, y + 2);
    u8g2.drawPixel(x + 2, y + 2);
    u8g2.drawHLine(x, y + 3, 4);
    return;
  }

  if (polling && !fresh) {
    // pulsing dot — actively fetching
    bool on = (millis() / 250) % 2;
    if (on) u8g2.drawDisc(x + 1, y + 1, 1);
    else    u8g2.drawCircle(x + 1, y + 1, 1);
    return;
  }

  if (!fresh) {
    // healthy idle — single faint baseline pixel (very subtle)
    u8g2.drawPixel(x + 1, y + 3);
  }
  // when 'fresh' is true, drawFreshPulse handles the top-right tick instead
}

void drawHeader(const char* pageName, const char* rightLbl) {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, pageName);
  if (rightLbl && rightLbl[0]) {
    int rw = u8g2.getStrWidth(rightLbl);
    u8g2.drawStr(128 - rw, 9, rightLbl);
  }
}

// Huge percentage — logisoso28, no '%' sign.
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
    float t = (float)el / BAR_ANIM_MS;
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

bool fetchUsage() {
  if (WiFi.status() != WL_CONNECTED) { lastError = "no wifi"; return false; }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, SERVER_URL)) { lastError = "begin"; return false; }
  int code = http.GET();
  if (code != 200) {
    lastError = "http " + String(code);
    http.end();
    return false;
  }

  // Parse straight from the network stream rather than buffering the whole
  // payload into a String first. On the ESP8266 that roughly halves peak heap
  // use (no multi-KB String + JsonDocument copy living at the same time).
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
  checkWaveTrigger();
  rebuildPageList();
  return true;
}

void rebuildPageList() {
  int prevCurId = (activePagesN > 0) ? (int)activePages[currentPageIdx].id : -1;
  activePagesN = 0;
  activePages[activePagesN++] = { PAGE_SESSION, "SESSION" };
  activePages[activePagesN++] = { PAGE_WEEK,    "WEEK" };
  if (modelsN > 0)    activePages[activePagesN++] = { PAGE_MODELS, "MODELS" };
                       activePages[activePagesN++] = { PAGE_TREND,  "TREND" };
  if (extraEnabled)    activePages[activePagesN++] = { PAGE_EXTRA,  "EXTRA" };

  // Keep the user on the same page if it still exists, else clamp to first.
  int newCur = -1;
  for (int i = 0; i < activePagesN; i++) {
    if ((int)activePages[i].id == prevCurId) { newCur = i; break; }
  }
  currentPageIdx = (newCur >= 0) ? newCur : 0;

  // A page transition may be in flight. Its cached from/to indices refer to
  // the OLD list; if the list just shrank (models/extra appeared or vanished)
  // those indices can now point past activePagesN and crash renderTransition.
  // Cancel the transition and land cleanly on the resolved current page.
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

  drawBigPct(xo + 64, 42, sesPct);

  currentBarVal(dispSesBarPct, sesBarFrom, sesBarTo, sesBarAnimStart);
  drawBar(xo + 4, 46, 120, 6, dispSesBarPct, sesPct >= 90);

  // Mood face — or wave celebration if recently crossed 90
  bool waving = (waveStartMs > 0) && (millis() - waveStartMs < WAVE_DURATION);
  if (waving) drawClaudeWavingHead(xo + 50, 55, 2);
  else        drawClaudeFace(xo + 50, 54, 2, moodFromPct(sesPct));
}

void renderWeek(int xo) {
  drawHeader("WEEK", (wkPct >= 0 && wkReset > 0) ? wkRel : "");

  if (wkPct < 0) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(xo + 40, 38, "no data");
    return;
  }

  drawBigPct(xo + 64, 42, wkPct);

  currentBarVal(dispWkBarPct, wkBarFrom, wkBarTo, wkBarAnimStart);
  drawBar(xo + 4, 46, 120, 6, dispWkBarPct, wkPct >= 90);

  if (wkProjected >= 0) {
    char buf[12];
    snprintf(buf, sizeof(buf), "end~%d", wkProjected);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(xo + 0, 62, buf);
  }
  drawClaudeFace(xo + 50, 54, 2, moodFromPct(wkPct));
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
    u8g2.drawStr(xo + 0, 63, "...");
  }
}

void renderTrend(int xo) {
  drawHeader("TREND", "");

  u8g2.setFont(u8g2_font_5x7_tf);

  // 5h row
  u8g2.drawStr(xo + 0, 19, "5h");
  char val[8];
  if (sesPct < 0) snprintf(val, sizeof(val), "--"); else snprintf(val, sizeof(val), "%d", sesPct);
  int vw = u8g2.getStrWidth(val);
  u8g2.drawStr(xo + 128 - vw, 19, val);
  drawTrendBars(xo + 14, 13, 108, 10, trend5h, trend5hN);

  // 7d row
  u8g2.drawStr(xo + 0, 39, "7d");
  if (wkPct < 0) snprintf(val, sizeof(val), "--"); else snprintf(val, sizeof(val), "%d", wkPct);
  vw = u8g2.getStrWidth(val);
  u8g2.drawStr(xo + 128 - vw, 39, val);
  drawTrendBars(xo + 14, 33, 108, 10, trend7d, trend7dN);

  // Mood pip bottom-right
  if (sesPct >= 0) drawClaudeFace(xo + 50, 54, 2, moodFromPct(sesPct));
}

void renderExtra(int xo) {
  drawHeader("EXTRA", (extraReset > 0) ? extraRel : "");

  if (extraPct < 0) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(xo + 24, 38, "off");
    return;
  }

  drawBigPct(xo + 64, 42, extraPct);
  drawBar(xo + 4, 46, 120, 6, (float)extraPct, extraPct >= 90);

  char buf[24];
  if (extraLimit > 0.5f) {
    snprintf(buf, sizeof(buf), "%.2f/%.0f%s", extraUsed, extraLimit, extraCurrency);
  } else {
    snprintf(buf, sizeof(buf), "%.2f%s", extraUsed, extraCurrency);
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(xo + 0, 62, buf);

  drawClaudeFace(xo + 88, 54, 2, moodFromPct(extraPct));
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
// Boot / error screen — Claude assembling + glyph status row.
// statusGlyph: 0=just assembling, 1=wifi pending, 2=server pending, 3=ok, -1=err
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

  int breath = 0;
  int eyeShift = 0;
  bool blink = false;
  if (!assembling) {
    breath = (int)(sinf((millis() % 2400) / 2400.0f * 2.0f * 3.14159f) * 1.5f);
    eyeShift = (int)(sinf((millis() % 4000) / 4000.0f * 2.0f * 3.14159f) * 1.4f);
    blink = (millis() % 3500) < 130;
  }

  drawClaudeAssembling(cx, cy + breath, scale, stage);

  // After assembly, override eyes with animation (blink + look around).
  if (!assembling && stage >= 4) {
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

  // Status glyph row — 3 dots: wifi, server, data.
  // No text leaks (SSID/URL/IP never appear).
  if (!assembling) {
    int gy = 60;
    int gxs[3] = { 56, 64, 72 };
    int states[3] = {0, 0, 0};
    if      (statusGlyph == -1) { states[0] = -1; }
    else if (statusGlyph == 1)  { states[0] =  0; }
    else if (statusGlyph == 2)  { states[0] =  1; states[1] = 0; }
    else if (statusGlyph == 3)  { states[0] =  1; states[1] = 1; states[2] = 1; }

    bool pulseOn = (millis() / 380) % 2;
    for (int i = 0; i < 3; i++) {
      int gx = gxs[i];
      if      (states[i] ==  1) u8g2.drawDisc(gx, gy, 1);
      else if (states[i] == -1) {
        u8g2.drawLine(gx-1, gy-1, gx+1, gy+1);
        u8g2.drawLine(gx-1, gy+1, gx+1, gy-1);
      } else {
        u8g2.drawCircle(gx, gy, 1);
        if (pulseOn) u8g2.drawPixel(gx, gy);
      }
    }
  }

  // Idle wander overlay
  if (wanderActive) {
    unsigned long el  = millis() - wanderStartMs;
    unsigned long dur = 4500;
    if (el >= dur) {
      wanderActive = false;
      lastWanderMs = millis();
    } else {
      float t = (float)el / dur;
      int wX  = (int)(-20 + t * 168);
      ClaudeFrame f = ((millis() / 130) % 2) ? FRAME_WALK_A : FRAME_WALK_B;
      drawClaude(wX, 50, 1, f, 1);
    }
  } else if (!assembling && millis() - lastWanderMs > WANDER_GAP_MS) {
    wanderActive  = true;
    wanderStartMs = millis();
  }

  u8g2.sendBuffer();
}

// ============================================================
// Page transition — small Claude (scale 1), snappy 1.5s
// ============================================================
void renderTransition() {
  unsigned long elapsed = millis() - transitionStartMs;
  float t = (float)elapsed / TRANSITION_MS;
  if (t > 1.0f) t = 1.0f;

  PageId fromP        = activePages[fromPageIdx].id;
  PageId toP          = activePages[toPageIdx].id;
  const char* toName  = activePages[toPageIdx].name;

  if (t < 0.13f) {
    float p = t / 0.13f;
    int xo = (int)(-128.0f * easeInCubic(p));
    renderPage(fromP, xo);
  } else if (t < 0.40f) {
    float p = (t - 0.13f) / 0.27f;
    float e = easeInOutCubic(p);
    int cX  = (int)(140 - e * 84);
    ClaudeFrame f = ((millis() / 120) % 2) ? FRAME_WALK_A : FRAME_WALK_B;
    drawClaude(cX, 26, 1, f, -1);
  } else if (t < 0.62f) {
    int   cX     = 56;
    float subp   = (t - 0.40f) / 0.22f;
    int   bounce = (int)(sinf(subp * 3.14159f) * -1.5f);
    ClaudeFrame f = FRAME_STAND;
    if (subp > 0.45f && subp < 0.55f) f = FRAME_BLINK;
    drawClaude(cX, 26 + bounce, 1, f, 0);
    drawSpeechBubble(cX + 8, 9, toName, easeOutCubic(subp));
  } else if (t < 0.80f) {
    float p = (t - 0.62f) / 0.18f;
    float e = easeInOutCubic(p);
    int cX  = (int)(56 - e * 84);
    ClaudeFrame f = ((millis() / 120) % 2) ? FRAME_WALK_A : FRAME_WALK_B;
    drawClaude(cX, 26, 1, f, -1);
    float bub = 1.0f - p;
    if (bub > 0.05f) drawSpeechBubble(cX + 8, 9, toName, bub);
  } else {
    float p = (t - 0.80f) / 0.20f;
    int xo  = (int)(128.0f * (1.0f - easeOutCubic(p)));
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
    if (WiFi.status() != WL_CONNECTED) glyph = 1;
    else if (lastError.indexOf("token") >= 0 || lastError.indexOf("401") >= 0) glyph = -1;
    else if (lastError.length() > 0)   glyph = -1;
    else                                glyph = 2;
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
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  unsigned long lastFrame = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
    if (millis() - lastFrame >= 30) {
      lastFrame = millis();
      renderBoot(1);
    }
    delay(5);
    yield();
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Wire.begin(D2, D1);
  u8g2.begin();

  bootEntryMs = millis();

  // Phase 1: assembly + breath, totally minimalist (no text)
  unsigned long t0 = millis();
  while (millis() - t0 < 1800) {
    renderBoot(0);
    delay(30);
    yield();
  }

  // Phase 2: WiFi
  connectWiFiAnimated();

  // Phase 3: first server contact
  unsigned long t1 = millis();
  while (!haveData && millis() - t1 < 15000) {
    renderBoot(2);
    fetchUsage();
    if (!haveData) delay(800);
    yield();
  }

  if (activePagesN == 0) {
    activePages[0] = { PAGE_SESSION, "SESSION" };
    activePages[1] = { PAGE_WEEK,    "WEEK" };
    activePages[2] = { PAGE_TREND,   "TREND" };
    activePagesN = 3;
  }

  pageStartMs = millis();
  lastPoll    = millis();
}

// ============================================================
// Loop
// ============================================================
void loop() {
  unsigned long now = millis();

  // WiFi auto-reconnect
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostMs == 0) wifiLostMs = now;
    if (now - wifiLostMs > 5000) {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      wifiLostMs = now;
    }
  } else {
    wifiLostMs = 0;
  }

  if (now - lastPoll >= POLL_MS) {
    lastPoll = now;
    fetchUsage();
  }

  // Tick down reset counters once per second
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

  static unsigned long lastRenderMs = 0;
  bool needFast = inTransition || !haveData
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
