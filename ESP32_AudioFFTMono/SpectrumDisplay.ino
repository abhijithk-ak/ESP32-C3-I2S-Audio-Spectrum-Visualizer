/*
 * 128x64 I2C OLED (SH1106) - SDA=GPIO8, SCL=GPIO9
 *
 * Layout: header (scrolling text + level readout) and a visual area with 4 modes
 * plus an idle animation when there is no sound.
 *
 * Serial Monitor (115200) commands:
 *   m            next visual mode; selects manual mode
 *   a            toggle automatic mode cycling
 *   +  /  -      more / less sensitive (moves the level range by 2 dB)
 *   c            recalibrate idle noise (do it when quiet, at your listening volume)
 *   tYour text   change the scrolling text, e.g.  tNow playing: Song - Artist
 * Settings are saved in ESP32 non-volatile storage and restored after reboot.
 */

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ---- level mapping: FFT magnitude in dB -> 0..100% bar height ----
float dbLow  = 20.0f;    // this level shows as 0%    (raise to hide more noise)
float dbHigh = 70.0f;    // this level shows as 100%  (raise so loud music stays lower)

// ---- tuning knobs ----
#define NUM_BARS      32       // NUM_BARS * BAR_STEP must be <= 128
#define BAR_W         3
#define BAR_STEP      4        // 3 px bar + 1 px gap
#define DEAD_ZONE     0.04f    // fraction of height ignored at the bottom (idle flicker)
#define RISE          0.7f     // 0..1, higher = snappier attack
#define FALL          0.2f     // 0..1, higher = faster decay
#define PEAK_DROP     0.015f   // fraction of height per frame the peak tick falls
#define ACTIVE_PCT    0.06f    // above this the sound counts as "playing"
#define IDLE_MS       5000     // silence for this long -> idle animation
#define AUTO_CYCLE_MS 20000    // auto mode interval in ms (0 = disable timed cycling)
#define MARQUEE_SPEED 30.0f    // scrolling text speed, px per second
#define MAX_MARQUEE_TEXT 80

// ---- layout ----
#define AREA_TOP 14            // visual area = rows 14..63
#define AREA_H   50

#define NUM_MODES 4
const char* MODE_NAMES[NUM_MODES] = { "BARS", "MIRROR", "LED", "LINE" };

String marquee = "SPECTRUM ANALYZER   *   ESP32-C3   *   ";   // ASCII only
Preferences settings;
bool settingsReady = false;
bool autoCycle = false;

int   bandStart[NUM_BARS + 1];
float barLevel[NUM_BARS];      // 0..1
float barPeak[NUM_BARS];       // 0..1
float hdrLevel = 0;            // smoothed overall level for the header readout
int   mode = 0;
uint32_t lastActiveMs = 0, lastModeChange = 0, modeLabelUntil = 0;

void loadSettings()
{
  settingsReady = settings.begin("audiofft", false);
  if (!settingsReady) {
    Serial.println("Settings unavailable; using defaults");
    return;
  }

  mode = settings.getUChar("mode", 0);
  if (mode >= NUM_MODES) mode = 0;
  autoCycle = settings.getBool("auto", false);
  dbLow = settings.getFloat("dbLow", dbLow);
  dbHigh = settings.getFloat("dbHigh", dbHigh);
  if (dbHigh <= dbLow) {
    dbLow = 20.0f;
    dbHigh = 70.0f;
  }

  String savedText = settings.getString("text", "");
  if (savedText.length()) marquee = savedText;
  lastModeChange = millis();
}

void saveSettings()
{
  if (!settingsReady) return;
  settings.putUChar("mode", (uint8_t)mode);
  settings.putBool("auto", autoCycle);
  settings.putFloat("dbLow", dbLow);
  settings.putFloat("dbHigh", dbHigh);
  settings.putString("text", marquee);
}

// Log-spaced groups of FFT bins (~60 Hz up to Nyquist), at least one bin per bar
void initBands()
{
  const int firstBin = 2;
  const int lastBin  = SAMPLE_BUFFER_SIZE / 2;
  int prev = firstBin;
  for (int b = 0; b <= NUM_BARS; b++) {
    int e = (int)(firstBin * powf((float)lastBin / firstBin, (float)b / NUM_BARS) + 0.5f);
    if (b > 0 && e <= prev) e = prev + 1;
    bandStart[b] = e;
    prev = e;
  }
  bandStart[NUM_BARS] = lastBin;
}

