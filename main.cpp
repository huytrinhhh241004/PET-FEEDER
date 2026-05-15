#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include "HX711.h"
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>

// ─── PINS ────────────────────────────────────────────────
#define BTN_RESET  32
#define SERVO_PIN  25
#define DT          4
#define SCK        14
#define SD_CS      15

// ─── PERIPHERALS ─────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231  rtc;
Servo       myServo;
HX711       scale;
WebServer   server(80);
Preferences prefs;

// ─── SERVO CONFIG ─────────────────────────────────────────
#define SERVO_CLOSE 30
#define SERVO_OPEN   0

// ─── SENSOR DATA ──────────────────────────────────────────
float weight             = 0;
float calibration_factor = 420.0;
long  offset             = 0;

// ─── SCHEDULE ─────────────────────────────────────────────
int   feedHourMorning   = 8;
int   feedMinuteMorning = 0;
float targetMorning     = 40.0;

int   feedHourEvening   = 19;
int   feedMinuteEvening = 0;
float targetEvening     = 60.0;

// ─── STATE ────────────────────────────────────────────────
bool  fedMorning       = false;
bool  fedEvening       = false;
bool  sdOK             = false;
bool  feeding          = false;
bool  lastBtnState     = HIGH;
unsigned long lastDebounceTime = 0;

// Cân sau bữa ăn gần nhất — dùng để tính lượng thú cưng ăn
float lastAfterWeight = -1.0f;  // -1 = chưa có dữ liệu

// ─── RTOS ─────────────────────────────────────────────────
DateTime          now;
SemaphoreHandle_t rtcMutex;
SemaphoreHandle_t weightMutex;
SemaphoreHandle_t feedSemaphore;
float             currentFeedTarget = 40.0;

// ═══════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════
String pad2(int v) { return (v < 10 ? "0" : "") + String(v); }

String logFilename(int y, int mo, int d) {
  return "/" + String(y) + "_" + String(mo) + "_" + String(d) + ".txt";
}

float readWeightNow() {
  float w = 0;
  if (xSemaphoreTake(weightMutex, pdMS_TO_TICKS(100))) {
    w = weight;
    xSemaphoreGive(weightMutex);
  }
  return w < 0 ? 0 : w;
}

// ═══════════════════════════════════════════════════════════
//  SD LOG
//
//  Format:
//  FEED,<session>,<before>,<added>,<after>,<target>,<atePrev>,<YYYY-MM-DD>,<HH:MM>
//
//  session  : M=sáng tự động | E=chiều tự động | X=thủ công
//  before   : cân bát TRƯỚC khi cho ăn (g) — lượng thức ăn còn thừa
//  added    : lượng máy THÊM VÀO (g) = after - before
//  after    : cân bát SAU khi cho ăn (g) — xấp xỉ target
//  target   : mục tiêu khẩu phần (g)
//  atePrev  : ước tính thú cưng ăn từ bữa trước = lastAfterWeight - before
//             (-1 nếu chưa có dữ liệu bữa trước)
// ═══════════════════════════════════════════════════════════
void logFeed(const char *session,
             float before, float added, float after,
             float target, float atePrev) {
  if (!sdOK) return;
  DateTime t;
  if (xSemaphoreTake(rtcMutex, portMAX_DELAY)) { t = now; xSemaphoreGive(rtcMutex); }
 
  File file = SD.open(logFilename(t.year(), t.month(), t.day()), FILE_APPEND);
  if (!file) return;
 
  // ── Dòng 1: CSV cho web parse ──
  char csv[100];
  snprintf(csv, sizeof(csv),
    "FEED,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%04d-%02d-%02d,%02d:%02d",
    session, before, added, after, target, atePrev,
    t.year(), t.month(), t.day(), t.hour(), t.minute());
  file.println(csv);
 
  // ── Dòng 2: Dễ đọc ──
  const char *tenBua;
  if      (strcmp(session, "M") == 0) tenBua = "SANG     ";
  else if (strcmp(session, "E") == 0) tenBua = "CHIEU    ";
  else                                tenBua = "THU_CONG ";
 
  char atePrevStr[24];
  if (atePrev < 0) snprintf(atePrevStr, sizeof(atePrevStr), "chua_co");
  else             snprintf(atePrevStr, sizeof(atePrevStr), "%.1fg", atePrev);
 
  char readable[180];
  if (after >= target - 2.0f) {
    snprintf(readable, sizeof(readable),
      "[%02d:%02d] %s| Con_thua:%.1fg  Them_vao:%.1fg  Sau_an:%.1fg  Muc_tieu:%.1fg  Da_an_truoc:%s  -> DAT",
      t.hour(), t.minute(), tenBua,
      before, added, after, target, atePrevStr);
  } else {
    snprintf(readable, sizeof(readable),
      "[%02d:%02d] %s| Con_thua:%.1fg  Them_vao:%.1fg  Sau_an:%.1fg  Muc_tieu:%.1fg  Da_an_truoc:%s  -> CHUA_DAT(thieu %.1fg)",
      t.hour(), t.minute(), tenBua,
      before, added, after, target, atePrevStr, target - after);
  }
  file.println(readable);
 
  // ── Dòng 3: Phân cách ──
  file.println("---");
  file.close();
}

// ═══════════════════════════════════════════════════════════
//  CONFIG
// ═══════════════════════════════════════════════════════════
void loadConfig() {
  prefs.begin("feeder", true);
  feedHourMorning   = prefs.getInt  ("mh",   8);
  feedMinuteMorning = prefs.getInt  ("mm",   0);
  targetMorning     = prefs.getFloat("tgM",  40.0);
  feedHourEvening   = prefs.getInt  ("eh",   19);
  feedMinuteEvening = prefs.getInt  ("em",   0);
  targetEvening     = prefs.getFloat("tgE",  60.0);
  fedMorning        = prefs.getBool ("fedM", false);
  fedEvening        = prefs.getBool ("fedE", false);
  offset            = prefs.getLong ("offset", 0);
  lastAfterWeight   = prefs.getFloat("lastAW", -1.0f);
  prefs.end();
}

void saveConfig() {
  prefs.begin("feeder", false);
  prefs.putInt  ("mh",   feedHourMorning);
  prefs.putInt  ("mm",   feedMinuteMorning);
  prefs.putFloat("tgM",  targetMorning);
  prefs.putInt  ("eh",   feedHourEvening);
  prefs.putInt  ("em",   feedMinuteEvening);
  prefs.putFloat("tgE",  targetEvening);
  prefs.putBool ("fedM", fedMorning);
  prefs.putBool ("fedE", fedEvening);
  prefs.putLong ("offset", offset);
  prefs.putFloat("lastAW", lastAfterWeight);
  prefs.end();
}

// ═══════════════════════════════════════════════════════════
//  FEEDING
//
//  Luồng xử lý:
//  1. Đọc cân TRƯỚC (before) → lượng thức ăn còn thừa
//  2. Tính atePrev = lastAfterWeight - before → thú cưng ăn bao nhiêu từ bữa trước
//  3. Nếu bát đã đủ target → bỏ qua, chỉ ghi log
//  4. Mở servo cho đến khi bát đạt target
//  5. Đọc cân SAU (after), tính added = after - before
//  6. Ghi log đầy đủ, lưu lastAfterWeight
// ═══════════════════════════════════════════════════════════
void feedUntilWeight(float target, const char *session) {
  // ── 1. Đọc cân trước ──
  vTaskDelay(pdMS_TO_TICKS(500));
  float before = readWeightNow();

  // ── 2. Tính lượng ăn bữa trước ──
  float atePrev = -1.0f;
  if (lastAfterWeight >= 0.0f) {
    atePrev = lastAfterWeight - before;
    if (atePrev < 0) atePrev = 0;
  }

  Serial.printf("[FEED] %s | before=%.1fg | lastAW=%.1fg | atePrev=%.1fg | target=%.1fg\n",
    session, before, lastAfterWeight, atePrev, target);

  // ── 3. Bát đã đủ → bỏ qua ──
  if (before >= target - 1.0f) {
    Serial.println("[FEED] Bat da du, bo qua.");
    logFeed(session, before, 0.0f, before, target, atePrev);
    lastAfterWeight = before;
    saveConfig();
    feeding = false;
    return;
  }

  // ── 4. Mở servo ──
  // Dùng 3 góc cố định thay vì nhấp nháy → servo không bị giật
  //
  //  SERVO_OPEN  =  0° → mở hoàn toàn, thức ăn chảy nhanh
  //  SERVO_SLOW  = 18° → mở hé, thức ăn nhỏ giọt khi gần đích
  //  SERVO_CLOSE = 30° → đóng hoàn toàn
  //
  // Servo di chuyển mượt giữa các góc, không nhấp nháy.

  const int SERVO_SLOW = 18;

  myServo.attach(SERVO_PIN);
  const unsigned long TIMEOUT_MS = 5000UL;
  unsigned long startTime = millis();

  float slowZone = target - 8.0f;   // còn 8g → chuyển sang mở hé
  float stopZone = target - 1.5f;   // còn 1.5g → đóng hẳn

  int currentAngle = -1;            // tránh ghi lệnh lặp nếu góc không đổi

  while (true) {
    if (millis() - startTime > TIMEOUT_MS) {
      Serial.println("[FEED] Timeout – dong servo");
      myServo.write(SERVO_CLOSE);
      break;
    }

    float w = readWeightNow();

    int targetAngle;
    if      (w >= stopZone) targetAngle = SERVO_CLOSE;
    else if (w >= slowZone) targetAngle = SERVO_SLOW;
    else                    targetAngle = SERVO_OPEN;

    // Chỉ ghi khi góc thay đổi → tín hiệu PWM ổn định, servo không giật
    if (targetAngle != currentAngle) {
      myServo.write(targetAngle);
      currentAngle = targetAngle;
      Serial.printf("[SERVO] angle=%d (w=%.1fg)\n", targetAngle, w);
    }

    if (targetAngle == SERVO_CLOSE) break;

    vTaskDelay(pdMS_TO_TICKS(50));  // poll cân mỗi 50ms
  }

  // ── 5. Đọc cân sau ──
  vTaskDelay(pdMS_TO_TICKS(800));
  myServo.detach();
  float after = readWeightNow();
  float added = after - before;
  if (added < 0) added = 0;

  Serial.printf("[FEED] Done | after=%.1fg | added=%.1fg\n", after, added);

  // ── 6. Ghi log + lưu trạng thái ──
  logFeed(session, before, added, after, target, atePrev);
  lastAfterWeight = after;
  saveConfig();
  feeding = false;
}

// ═══════════════════════════════════════════════════════════
//  WEB – /stats
//
//  JSON mỗi ngày:
//  {
//    "date": "2026-05-06",
//    "morning": { "before":5.0, "added":35.0, "after":40.0, "target":40.0, "atePrev":37.0 },
//    "evening": { "before":3.0, "added":57.0, "after":60.0, "target":60.0, "atePrev":45.0 },
//    "leftover": 5.0   ← before của bữa sáng = thừa từ tối hôm qua
//  }
// ═══════════════════════════════════════════════════════════
struct FeedRecord {
  char  session[4];
  float before, added, after, target, atePrev;
};

bool parseFeedLine(const String &line, FeedRecord &rec) {
  if (!line.startsWith("FEED,")) return false;
  // FEED,M,5.0,35.0,40.0,40.0,37.0,2026-05-06,08:02
  String parts[9];
  int idx = 0, start = 0;
  for (int i = 0; i < (int)line.length() && idx < 8; i++) {
    if (line[i] == ',') { parts[idx++] = line.substring(start, i); start = i + 1; }
  }
  parts[idx] = line.substring(start);
  if (idx < 5) return false;

  strncpy(rec.session, parts[1].c_str(), 3); rec.session[3] = '\0';
  rec.before  = parts[2].toFloat();
  rec.added   = parts[3].toFloat();
  rec.after   = parts[4].toFloat();
  rec.target  = parts[5].toFloat();
  rec.atePrev = (idx >= 6) ? parts[6].toFloat() : -1.0f;
  return true;
}