void initDisplay()
{
  u8g2.begin();                 // Wire was already started on GPIO8/GPIO9 in setup()
  u8g2.setBusClock(400000);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setDrawColor(1);
  u8g2.setFontPosTop();
  initBands();
  showMessage("Start FFT", "");
}

void showMessage(const char* l1, const char* l2)
{
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 0, l1);
  u8g2.drawStr(0, 14, l2);
  u8g2.sendBuffer();
}

void displayError(const char* msg)
{
  showMessage("Error:", msg);
}

void nextMode()
{
  mode = (mode + 1) % NUM_MODES;
  modeLabelUntil = millis() + 1500;
  lastModeChange = millis();
}

void printRange()
{
  Serial.printf("range: %.0f dB = 0%%, %.0f dB = 100%%\n", dbLow, dbHigh);
}

void handleSerial()
{
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'c': calibrateNoise(); break;
      case 'm':
        autoCycle = false;
        nextMode();
        saveSettings();
        Serial.printf("Mode: %s (manual)\n", MODE_NAMES[mode]);
        break;
      case 'a':
        autoCycle = !autoCycle;
        lastModeChange = millis();
        modeLabelUntil = millis() + 1500;
        saveSettings();
        Serial.printf("Mode: %s (%s)\n", MODE_NAMES[mode], autoCycle ? "auto" : "manual");
        break;
      case '+':
        dbLow -= 2; dbHigh -= 2; saveSettings(); printRange(); break;   // more sensitive
      case '-':
        dbLow += 2; dbHigh += 2; saveSettings(); printRange(); break;   // less sensitive
      case 't': {
        String s = Serial.readStringUntil('\n');
        s.trim();
        if (s.length()) {
          if (s.length() > MAX_MARQUEE_TEXT) s = s.substring(0, MAX_MARQUEE_TEXT);
          marquee = s + "   *   ";
          saveSettings();
        }
        break;
      }
      default: break;
    }
  }
}

void updateBars()
{
  float frameMaxDb = -100.0f;
  float pk = 0;

  for (int b = 0; b < NUM_BARS; b++) {
    float m = 0;
    for (int i = bandStart[b]; i < bandStart[b + 1]; i++) {
      float v = vReal[i] - noiseFloor[i];     // subtract idle noise
      if (v > m) m = v;
    }
    float tilt = 1.0f + 0.04f * b;            // gentle treble lift, music falls off with frequency
    float db = 20.0f * log10f(m * tilt + 0.001f);
    if (db > frameMaxDb) frameMaxDb = db;

    float target = (db - dbLow) / (dbHigh - dbLow);
    target = constrain(target - DEAD_ZONE, 0.0f, 1.0f);

    if (target > barLevel[b]) barLevel[b] += (target - barLevel[b]) * RISE;
    else                      barLevel[b] -= (barLevel[b] - target) * FALL;

    if (barLevel[b] > barPeak[b]) barPeak[b] = barLevel[b];
    else if ((barPeak[b] -= PEAK_DROP) < 0) barPeak[b] = 0;

    if (barLevel[b] > pk) pk = barLevel[b];
  }

  hdrLevel += (pk - hdrLevel) * 0.3f;
  if (pk > ACTIVE_PCT) lastActiveMs = millis();

  // Once a second, print the loudest level seen (handy for tuning dbLow/dbHigh)
  static uint32_t lastPrint = 0;
  static float winMaxDb = -100.0f;
  if (frameMaxDb > winMaxDb) winMaxDb = frameMaxDb;
  if (millis() - lastPrint > 1000) {
    if (winMaxDb > dbLow) {
      float pct = constrain((winMaxDb - dbLow) / (dbHigh - dbLow) - DEAD_ZONE, 0.0f, 1.0f);
      Serial.printf("peak %.1f dB -> %d%%   (range %.0f..%.0f dB)\n", winMaxDb, (int)(pct * 100), dbLow, dbHigh);
    }
    winMaxDb = -100.0f;
    lastPrint = millis();
  }
}

// ---------- header ----------

// Scrolling text: fixed-width 6x10 font, drawn from a small circular window of the string
void drawMarquee()
{
  const int CH_W = 6, VIS = 17;
  int len = marquee.length();
  if (len == 0) return;
  uint32_t px = (uint32_t)(millis() * (MARQUEE_SPEED / 1000.0f));
  int idx   = (px / CH_W) % len;
  int shift = px % CH_W;
  char buf[VIS + 2];
  for (int i = 0; i < VIS + 1; i++) buf[i] = marquee[(idx + i) % len];
  buf[VIS + 1] = 0;

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setClipWindow(0, 0, 97, 11);
  u8g2.drawStr(-shift, 1, buf);
  u8g2.setMaxClipWindow();
}

void drawHeader()
{
  drawMarquee();

  u8g2.setFont(u8g2_font_5x7_tf);
  char txt[8];
  const char* s;
  if (millis() < modeLabelUntil) {
    s = MODE_NAMES[mode];
  } else {
    snprintf(txt, sizeof(txt), "%d%%", (int)(hdrLevel * 100 + 0.5f));
    s = txt;
  }
  int w = u8g2.getStrWidth(s);
  u8g2.drawStr(127 - w, 2, s);
  u8g2.drawHLine(0, 12, 128);
}

// ---------- visual modes ----------

void drawBarsMode()
{
  for (int b = 0; b < NUM_BARS; b++) {
    int x = b * BAR_STEP;
    int h = (int)(barLevel[b] * AREA_H + 0.5f);
    int p = (int)(barPeak[b] * AREA_H + 0.5f);
    if (h > 0) u8g2.drawBox(x, 64 - h, BAR_W, h);
    if (p > h + 1) u8g2.drawHLine(x, 64 - p, BAR_W);
  }
}

void drawMirrorMode()
{
  const int cy = AREA_TOP + AREA_H / 2;
  const int half = AREA_H / 2 - 1;
  for (int b = 0; b < NUM_BARS; b++) {
    int x = b * BAR_STEP;
    int h = (int)(barLevel[b] * half + 0.5f);
    int p = (int)(barPeak[b] * half + 0.5f);
    if (h > 0) u8g2.drawBox(x, cy - h, BAR_W, 2 * h + 1);
    else       u8g2.drawHLine(x, cy, BAR_W);          // dotted centre line when quiet
    if (p > h + 1) {
      u8g2.drawHLine(x, cy - p, BAR_W);
      u8g2.drawHLine(x, cy + p, BAR_W);
    }
  }
}

void drawLedMode()                                     // retro LED-segment meter
{
  const int SEGS = AREA_H / 3;                         // 3 px pitch, 2 px tall
  for (int b = 0; b < NUM_BARS; b++) {
    int x = b * BAR_STEP;
    int segs = (int)(barLevel[b] * SEGS + 0.5f);
    int ps   = (int)(barPeak[b] * SEGS + 0.5f);
    for (int k = 0; k < segs; k++) u8g2.drawBox(x, 62 - k * 3, BAR_W, 2);
    if (ps > segs && ps > 0) u8g2.drawBox(x, 62 - (ps - 1) * 3, BAR_W, 2);
  }
}

void drawLineMode()                                    // connected line with dotted drop lines
{
  int px = 0, py = 0;
  for (int b = 0; b < NUM_BARS; b++) {
    int x = b * BAR_STEP + 1;
    int y = 63 - (int)(barLevel[b] * AREA_H + 0.5f);
    if (b > 0) u8g2.drawLine(px, py, x, y);
    for (int yy = y + 2; yy <= 63; yy += 3) u8g2.drawPixel(x, yy);
    px = x; py = y;
  }
}

// Shown when nothing has been playing for a while
void drawIdle()
{
  float t = millis() / 1000.0f;
  int cy = AREA_TOP + 18;
  int px = 0, py = cy;
  for (int x = 0; x < 128; x += 2) {
    float env = 0.5f + 0.5f * sinf(t * 0.8f + x * 0.03f);   // slow breathing envelope
    int y = cy + (int)(9.0f * env * sinf(x * 0.12f + t * 3.0f));
    if (x > 0) u8g2.drawLine(px, py, x, y);
    px = x; py = y;
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  const char* s = "waiting for music";
  int w = u8g2.getStrWidth(s);
  u8g2.drawStr((128 - w) / 2, 52, s);
}

void displayAll()
{
  updateBars();

  if (autoCycle && AUTO_CYCLE_MS && millis() - lastModeChange > AUTO_CYCLE_MS) nextMode();

  u8g2.clearBuffer();
  drawHeader();

  if (millis() - lastActiveMs > IDLE_MS) {
    drawIdle();
  } else {
    switch (mode) {
      case 0: drawBarsMode();   break;
      case 1: drawMirrorMode(); break;
      case 2: drawLedMode();    break;
      default: drawLineMode();  break;
    }
  }
  u8g2.sendBuffer();
}