void handleStats() {
  int days = 7;
  if (server.hasArg("days")) days = constrain(server.arg("days").toInt(), 1, 30);

  DateTime today;
  if (xSemaphoreTake(rtcMutex, portMAX_DELAY)) { today = now; xSemaphoreGive(rtcMutex); }

  String json = "[";

  for (int i = days - 1; i >= 0; i--) {
    DateTime d(today.unixtime() - (uint32_t)i * 86400UL);
    String filename = logFilename(d.year(), d.month(), d.day());

    bool  hasMorning = false, hasEvening = false;
    float mBefore=-1,mAdded=-1,mAfter=-1,mTarget=-1,mAtePrev=-1;
    float eBefore=-1,eAdded=-1,eAfter=-1,eTarget=-1,eAtePrev=-1;

    if (sdOK && SD.exists(filename)) {
      File file = SD.open(filename, FILE_READ);
      if (file) {
        while (file.available()) {
          String line = file.readStringUntil('\n');
          line.trim();
          FeedRecord rec;
          if (!parseFeedLine(line, rec)) continue;
          if (strcmp(rec.session,"M")==0) {
            hasMorning=true;
            mBefore=rec.before; mAdded=rec.added; mAfter=rec.after;
            mTarget=rec.target; mAtePrev=rec.atePrev;
          } else if (strcmp(rec.session,"E")==0) {
            hasEvening=true;
            eBefore=rec.before; eAdded=rec.added; eAfter=rec.after;
            eTarget=rec.target; eAtePrev=rec.atePrev;
          }
        }
        file.close();
      }
    }

    char dateBuf[12];
    snprintf(dateBuf,sizeof(dateBuf),"%04d-%02d-%02d",d.year(),d.month(),d.day());

    auto fJ = [](float v)->String { return v<0?"null":String(v,1); };

    if (i < days-1) json += ",";
    json += "{\"date\":\"" + String(dateBuf) + "\",";

    json += "\"morning\":";
    if (hasMorning) {
      json += "{\"before\":"  + fJ(mBefore)
            + ",\"added\":"   + fJ(mAdded)
            + ",\"after\":"   + fJ(mAfter)
            + ",\"target\":"  + fJ(mTarget)
            + ",\"atePrev\":" + fJ(mAtePrev) + "}";
    } else json += "null";

    json += ",\"evening\":";
    if (hasEvening) {
      json += "{\"before\":"  + fJ(eBefore)
            + ",\"added\":"   + fJ(eAdded)
            + ",\"after\":"   + fJ(eAfter)
            + ",\"target\":"  + fJ(eTarget)
            + ",\"atePrev\":" + fJ(eAtePrev) + "}";
    } else json += "null";

    // leftover = thức ăn thừa trước bữa sáng hôm này (= before của bữa sáng)
    json += ",\"leftover\":" + fJ(mBefore);
    json += "}";
  }

  json += "]";
  server.send(200, "application/json", json);
}

// ═══════════════════════════════════════════════════════════
//  WEB – /data
// ═══════════════════════════════════════════════════════════
void handleData() {
  DateTime t; float w;
  if (xSemaphoreTake(rtcMutex,    portMAX_DELAY)) { t = now;    xSemaphoreGive(rtcMutex);    }
  if (xSemaphoreTake(weightMutex, portMAX_DELAY)) { w = weight; xSemaphoreGive(weightMutex); }

  String json = "{";
  json += "\"time\":\""  + pad2(t.hour())+":"+pad2(t.minute())+":"+pad2(t.second()) + "\",";
  json += "\"weight\":"  + String(w,1)  + ",";
  json += "\"mh\":"      + String(feedHourMorning)   + ",";
  json += "\"mm\":"      + String(feedMinuteMorning) + ",";
  json += "\"tgM\":"     + String(targetMorning,1)   + ",";
  json += "\"eh\":"      + String(feedHourEvening)   + ",";
  json += "\"em\":"      + String(feedMinuteEvening) + ",";
  json += "\"tgE\":"     + String(targetEvening,1)   + ",";
  json += "\"sd\":"      + String(sdOK?1:0)          + ",";
  json += "\"feeding\":" + String(feeding?1:0)       + ",";
  json += "\"lastAW\":"  + String(lastAfterWeight,1);
  json += "}";
  server.send(200, "application/json", json);
}

void handleFeed() {
  if (!feeding) {
    DateTime t;
    if (xSemaphoreTake(rtcMutex, portMAX_DELAY)) { t = now; xSemaphoreGive(rtcMutex); }
    currentFeedTarget = (t.hour() >= feedHourEvening) ? targetEvening : targetMorning;
    xSemaphoreGive(feedSemaphore);
    feeding = true;
  }
  server.send(200, "text/plain", "OK");
}

void handleSet() {
  if (server.hasArg("mh")  && server.arg("mh")  !="") feedHourMorning   = server.arg("mh").toInt();
  if (server.hasArg("mm")  && server.arg("mm")  !="") feedMinuteMorning = server.arg("mm").toInt();
  if (server.hasArg("tgM") && server.arg("tgM") !="") targetMorning     = server.arg("tgM").toFloat();
  if (server.hasArg("eh")  && server.arg("eh")  !="") feedHourEvening   = server.arg("eh").toInt();
  if (server.hasArg("em")  && server.arg("em")  !="") feedMinuteEvening = server.arg("em").toInt();
  if (server.hasArg("tgE") && server.arg("tgE") !="") targetEvening     = server.arg("tgE").toFloat();
  if (server.hasArg("mh")  || server.hasArg("mm")) fedMorning = false;
  if (server.hasArg("eh")  || server.hasArg("em")) fedEvening = false;
  saveConfig();
  server.send(200, "text/plain", "Saved");
}

void handleSetTime() {
  if (!server.hasArg("time")) { server.send(400,"text/plain","Missing time"); return; }
  String t = server.arg("time");
  rtc.adjust(DateTime(
    t.substring(0,4).toInt(), t.substring(5,7).toInt(), t.substring(8,10).toInt(),
    t.substring(11,13).toInt(), t.substring(14,16).toInt(), t.substring(17,19).toInt()
  ));
  server.send(200, "text/plain", "OK");
}

void handleDownload() {
  if (!server.hasArg("file")) { server.send(400,"text/plain","Missing file"); return; }
  String filename = "/" + server.arg("file");
  if (!SD.exists(filename)) { server.send(404,"text/plain","Not found"); return; }
  File file = SD.open(filename, FILE_READ);
  server.sendHeader("Content-Disposition","attachment; filename="+server.arg("file"));
  server.streamFile(file,"text/plain");
  file.close();
}

// ═══════════════════════════════════════════════════════════
//  WEB – / (HTML)
// ═══════════════════════════════════════════════════════════
void handleRoot() {
  // HTML được nén tối đa để vừa bộ nhớ ESP32
  String html = R"rawliteral(<!DOCTYPE html>
<html lang="vi"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Pet Feeder</title>
<link href="https://fonts.googleapis.com/css2?family=DM+Sans:wght@400;500;600&family=Space+Mono:wght@700&display=swap" rel="stylesheet">
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0d1117;--card:#161b22;--card2:#1c2430;--border:#2a3340;--text:#e6edf3;--muted:#8b949e;--sky:#58a6ff;--green:#3fb950;--amber:#d29922;--red:#f85149;--accent:#79c0ff}
body{background:var(--bg);color:var(--text);font-family:'DM Sans',sans-serif;font-size:14px;padding:0 14px 40px;max-width:440px;margin:0 auto}
.hd{display:flex;align-items:center;justify-content:space-between;padding:18px 0 6px}
.ht{font-family:'Space Mono',monospace;font-size:15px;color:var(--sky);letter-spacing:.5px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--green)}
.tabs{display:flex;gap:4px;margin:10px 0 14px;background:var(--card);border:1px solid var(--border);border-radius:10px;padding:4px}
.tab{flex:1;padding:8px 0;border:none;border-radius:7px;font-size:13px;font-weight:500;cursor:pointer;background:transparent;color:var(--muted);transition:all .2s;font-family:'DM Sans',sans-serif}
.tab.on{background:var(--card2);color:var(--text);border:1px solid var(--border)}
.pg{display:none}.pg.on{display:block}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px}
.c{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px;margin-bottom:10px}
.cnm{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px}
.lb{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.8px;margin-bottom:6px}
.vl{font-family:'Space Mono',monospace;font-size:22px;font-weight:700}
.sky{color:var(--sky)}.grn{color:var(--green)}.amb{color:var(--amber)}.red{color:var(--red)}
.pb{height:6px;background:#2a3340;border-radius:3px;overflow:hidden;margin-top:10px}
.pbi{height:100%;background:linear-gradient(90deg,var(--sky),var(--green));border-radius:3px;transition:width .4s}
.ig3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-top:10px}
.ig{display:flex;flex-direction:column;gap:4px}
.il{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.4px}
input[type=number],input[type=datetime-local]{background:#0d1117;border:1px solid var(--border);border-radius:8px;color:var(--text);font-size:14px;padding:8px 10px;width:100%;font-family:'Space Mono',monospace;outline:none;-moz-appearance:textfield}
input[type=number]::-webkit-outer-spin-button,input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none}
input:focus{border-color:var(--sky)}
input[type=datetime-local]{font-family:'DM Sans',sans-serif;font-size:13px}
.btn{border-radius:9px;border:none;padding:11px;font-size:13px;font-weight:600;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:6px;font-family:'DM Sans',sans-serif;transition:opacity .15s,transform .1s;width:100%}
.btn:active{transform:scale(.97)}
.bs{background:var(--sky);color:#0d1117}
.bg{background:var(--card2);color:var(--text);border:1px solid var(--border)}
.bf{background:#238636;color:#fff}
.bdg{font-size:11px;font-family:'Space Mono',monospace;font-weight:700;padding:3px 8px;border-radius:5px}
.bOK{background:rgba(63,185,80,.15);color:var(--green)}
.bNO{background:rgba(248,81,73,.15);color:var(--red)}
.sep{font-size:12px;display:flex;align-items:center;gap:5px;margin:12px 0 8px;color:var(--amber)}
.sep.pm{color:var(--sky)}
.note{font-size:11px;color:var(--muted);margin-top:8px;line-height:1.5}
/* insight */
.ins{border-left:3px solid var(--amber);border-radius:0 8px 8px 0;padding:10px 12px;margin-bottom:10px;font-size:12px;line-height:1.6;background:#1c2430}
.ins.ok{border-color:var(--green)}.ins.warn{border-color:var(--red)}
/* stats */
.sg{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px}
.sc{background:#1c2430;border-radius:10px;padding:12px}
.sv{font-size:22px;font-weight:700;font-family:'Space Mono',monospace}
.rbs{display:flex;gap:6px;margin-bottom:12px}
.rb{flex:1;padding:7px 0;border:1px solid var(--border);border-radius:8px;background:transparent;color:var(--muted);font-size:12px;font-weight:500;cursor:pointer;font-family:'DM Sans',sans-serif}
.rb.on{background:var(--card2);color:var(--text);border-color:var(--sky)}
.cw{position:relative;width:100%}
.dr{display:flex;align-items:flex-start;gap:10px;padding:10px 0;border-bottom:1px solid var(--border)}
.dr:last-child{border-bottom:none}
.chip{display:inline-flex;align-items:center;gap:4px;font-size:11px;padding:2px 7px;border-radius:4px;margin-right:3px;margin-top:2px}
.cm{background:rgba(210,153,34,.15);color:var(--amber)}
.ce{background:rgba(88,166,255,.12);color:var(--sky)}
.cx{background:rgba(248,81,73,.12);color:var(--red)}
/* toast */
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#1f2937;border:1px solid var(--border);color:var(--green);padding:10px 20px;border-radius:10px;font-size:13px;font-weight:600;opacity:0;transition:opacity .3s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}
</style></head><body>

<div class="hd">
  <span class="ht">PET FEEDER</span>
  <div style="display:flex;align-items:center;gap:7px">
    <div class="dot"></div>
  </div>
</div>

<div class="tabs">
  <button class="tab on"  onclick="showTab('home')">Trang chủ</button>
  <button class="tab"     onclick="showTab('set')">Cài đặt</button>
  <button class="tab"     onclick="showTab('stat')">Thống kê</button>
</div>

<!-- ══ HOME ══ -->
<div id="tab-home" class="pg on">
  <div class="g2">
    <div class="cnm"><div class="lb">Thời gian</div><div class="vl sky" id="el-t" style="font-size:19px">--:--:--</div></div>
    <div class="cnm"><div class="lb">SD Card</div>
      <div style="display:flex;align-items:center;justify-content:space-between">
        <div class="vl grn" id="el-sd" style="font-size:19px">--</div>
        <span id="el-sdb" class="bdg">--</span>
      </div>
    </div>
  </div>

  <div id="ins-box" class="ins" style="display:none"></div>

  <div class="c">
    <div class="lb">Cân trong bát</div>
    <div style="display:flex;align-items:baseline;gap:8px;margin-top:4px">
      <div class="vl" id="el-w">--</div>
      <span style="color:var(--muted);font-size:13px">g</span>
      <span id="el-lft" style="font-size:11px;color:var(--amber)"></span>
    </div>
    <div class="pb"><div class="pbi" id="el-pb" style="width:0%"></div></div>
    <div style="display:flex;justify-content:space-between;margin-top:5px">
      <span style="font-size:11px;color:var(--muted)">0 g</span>
      <span style="font-size:11px;color:var(--muted)">Mục tiêu: <span id="el-tg">--</span> g</span>
    </div>
  </div>

  <div class="g2">
    <div class="cnm"><div class="lb">Bữa sáng</div><div style="font-family:'Space Mono',monospace;font-size:16px;color:var(--accent)" id="dp-mt">--:--</div><div style="font-size:12px;color:var(--muted);margin-top:3px" id="dp-mg">--g</div></div>
    <div class="cnm"><div class="lb">Buổi chiều</div><div style="font-family:'Space Mono',monospace;font-size:16px;color:var(--accent)" id="dp-et">--:--</div><div style="font-size:12px;color:var(--muted);margin-top:3px" id="dp-eg">--g</div></div>
  </div>

  <div class="c">
    <div class="lb" style="margin-bottom:10px">Điều khiển thủ công</div>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px">
      <button class="btn bf" id="btn-feed" onclick="doFeed()">Cho ăn ngay</button>
      <button class="btn bg"              onclick="doTare()">Reset cân</button>
    </div>
    <div class="note" id="feed-note">Sẽ dùng khẩu phần theo giờ hiện tại.</div>
  </div>
</div>

<!-- ══ SETTINGS ══ -->
<div id="tab-set" class="pg">
  <div class="c">
    <div class="lb" style="margin-bottom:4px">Lịch &amp; khẩu phần</div>
    <div class="sep">☀ Buổi sáng</div>
    <div class="ig3">
      <div class="ig"><span class="il">Giờ</span><input type="number" id="s-mh" min="0" max="23"></div>
      <div class="ig"><span class="il">Phút</span><input type="number" id="s-mm" min="0" max="59"></div>
      <div class="ig"><span class="il">Gram</span><input type="number" id="s-tgM" min="1" max="500"></div>
    </div>
    <div class="sep pm">🌙 Buổi chiều</div>
    <div class="ig3">
      <div class="ig"><span class="il">Giờ</span><input type="number" id="s-eh" min="0" max="23"></div>
      <div class="ig"><span class="il">Phút</span><input type="number" id="s-em" min="0" max="59"></div>
      <div class="ig"><span class="il">Gram</span><input type="number" id="s-tgE" min="1" max="500"></div>
    </div>
    <div style="margin-top:12px"><button class="btn bs" onclick="doSave()">Lưu cài đặt</button></div>
  </div>
  <div class="c">
    <div class="lb" style="margin-bottom:10px">Đồng bộ thời gian RTC</div>
    <input type="datetime-local" id="rtctime">
    <button class="btn bg" onclick="doSetTime()" style="margin-top:8px">Cập nhật thời gian</button>
  </div>
  <div class="c">
    <div class="lb" style="margin-bottom:10px">Tải log SD Card</div>
    <button class="btn bg" onclick="dlToday()">⬇ Tải log hôm nay</button>
  </div>
</div>

<!-- ══ STATS ══ -->
<div id="tab-stat" class="pg">
  <div class="rbs">
    <button class="rb on" id="rb7"  onclick="loadStat(7)">7 ngày</button>
    <button class="rb"    id="rb14" onclick="loadStat(14)">14 ngày</button>
    <button class="rb"    id="rb30" onclick="loadStat(30)">30 ngày</button>
  </div>

  <div class="sg">
    <div class="sc"><div class="lb">TB ăn / ngày</div><div class="sv grn" id="st-avg">--</div><div style="font-size:11px;color:var(--muted)">g / ngày</div></div>
    <div class="sc"><div class="lb">TB thừa / ngày</div><div class="sv amb" id="st-lft">--</div><div style="font-size:11px;color:var(--muted)">g / ngày</div></div>
  </div>
  <div class="sg">
    <div class="sc"><div class="lb">Xu hướng ăn</div><div class="sv" id="st-trd">--</div><div style="font-size:11px;color:var(--muted)">so nửa trước</div></div>
    <div class="sc"><div class="lb">Bỏ bữa</div><div class="sv red" id="st-mis">--</div><div style="font-size:11px;color:var(--muted)">bữa</div></div>
  </div>

  <div id="adv-box" class="ins" style="display:none;margin-bottom:10px"></div>

  <div class="cnm" style="margin-bottom:10px">
    <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:10px">
      <span style="font-size:13px;font-weight:500">Lượng thêm &amp; thừa / ngày</span>
      <div style="display:flex;gap:8px;font-size:11px;color:var(--muted)">
        <span><span style="display:inline-block;width:8px;height:8px;background:#d29922;border-radius:2px;margin-right:2px"></span>Sáng</span>
        <span><span style="display:inline-block;width:8px;height:8px;background:#58a6ff;border-radius:2px;margin-right:2px"></span>Chiều</span>
        <span><span style="display:inline-block;width:8px;height:8px;background:rgba(248,81,73,.6);border-radius:2px;margin-right:2px"></span>Thừa</span>
      </div>
    </div>
    <div class="cw" style="height:200px"><canvas id="chartEat"></canvas></div>
  </div>

  <div class="cnm" style="margin-bottom:10px">
    <div style="font-size:13px;font-weight:500;margin-bottom:10px">Thức ăn thừa trước mỗi bữa sáng</div>
    <div class="cw" style="height:140px"><canvas id="chartLft"></canvas></div>
  </div>

  <div class="cnm">
    <div style="font-size:13px;font-weight:500;margin-bottom:10px">Chi tiết từng ngày</div>
    <div id="dl-list"><div style="color:var(--muted);font-size:13px;text-align:center;padding:20px">Đang tải...</div></div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.js"></script>
<script>
function pad(n){return n<10?'0'+n:String(n)}
function toast(msg,ok=true){
  const t=document.getElementById('toast');
  t.textContent=msg;t.style.color=ok?'var(--green)':'var(--red)';
  t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2200);
}
function fmtG(v){return v==null||v<0?'—':parseFloat(v).toFixed(1)+'g'}

// ── TABS ──
let statLoaded=false,setPopulated=false;
function showTab(n){
  document.querySelectorAll('.tab').forEach((b,i)=>b.classList.toggle('on',['home','set','stat'][i]===n));
  document.querySelectorAll('.pg').forEach(p=>p.classList.remove('on'));
  document.getElementById('tab-'+n).classList.add('on');
  if(n==='stat'&&!statLoaded){loadStat(7);statLoaded=true;}
  if(n==='set'&&!setPopulated){popSet();}
}

// ── LIVE DATA ──
let _d={};
function tick(){
  fetch('/data').then(r=>r.json()).then(d=>{
    _d=d;
    document.getElementById('el-t').textContent=d.time;
    document.getElementById('el-w').textContent=parseFloat(d.weight).toFixed(1);
    const h=parseInt(d.time.split(':')[0]);
    const tg=h>=d.eh?d.tgE:d.tgM;
    document.getElementById('el-tg').textContent=Math.round(tg);
    document.getElementById('el-pb').style.width=Math.min(100,d.weight/tg*100).toFixed(0)+'%';

    // Thừa
    const lft=parseFloat(d.weight);
    const lEl=document.getElementById('el-lft');
    if(d.lastAW>0&&lft>1&&lft<d.lastAW-1)
      lEl.textContent='(thừa '+lft.toFixed(1)+'g từ bữa trước)';
    else lEl.textContent='';

    // Insight
    const ibox=document.getElementById('ins-box');
    if(d.lastAW>0){
      ibox.style.display='block';
      if(lft>d.lastAW*0.4){
        ibox.className='ins warn';
        ibox.innerHTML='⚠ Còn thừa <b>'+lft.toFixed(1)+'g</b> ('+Math.round(lft/d.lastAW*100)+'%). Có thể <b>giảm</b> khẩu phần.';
      } else if(lft<2){
        ibox.className='ins ok';
        ibox.innerHTML='✓ Thú cưng ăn hết. Khẩu phần đang phù hợp.';
      } else {
        ibox.className='ins';
        ibox.innerHTML='Còn thừa <b>'+lft.toFixed(1)+'g</b> trước bữa tiếp theo.';
      }
    }

    const ok=d.sd===1;
    document.getElementById('el-sd').textContent=ok?'OK':'NO';
    document.getElementById('el-sdb').className='bdg '+(ok?'bOK':'bNO');
    document.getElementById('el-sdb').textContent=ok?'READY':'MISS';

    document.getElementById('dp-mt').textContent=pad(d.mh)+':'+pad(d.mm);
    document.getElementById('dp-mg').textContent=Math.round(d.tgM)+'g';
    document.getElementById('dp-et').textContent=pad(d.eh)+':'+pad(d.em);
    document.getElementById('dp-eg').textContent=Math.round(d.tgE)+'g';

    const fb=document.getElementById('btn-feed');
    const isEve=parseInt(d.time.split(':')[0])>=d.eh;
    if(d.feeding){fb.disabled=true;fb.textContent='Đang cho ăn...';}
    else{fb.disabled=false;fb.textContent='Cho ăn ngay';
      document.getElementById('feed-note').textContent=
        'Sẽ dùng khẩu phần '+(isEve?'chiều ('+Math.round(d.tgE)+'g)':'sáng ('+Math.round(d.tgM)+'g)')+'.';
    }
  }).catch(()=>{});
}
tick();setInterval(tick,1500);

function popSet(){
  if(_d.mh===undefined){setTimeout(popSet,500);return;}
  document.getElementById('s-mh').value=_d.mh;
  document.getElementById('s-mm').value=pad(_d.mm);
  document.getElementById('s-tgM').value=Math.round(_d.tgM);
  document.getElementById('s-eh').value=_d.eh;
  document.getElementById('s-em').value=pad(_d.em);
  document.getElementById('s-tgE').value=Math.round(_d.tgE);
  setPopulated=true;
}

// ── ACTIONS ──
function doFeed(){fetch('/feed').then(()=>toast('Đang cho ăn...')).catch(()=>toast('Lỗi',false));}
function doTare(){fetch('/tare').then(()=>toast('Đã reset cân ✓')).catch(()=>toast('Lỗi',false));}
function doSave(){
  const p=new URLSearchParams({
    mh:document.getElementById('s-mh').value,mm:document.getElementById('s-mm').value,
    tgM:document.getElementById('s-tgM').value,
    eh:document.getElementById('s-eh').value,em:document.getElementById('s-em').value,
    tgE:document.getElementById('s-tgE').value
  });
  fetch('/set?'+p).then(()=>{toast('Đã lưu ✓');setPopulated=false;}).catch(()=>toast('Lỗi',false));
}
function doSetTime(){
  const v=document.getElementById('rtctime').value;
  if(!v){toast('Chọn thời gian!',false);return;}
  fetch('/settime?time='+encodeURIComponent(v.replace('T',' ')+':00'))
    .then(()=>toast('Đã cập nhật ✓')).catch(()=>toast('Lỗi',false));
}
function dlToday(){
  const n=new Date();
  window.location='/download?file='+n.getFullYear()+'_'+(n.getMonth()+1)+'_'+n.getDate()+'.txt';
}

// ── STATS ──
let ec=null,lc=null;
function avg_of(a){const v=a.filter(x=>x!=null&&x>=0);return v.length?v.reduce((s,x)=>s+x,0)/v.length:0;}

function loadStat(days){
  ['rb7','rb14','rb30'].forEach(id=>document.getElementById(id).classList.remove('on'));
  document.getElementById('rb'+days).classList.add('on');
  document.getElementById('dl-list').innerHTML='<div style="color:var(--muted);font-size:13px;text-align:center;padding:20px">Đang tải...</div>';
  fetch('/stats?days='+days).then(r=>r.json()).then(data=>renderStat(data))
    .catch(()=>{document.getElementById('dl-list').innerHTML='<div style="color:var(--red);font-size:13px;text-align:center;padding:20px">Không thể tải — kiểm tra SD Card.</div>';});
}

function renderStat(data){
  const labels=data.map(d=>{const p=d.date.split('-');return p[1]+'/'+p[2];});
  const addedM=data.map(d=>d.morning?parseFloat(d.morning.added)||0:0);
  const addedE=data.map(d=>d.evening?parseFloat(d.evening.added)||0:0);
  const lftArr=data.map(d=>d.leftover!=null&&d.leftover>=0?parseFloat(d.leftover):null);

  // Lượng thú cưng ăn sáng = atePrev của bữa chiều cùng ngày
  const ateM=data.map(d=>d.evening&&d.evening.atePrev>=0?d.evening.atePrev:null);
  // Lượng ăn chiều = atePrev của bữa sáng hôm sau (không có → null)
  // Tổng ăn ước tính = ateM (biết) + ateEvening (chưa biết hết)
  const totalAte=ateM.map((v,i)=>v!=null?v:null);

  const validAte=totalAte.filter(v=>v!=null&&v>=0);
  const validLft=lftArr.filter(v=>v!=null);
  const avgAte=validAte.length?Math.round(validAte.reduce((a,b)=>a+b,0)/validAte.length):0;
  const avgLft=validLft.length?(validLft.reduce((a,b)=>a+b,0)/validLft.length).toFixed(1):'—';
  const miss=data.filter(d=>!d.morning||!d.evening).length;

  document.getElementById('st-avg').textContent=avgAte||'—';
  document.getElementById('st-lft').textContent=avgLft;
  document.getElementById('st-mis').textContent=miss;

  const half=Math.floor(data.length/2);
  const diff=Math.round(avg_of(totalAte.slice(half))-avg_of(totalAte.slice(0,half)));
  const te=document.getElementById('st-trd');
  te.textContent=(diff>=0?'+':'')+diff+'g';
  te.style.color=diff>3?'var(--green)':diff<-3?'var(--red)':'var(--text)';

  // Gợi ý
  const adv=document.getElementById('adv-box');
  adv.style.display='block';
  const avgLftN=parseFloat(avgLft)||0;
  const tgM=_d.tgM||40,tgE=_d.tgE||60;
  if(avgLftN>(tgM)*0.35){
    adv.className='ins warn';
    adv.innerHTML='⚠ Trung bình còn thừa <b>'+avgLft+'g</b>/ngày. Nên <b>giảm bữa sáng ~'+Math.round(avgLftN)+'g</b> để thức ăn không bị ôi.';
  } else if(avgAte>0&&avgAte<(tgM+tgE)*0.7){
    adv.className='ins warn';
    adv.innerHTML='⚠ Thú cưng ăn ít hơn mục tiêu. Kiểm tra sức khoẻ hoặc <b>giảm khẩu phần</b>.';
  } else if(avgAte>0&&avgLftN<3){
    adv.className='ins ok';
    adv.innerHTML='✓ Khẩu phần hợp lý — thú cưng ăn đều, ít thừa.';
  } else {
    adv.className='ins';
    adv.innerHTML='Đang thu thập dữ liệu. Cần thêm vài ngày để phân tích chính xác.';
  }

  // Biểu đồ cột stacked
  if(ec)ec.destroy();
  ec=new Chart(document.getElementById('chartEat'),{
    type:'bar',
    data:{labels,datasets:[
      {label:'Sáng thêm', data:addedM,backgroundColor:'#d29922',borderRadius:3,stack:'a'},
      {label:'Chiều thêm',data:addedE,backgroundColor:'#58a6ff',borderRadius:3,stack:'a'},
      {label:'Thừa sáng', data:lftArr.map(v=>v??0),backgroundColor:'rgba(248,81,73,.55)',borderRadius:3,stack:'a'}
    ]},
    options:{responsive:true,maintainAspectRatio:false,
      plugins:{legend:{display:false},tooltip:{callbacks:{label:c=>c.dataset.label+': '+c.raw.toFixed(1)+'g'}}},
      scales:{
        x:{stacked:true,ticks:{color:'#8b949e',font:{size:10},maxRotation:0},grid:{display:false}},
        y:{stacked:true,ticks:{color:'#8b949e',font:{size:10},callback:v=>v+'g'},
           grid:{color:'rgba(255,255,255,.05)'},suggestedMax:130}
      }
    }
  });

  // Biểu đồ thừa
  if(lc)lc.destroy();
  lc=new Chart(document.getElementById('chartLft'),{
    type:'bar',
    data:{labels,datasets:[{
      label:'Thừa',data:lftArr.map(v=>v??0),borderRadius:4,
      backgroundColor:lftArr.map(v=>!v||v<3?'rgba(63,185,80,.55)':v<10?'rgba(210,153,34,.7)':'rgba(248,81,73,.7)')
    }]},
    options:{responsive:true,maintainAspectRatio:false,
      plugins:{legend:{display:false},tooltip:{callbacks:{label:c=>c.raw.toFixed(1)+'g thừa'}}},
      scales:{
        x:{ticks:{color:'#8b949e',font:{size:10},maxRotation:0},grid:{display:false}},
        y:{ticks:{color:'#8b949e',font:{size:10},callback:v=>v+'g'},
           grid:{color:'rgba(255,255,255,.05)'},min:0,suggestedMax:20}
      }
    }
  });

  // Chi tiết
  const list=document.getElementById('dl-list');
  list.innerHTML=[...data].reverse().map(d=>{
    const p=d.date.split('-'),label=p[2]+'/'+p[1];
    const m=d.morning,e=d.evening;
    const lft=d.leftover!=null&&d.leftover>=0?parseFloat(d.leftover).toFixed(1)+'g':'—';
    const lftNum=parseFloat(lft)||0;
    const lftColor=lftNum>10?'var(--red)':lftNum>3?'var(--amber)':'var(--green)';
    const totalAdded=((m?parseFloat(m.added):0)+(e?parseFloat(e.added):0)).toFixed(0);
    const ateM_val=e&&e.atePrev>=0?parseFloat(e.atePrev).toFixed(1)+'g':'—';
    return `<div class="dr">
      <div style="min-width:34px;font-size:12px;color:var(--muted);padding-top:2px">${label}</div>
      <div style="flex:1">
        <div style="margin-bottom:5px">
          ${m?`<span class="chip cm">☀ +${fmtG(m.added)}</span>`:`<span class="chip cx">☀ bỏ</span>`}
          ${e?`<span class="chip ce">🌙 +${fmtG(e.added)}</span>`:`<span class="chip cx">🌙 bỏ</span>`}
        </div>
        <div style="font-size:11px;color:var(--muted);display:flex;gap:14px;flex-wrap:wrap">
          <span>Thừa trước: <b style="color:${lftColor}">${lft}</b></span>
          <span>Ăn sáng: <b style="color:var(--text)">${ateM_val}</b></span>
        </div>
      </div>
      <div style="font-size:12px;color:var(--muted);min-width:40px;text-align:right">+${totalAdded}g</div>
    </div>`;
  }).join('');
}
</script>
</body></html>)rawliteral";

  server.send(200, "text/html", html);
}

// ═══════════════════════════════════════════════════════════
//  RTOS TASKS
// ═══════════════════════════════════════════════════════════
void TaskRTC(void *pv) {
  while (true) {
    DateTime t = rtc.now();
    if (xSemaphoreTake(rtcMutex, portMAX_DELAY)) { now = t; xSemaphoreGive(rtcMutex); }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void TaskLoadcell(void *pv) {
  static float filtered = 0;
  while (true) {
    if (scale.is_ready()) {
      float raw = scale.get_units(1);
      filtered  = 0.7f * filtered + 0.3f * raw;
      float w   = (fabsf(filtered) < 0.5f) ? 0 : filtered;
      if (w < 0) w = 0;
      if (xSemaphoreTake(weightMutex, pdMS_TO_TICKS(20))) { weight = w; xSemaphoreGive(weightMutex); }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void TaskServo(void *pv) {
  int lastDay = -1;
  while (true) {
    DateTime t;
    if (xSemaphoreTake(rtcMutex, portMAX_DELAY)) { t = now; xSemaphoreGive(rtcMutex); }

    if (t.day() != lastDay) {
      fedMorning = false;
      fedEvening = false;
      lastDay    = t.day();
      saveConfig();
    }

    if (xSemaphoreTake(feedSemaphore, 0) == pdTRUE) {
      feedUntilWeight(currentFeedTarget, "X");
    } else {
      if (t.hour()==feedHourMorning && t.minute()==feedMinuteMorning
          && t.second()<2 && !fedMorning) {
        fedMorning = true;
        feedUntilWeight(targetMorning, "M");
      }
      if (t.hour()==feedHourEvening && t.minute()==feedMinuteEvening
          && t.second()<2 && !fedEvening) {
        fedEvening = true;
        feedUntilWeight(targetEvening, "E");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void TaskLCD(void *pv) {
  char line1[17], line2[17];
  String last1="", last2="";
  while (true) {
    DateTime t; float w;
    if (xSemaphoreTake(rtcMutex,    portMAX_DELAY)) { t=now;    xSemaphoreGive(rtcMutex);    }
    if (xSemaphoreTake(weightMutex, portMAX_DELAY)) { w=weight; xSemaphoreGive(weightMutex); }
    snprintf(line1,sizeof(line1),"%02d:%02d  W:%5.1fg",t.hour(),t.minute(),w);
    snprintf(line2,sizeof(line2),"%-16s",WiFi.localIP().toString().c_str());
    String s1(line1),s2(line2);
    if(s1!=last1){lcd.setCursor(0,0);lcd.print(s1);last1=s1;}
    if(s2!=last2){lcd.setCursor(0,1);lcd.print(s2);last2=s2;}
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void TaskWeb(void *pv) {
  static bool printedIP = false;
  while (true) {
    server.handleClient();
    if (WiFi.status()==WL_CONNECTED && !printedIP) {
      Serial.print("IP: "); Serial.println(WiFi.localIP()); printedIP=true;
    }
    vTaskDelay(1);
  }
}

void TaskButton(void *pv) {
  while (true) {
    bool reading = digitalRead(BTN_RESET);
    if (lastBtnState==HIGH && reading==LOW && millis()-lastDebounceTime>200) {
      if (!feeding) {
        DateTime t;
        if (xSemaphoreTake(rtcMutex, portMAX_DELAY)) { t=now; xSemaphoreGive(rtcMutex); }
        currentFeedTarget = (t.hour()>=feedHourEvening) ? targetEvening : targetMorning;
        xSemaphoreGive(feedSemaphore);
        feeding = true;
      }
      lastDebounceTime = millis();
    }
    lastBtnState = reading;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);
  lcd.init(); lcd.backlight();
  rtc.begin();

  pinMode(SERVO_PIN, OUTPUT); digitalWrite(SERVO_PIN, LOW); delay(300);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.write(SERVO_CLOSE); delay(800);
  myServo.detach();

  scale.begin(DT, SCK); delay(2000);
  for (int i = 0; i < 15; i++) { scale.get_units(1); delay(100); }

  loadConfig();
  scale.set_scale(calibration_factor);
  scale.set_offset(offset);
  Serial.printf("[SCALE] offset=%ld | lastAW=%.1f\n", offset, lastAfterWeight);

  SPI.begin(18, 19, 23, SD_CS);
  sdOK = SD.begin(SD_CS);
  Serial.println(sdOK ? "[SD] OK" : "[SD] FAIL");

  pinMode(BTN_RESET, INPUT_PULLUP);
  WiFiManager wm;
  unsigned long t0 = millis();
  while (digitalRead(BTN_RESET) == LOW) {
    if (millis()-t0 > 3000) {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Reset WiFi...");
      wm.resetSettings(); delay(1000); ESP.restart();
    }
    delay(10);
  }

  bool res = wm.autoConnect("PET_FEEDER", "12345678");
  Serial.println(res ? "[WiFi] Connected" : "[WiFi] FAIL");
  if (res) Serial.println(WiFi.localIP());

  server.on("/",         handleRoot);
  server.on("/data",     handleData);
  server.on("/stats",    handleStats);
  server.on("/feed",     handleFeed);
  server.on("/set",      handleSet);
  server.on("/settime",  handleSetTime);
  server.on("/download", handleDownload);
  server.on("/tare", []() {
    scale.tare();
    offset = scale.get_offset();
    prefs.begin("feeder",false); prefs.putLong("offset",offset); prefs.end();
    Serial.printf("[TARE] offset=%ld\n", offset);
    server.send(200,"text/plain","Tared & Saved");
  });
  server.begin();

  rtcMutex      = xSemaphoreCreateMutex();
  weightMutex   = xSemaphoreCreateMutex();
  feedSemaphore = xSemaphoreCreateBinary();

  xTaskCreate(TaskRTC,      "RTC",   2048, NULL, 2, NULL);
  xTaskCreate(TaskButton,   "BTN",   2048, NULL, 2, NULL);
  xTaskCreate(TaskLoadcell, "LOAD",  2048, NULL, 1, NULL);
  xTaskCreate(TaskServo,    "SERVO", 4096, NULL, 1, NULL);
  xTaskCreate(TaskLCD,      "LCD",   2048, NULL, 1, NULL);
  xTaskCreate(TaskWeb,      "WEB",   4096, NULL, 1, NULL);
}

void loop() {}