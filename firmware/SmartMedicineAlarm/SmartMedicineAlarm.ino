/*
  SMART MEDICINE ALERT
  ESP32 + IR sensor + Servo + Buzzer + I2C LCD
  Local Wi-Fi Access Point and Web API

  GPIO:
  Buzzer       -> GPIO 33
  Servo        -> GPIO 32
  IR sensor OUT-> GPIO 35
  LCD SDA      -> GPIO 21
  LCD SCL      -> GPIO 22

  Wi-Fi SSID   : Smart_Medicine_Alarm
  Wi-Fi Password: 12345678
  Dashboard    : open http://192.168.4.1 in any phone/laptop browser
                 (dashboard/dashboard.py also still works)

  Default medicine schedule: 08:00, 14:00, 20:00
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_system.h>

// ---------------- WIFI ACCESS POINT ----------------

const char* AP_SSID = "Smart_Medicine_Alarm";
const char* AP_PASSWORD = "12345678";

IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ---------------- GPIO PINS ------------------------

#define BUZZER_PIN 33
#define SERVO_PIN  32
#define IR_SENSOR_PIN 35

// Many IR obstacle modules output LOW when detecting.
// Change to HIGH if your module's output is HIGH on detection.
#define IR_OBJECT_DETECTED LOW

// A sensor reading must hold this long before the alarm trusts it.
// Filters flicker from falling tablets and servo motor noise.
// Raise it if the alarm still stops by itself.
const unsigned long IR_STABLE_MS = 500;

// ---------------- OBJECTS --------------------------

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo dispenserServo;
WebServer server(80);
Preferences prefs;

// ---------------- SERVO ----------------------------

const int SERVO_START_ANGLE = 0;
const int SERVO_STEP_DEGREES = 60;
const int SERVO_MAX_ANGLE = 180;

// Pulse range: 0 deg = 500 us, 180 deg = 2400 us. Calibrate per servo.
const int SERVO_MIN_US = 500;
const int SERVO_MAX_US = 2400;

// Smooth slow movement with ease-in/ease-out.
// Move time = degrees x SERVO_MS_PER_DEGREE.
// 35 -> 60 deg dose step ~2.1 s, 180 deg reset ~6.3 s. Bigger = slower.
const unsigned long SERVO_MS_PER_DEGREE = 35;
const unsigned long SERVO_UPDATE_MS = 20;   // servo frame is 20 ms (50 Hz)

int currentServoAngle = SERVO_START_ANGLE;  // target angle
float servoPos = SERVO_START_ANGLE;         // live angle sent to servo
float moveFrom = SERVO_START_ANGLE;
int moveTarget = SERVO_START_ANGLE;
unsigned long moveStart = 0;
unsigned long moveDuration = 0;
unsigned long lastServoStep = 0;

// ---------------- MEDICINE SCHEDULE ----------------

struct DoseTime {
  int hour;
  int minute;
};

DoseTime doses[3] = {
  {8, 0},
  {14, 0},
  {20, 0}
};

bool doseDone[3] = {false, false, false};
int slotsLeft = 3;
int storedDay = -1;

// ---------------- SYSTEM STATUS --------------------

String currentTimeStr = "--:--";
String nextDoseStr = "--:--";
String statusStr = "Starting";
bool clockSet = false;

// ---------------- ALARM / IR STATE ----------------

bool alarmActive = false;
bool buzzerOn = false;
bool tabletSeenDuringAlarm = false;

bool irRaw = false;              // last raw sensor reading
bool irStable = false;           // reading after IR_STABLE_MS filter
unsigned long irChangedAt = 0;

unsigned long lastBuzzerToggle = 0;
const unsigned long buzzerToggleMs = 200;

// ---------------- TRIGGER PROTECTION ----------------

int lastTriggeredDose = -1;
int lastTriggeredYDay = -1;
int lastTriggeredMinute = -1;

// ---------------- TIME FORMATTING ------------------

String formatTime12(int hour24, int minute) {
  String ampm = (hour24 >= 12) ? "PM" : "AM";

  int hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;

  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d %s",
           hour12, minute, ampm.c_str());

  return String(buf);
}

String formatTime24(int hour24, int minute) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d",
           hour24, minute);
  return String(buf);
}

String jsonEscape(const String& input) {
  String output;

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];

    if (c == '"' || c == '\\') {
      output += '\\';
      output += c;
    } else if (c == '\n') {
      output += "\\n";
    } else if (c != '\r') {
      output += c;
    }
  }

  return output;
}

// ---------------- JSON RESPONSE ---------------------

void sendJson(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

// ---------------- CLOCK -----------------------------

bool getLocalTimeSafe(struct tm &timeinfo) {
  if (!clockSet) return false;

  time_t now = time(nullptr);
  if (now < 1700000000) return false;

  localtime_r(&now, &timeinfo);
  return true;
}

void setDeviceEpoch(time_t epoch) {
  // India Standard Time: UTC+05:30
  setenv("TZ", "IST-5:30", 1);
  tzset();

  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;

  settimeofday(&tv, nullptr);
  clockSet = (epoch > 1700000000);
}

// ---------------- SAVE / LOAD SCHEDULE --------------

void saveDoseTimes() {
  prefs.begin("med", false);

  prefs.putInt("d1h", doses[0].hour);
  prefs.putInt("d1m", doses[0].minute);

  prefs.putInt("d2h", doses[1].hour);
  prefs.putInt("d2m", doses[1].minute);

  prefs.putInt("d3h", doses[2].hour);
  prefs.putInt("d3m", doses[2].minute);

  prefs.end();
}

void loadDoseTimes() {
  prefs.begin("med", true);

  doses[0].hour = prefs.getInt("d1h", 8);
  doses[0].minute = prefs.getInt("d1m", 0);

  doses[1].hour = prefs.getInt("d2h", 14);
  doses[1].minute = prefs.getInt("d2m", 0);

  doses[2].hour = prefs.getInt("d3h", 20);
  doses[2].minute = prefs.getInt("d3m", 0);

  prefs.end();

  for (int i = 0; i < 3; i++) {
    doses[i].hour = constrain(doses[i].hour, 0, 23);
    doses[i].minute = constrain(doses[i].minute, 0, 59);
  }
}

// ---------------- LCD -------------------------------

void showLCD(String line1, String line2) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));

  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void showNormalLCD() {
  if (!clockSet) {
    showLCD("Set device time", "Open 192.168.4.1");
  } else {
    showLCD(currentTimeStr,
            "L:" + String(slotsLeft) + " N:" + nextDoseStr);
  }
}

void showDoseLCD() {
  showLCD("Take Medicine", "Remove tablets");
}


// ---------------- NEXT DOSE -------------------------

String computeNextDoseString(struct tm &timeinfo) {
  int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  int bestIndex = -1;
  int bestMinutes = 1441;

  for (int i = 0; i < 3; i++) {
    int doseMinutes =
      doses[i].hour * 60 + doses[i].minute;

    if (!doseDone[i] &&
        doseMinutes >= nowMinutes &&
        doseMinutes < bestMinutes) {
      bestMinutes = doseMinutes;
      bestIndex = i;
    }
  }

  if (bestIndex >= 0) {
    return formatTime12(doses[bestIndex].hour,
                        doses[bestIndex].minute);
  }

  return "Tomorrow";
}

// ---------------- DAILY RESET ----------------------

void resetDayIfNeeded(struct tm &timeinfo) {
  if (storedDay == -1) {
    storedDay = timeinfo.tm_yday;
    return;
  }

  if (timeinfo.tm_yday != storedDay) {
    storedDay = timeinfo.tm_yday;

    for (int i = 0; i < 3; i++) {
      doseDone[i] = false;
    }

    slotsLeft = 3;

    lastTriggeredDose = -1;
    lastTriggeredYDay = -1;
    lastTriggeredMinute = -1;

    statusStr = "New day reset";
  }
}

// ---------------- START ALARM -----------------------

void startAlarm() {
  alarmActive = true;
  buzzerOn = false;
  lastBuzzerToggle = 0;

  // Do not assume tablets were picked up.
  // The sensor must detect tablets present and then absent.
  tabletSeenDuringAlarm = false;

  showDoseLCD();

  Serial.println("ALARM: waiting for tablet removal");
}

// ---------------- UPDATE ALARM ----------------------

bool readTabletPresent() {
  bool raw = (digitalRead(IR_SENSOR_PIN) == IR_OBJECT_DETECTED);
  unsigned long now = millis();

  if (raw != irRaw) {
    irRaw = raw;
    irChangedAt = now;
  }

  if (now - irChangedAt >= IR_STABLE_MS) {
    irStable = irRaw;
  }

  return irStable;
}

void updateAlarm() {
  // Sample every loop so the filter is current when the alarm needs it.
  bool tabletPresent = readTabletPresent();

  if (!alarmActive) return;

  // Ignore the sensor while the disk turns. One flicker (falling tablet,
  // motor noise) used to stop the alarm within milliseconds.
  if (servoPos == currentServoAngle) {
    // First confirm that the tablets are present.
    if (tabletPresent) {
      tabletSeenDuringAlarm = true;
    }

    // Stop the alarm only after tablets were detected
    // and are subsequently removed.
    if (tabletSeenDuringAlarm && !tabletPresent) {
      alarmActive = false;
      buzzerOn = false;

      digitalWrite(BUZZER_PIN, LOW);

      statusStr = "Medicine taken";

      Serial.println("Tablet removal detected. Alarm stopped.");

      showLCD("Medicine Taken", "Alarm stopped");
      return;
    }
  }

  // Keep sounding indefinitely until tablet removal.
  unsigned long now = millis();

  if (lastBuzzerToggle == 0 ||
      now - lastBuzzerToggle >= buzzerToggleMs) {

    lastBuzzerToggle = now;
    buzzerOn = !buzzerOn;

    digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);
  }
}

// ---------------- SERVO MOVEMENT --------------------

void moveToNextSlot() {
  if (currentServoAngle >= SERVO_MAX_ANGLE) {
    // Day rolled over without Reset doses: disk is on its last slot.
    Serial.println("Servo at last slot. Refill the disk, then Reset doses.");
    return;
  }

  currentServoAngle += SERVO_STEP_DEGREES;

  if (currentServoAngle > SERVO_MAX_ANGLE) {
    currentServoAngle = SERVO_MAX_ANGLE;
  }

  // updateServo() moves there slowly.
  Serial.printf("Servo target: %d degrees\n",
                currentServoAngle);
}

// Called every loop. Eases from current position to currentServoAngle.
void updateServo() {
  unsigned long now = millis();

  // New target (dose or reset): plan a move from where the servo is now.
  if (currentServoAngle != moveTarget) {
    moveFrom = servoPos;
    moveTarget = currentServoAngle;
    moveStart = now;
    moveDuration = (unsigned long)(fabs(moveTarget - moveFrom) * SERVO_MS_PER_DEGREE);
  }

  if (servoPos == moveTarget) return;
  if (now - lastServoStep < SERVO_UPDATE_MS) return;
  lastServoStep = now;

  float t = moveDuration ? (float)(now - moveStart) / moveDuration : 1.0f;

  if (t >= 1.0f) {
    servoPos = moveTarget;  // exact end, stops further updates
  } else {
    float eased = (1.0f - cosf(t * PI)) / 2.0f;  // slow start, slow stop
    servoPos = moveFrom + (moveTarget - moveFrom) * eased;
  }

  // Microseconds give ~0.1 deg resolution instead of whole degrees.
  dispenserServo.writeMicroseconds(
    SERVO_MIN_US + (int)((SERVO_MAX_US - SERVO_MIN_US) * servoPos / 180.0f));
}

// ---------------- DISPENSE DOSE ---------------------

void dispenseDose(int doseIndex, struct tm &timeinfo) {
  doseDone[doseIndex] = true;

  slotsLeft--;
  if (slotsLeft < 0) slotsLeft = 0;

  statusStr = "Dose due";

  // Move the servo and sound the alarm.
  moveToNextSlot();
  startAlarm();

  Serial.printf("Dose %d due at %s\n",
                doseIndex + 1,
                formatTime12(timeinfo.tm_hour,
                             timeinfo.tm_min).c_str());
}

// ---------------- SCHEDULE CHECK --------------------

void checkSchedule() {
  struct tm timeinfo;

  if (!getLocalTimeSafe(timeinfo)) {
    currentTimeStr = "--:--";
    nextDoseStr = "--:--";

    if (!alarmActive) {
      statusStr = "Waiting for time sync";
    }
    return;
  }

  resetDayIfNeeded(timeinfo);

  currentTimeStr =
    formatTime12(timeinfo.tm_hour, timeinfo.tm_min);

  for (int i = 0; i < 3; i++) {
    if (!doseDone[i] &&
        timeinfo.tm_hour == doses[i].hour &&
        timeinfo.tm_min == doses[i].minute) {

      if (!(lastTriggeredDose == i &&
            lastTriggeredYDay == timeinfo.tm_yday &&
            lastTriggeredMinute == timeinfo.tm_min)) {

        lastTriggeredDose = i;
        lastTriggeredYDay = timeinfo.tm_yday;
        lastTriggeredMinute = timeinfo.tm_min;

        dispenseDose(i, timeinfo);
      }
    }
  }

  nextDoseStr = computeNextDoseString(timeinfo);

  if (slotsLeft == 0 && !alarmActive) {
    statusStr = "All doses done";
  } else if (!alarmActive && statusStr != "Manual reset") {
    statusStr = "Waiting";
  }

  // Do not overwrite the alert screen while the alarm is active.
  if (!alarmActive) {
    showNormalLCD();
  }
}

// ---------------- RESET DOSES -----------------------

void resetDoses() {
  for (int i = 0; i < 3; i++) {
    doseDone[i] = false;
  }

  slotsLeft = 3;

  // lastTriggered* is kept on purpose: a reset during a dose's own
  // minute must not fire that dose again straight away.

  // Stop the alarm and reset the servo.
  alarmActive = false;
  buzzerOn = false;
  tabletSeenDuringAlarm = false;

  digitalWrite(BUZZER_PIN, LOW);

  currentServoAngle = SERVO_START_ANGLE;   // updateServo() returns slowly

  statusStr = "Manual reset";

  showLCD("Reset Done", "3 slots left");
}

// ---------------- STATUS JSON -----------------------

String statusJson() {
  struct tm ti;
  bool timeOk = getLocalTimeSafe(ti);

  if (timeOk) {
    currentTimeStr =
      formatTime12(ti.tm_hour, ti.tm_min);

    nextDoseStr = computeNextDoseString(ti);
  }

  String json = "{";

  json += "\"device\":\"Smart Medicine Alert\",";
  json += "\"ssid\":\"" + jsonEscape(String(AP_SSID)) + "\",";
  json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"clock_set\":";
  json += timeOk ? "true," : "false,";
  json += "\"current_time\":\"" + jsonEscape(currentTimeStr) + "\",";
  json += "\"next_dose\":\"" + jsonEscape(nextDoseStr) + "\",";
  json += "\"status\":\"" + jsonEscape(statusStr) + "\",";
  json += "\"slots_left\":" + String(slotsLeft) + ",";
  json += "\"alarm_active\":";
  json += alarmActive ? "true," : "false,";
  json += "\"tablet_seen\":" ;
  json += tabletSeenDuringAlarm ? "true," : "false,";
  json += "\"tablet_now\":";              // live filtered sensor reading
  json += irStable ? "true," : "false,";
  json += "\"client_count\":" +
          String(WiFi.softAPgetStationNum()) + ",";
  json += "\"servo_angle\":" + String((int)lroundf(servoPos)) + ",";
  json += "\"doses\":[";

  for (int i = 0; i < 3; i++) {
    if (i > 0) json += ",";

    json += "{\"hour\":" + String(doses[i].hour);
    json += ",\"minute\":" + String(doses[i].minute);
    json += ",\"time\":\"" +
            formatTime12(doses[i].hour,
                         doses[i].minute) + "\"";
    json += ",\"done\":";
    json += doseDone[i] ? "true}" : "false}";
  }

  json += "]}";

  return json;
}

// ---------------- ROOT WEB PAGE ---------------------
// Phone/laptop dashboard served by the ESP32 itself.
// Self-contained (no CDN): the access point has no internet.
// The browser sends its own clock to /api/settime, so no PC app is needed.

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>Smart Medicine</title>
<style>
:root{--bg:#f3f5f7;--card:#fff;--text:#141920;--muted:#4b5563;--line:#d8dde4;--accent:#0f766e;--on-accent:#fff;--danger:#b42318;--danger-bg:#fdecea;--ok:#146c43;--r:12px;--r-sm:8px}
@media (prefers-color-scheme:dark){:root{--bg:#0f1318;--card:#171d25;--text:#e7ecf1;--muted:#a4aeba;--line:#2b343f;--accent:#2dd4bf;--on-accent:#06201c;--danger:#ff8f85;--danger-bg:#3a1613;--ok:#6ee7a8}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:960px;margin:0 auto;padding:16px}
header{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:baseline;gap:4px 16px;margin-bottom:16px}
h1{font-size:1.5rem;line-height:1.2;margin:0}
h2{font-size:1.05rem;margin:0 0 8px}
#conn{margin:0;font-size:.9rem;color:var(--muted)}
#conn.bad{color:var(--danger);font-weight:600}
.alarm{background:var(--danger-bg);border:2px solid var(--danger);border-radius:var(--r);padding:16px;margin-bottom:16px}
.alarm strong{display:block;font-size:1.3rem;color:var(--danger)}
.metrics{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:16px}
.metric,section{background:var(--card);border:1px solid var(--line);border-radius:var(--r)}
.metric{padding:12px 14px}
.metric span{display:block;font-size:.85rem;color:var(--muted)}
.metric b{display:block;font-size:1.25rem;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.grid{display:grid;gap:16px}
section{padding:16px}
.dose{display:grid;grid-template-columns:1fr auto;grid-template-areas:"l s" "i i";align-items:center;gap:6px 12px;padding:10px 0}
.dose label{grid-area:l}.dose input{grid-area:i}.dose .tag{grid-area:s}
@media (min-width:420px){.dose{grid-template-columns:4.5rem minmax(0,1fr) auto;grid-template-areas:"l i s"}.dose input{max-width:11rem}}
.dose+.dose{border-top:1px solid var(--line)}
label{font-weight:600}
input[type=time]{font:inherit;font-size:1.1rem;width:100%;min-height:44px;padding:6px 10px;color:var(--text);background:var(--bg);border:1px solid var(--line);border-radius:var(--r-sm)}
.tag{font-size:.9rem;color:var(--muted)}
.tag.done{color:var(--ok);font-weight:600}
.msg{min-height:1.5em;margin:8px 0 0;font-weight:600}
.msg.bad{color:var(--danger)}.msg.ok{color:var(--ok)}
.hint{margin:4px 0 12px;font-size:.9rem;color:var(--muted)}
dl{display:grid;grid-template-columns:auto 1fr;gap:8px 16px;margin:0 0 16px}
dt{color:var(--muted)}dd{margin:0;text-align:right;font-weight:600}
.btn{font:inherit;font-weight:600;min-height:44px;padding:0 18px;border-radius:var(--r-sm);border:1px solid transparent;cursor:pointer}
.btn:active{transform:translateY(1px)}
.btn:disabled{opacity:.6;cursor:wait}
.primary{background:var(--accent);color:var(--on-accent)}
.danger{background:transparent;color:var(--danger);border-color:var(--danger);width:100%}
:focus-visible{outline:3px solid var(--accent);outline-offset:2px}
@media (min-width:720px){.metrics{grid-template-columns:repeat(4,1fr)}}
@media (min-width:900px){.grid{grid-template-columns:3fr 2fr;align-items:start}}
@media (prefers-reduced-motion:no-preference){.alarm{animation:blink 1.2s ease-in-out infinite}}
@keyframes blink{50%{border-color:transparent}}
</style></head>
<body><main>
<header><h1>Smart Medicine Alert</h1><p id="conn" role="status">Connecting...</p></header>
<div id="alarm" class="alarm" role="alert" hidden><strong>Time to take medicine</strong><span id="alarmMsg"></span></div>
<div class="metrics">
<div class="metric"><span>Device time</span><b id="now">--</b></div>
<div class="metric"><span>Next dose</span><b id="next">--</b></div>
<div class="metric"><span>Slots left</span><b id="slots">--</b></div>
<div class="metric"><span>Status</span><b id="status">--</b></div>
</div>
<div class="grid">
<section aria-labelledby="sh"><h2 id="sh">Daily schedule</h2>
<form id="form" novalidate>
<div class="dose"><label for="t1">Dose 1</label><input type="time" id="t1" required><span id="s1" class="tag">--</span></div>
<div class="dose"><label for="t2">Dose 2</label><input type="time" id="t2" required><span id="s2" class="tag">--</span></div>
<div class="dose"><label for="t3">Dose 3</label><input type="time" id="t3" required><span id="s3" class="tag">--</span></div>
<p id="formMsg" class="msg" role="status"></p>
<p class="hint">Saving changes only the times. Doses already given today stay given. To start over, use Reset today's doses.</p>
<button class="btn primary" id="save" type="submit">Save schedule</button>
</form></section>
<section aria-labelledby="dh"><h2 id="dh">Device</h2>
<dl>
<dt>Clock</dt><dd id="clock">--</dd>
<dt>Tablet sensor</dt><dd id="ir">--</dd>
<dt>Servo angle</dt><dd id="servo">--</dd>
<dt>Connected devices</dt><dd id="clients">--</dd>
<dt>Last update</dt><dd id="upd">Never</dd>
</dl>
<button class="btn danger" id="reset" type="button">Reset today's doses</button>
</section>
</div></main>
<script>
const $=id=>document.getElementById(id);
const pad=n=>String(n).padStart(2,'0');
const OFFLINE='Not connected. Join the Smart_Medicine_Alarm Wi-Fi.';
let dirty=false,busy=false,lastSync=0,wasAlarm=false;

async function api(path){
  const opt={cache:'no-store'};
  if(AbortSignal.timeout)opt.signal=AbortSignal.timeout(4000);
  const j=await (await fetch(path,opt)).json();
  if(j.ok===false){const e=new Error(j.error||'Device error');e.device=true;throw e;}
  return j;
}
function syncTime(){
  lastSync=Date.now();
  return api('/api/settime?epoch='+Math.floor(Date.now()/1000));
}
function setConn(ok,text){$('conn').textContent=ok?'Connected':text;$('conn').className=ok?'':'bad';}
function setMsg(text,cls){$('formMsg').textContent=text;$('formMsg').className='msg '+cls;}

function render(d){
  const a=d.alarm_active;
  $('alarm').hidden=!a;
  $('alarmMsg').textContent=d.tablet_seen
    ?'Tablets detected. Take them from the tray to stop the alarm.'
    :'Waiting for the sensor to detect the tablets.';
  if(a&&!wasAlarm&&navigator.vibrate)navigator.vibrate([400,200,400,200,400]);
  wasAlarm=a;
  $('now').textContent=d.clock_set?d.current_time:'Not set';
  $('next').textContent=d.next_dose;
  $('slots').textContent=d.slots_left+' of 3';
  $('status').textContent=d.status;
  $('clock').textContent=d.clock_set?'Synchronized':'Not set';
  $('ir').textContent=d.tablet_now?'Sees tablets':'Clear';
  $('servo').textContent=d.servo_angle+'°';
  $('clients').textContent=d.client_count;
  $('upd').textContent=new Date().toLocaleTimeString();
  d.doses.forEach((x,i)=>{
    const n=i+1,s=$('s'+n);
    if(!dirty)$('t'+n).value=pad(x.hour)+':'+pad(x.minute);
    s.textContent=x.done?'Dispensed':'Pending';
    s.className='tag'+(x.done?' done':'');
  });
}

async function refresh(){
  if(busy)return;
  busy=true;
  try{
    let d=await api('/api/status');
    // Clock is lost on reboot; also resync hourly to correct drift.
    if(!d.clock_set||Date.now()-lastSync>36e5){await syncTime();d=await api('/api/status');}
    render(d);setConn(true);
  }catch(e){setConn(false,e.device?e.message:OFFLINE);}
  busy=false;
}

['t1','t2','t3'].forEach(id=>$(id).addEventListener('input',()=>{dirty=true;}));

$('form').addEventListener('submit',async e=>{
  e.preventDefault();
  const q=new URLSearchParams(),seen=new Set();
  for(let n=1;n<=3;n++){
    const v=$('t'+n).value;
    if(!v){setMsg('Enter a time for dose '+n+'.','bad');$('t'+n).focus();return;}
    if(seen.has(v)){setMsg('Dose '+n+' has the same time as another dose.','bad');$('t'+n).focus();return;}
    seen.add(v);
    const p=v.split(':');q.set('h'+n,+p[0]);q.set('m'+n,+p[1]);
  }
  $('save').disabled=true;
  try{await api('/api/schedule?'+q);dirty=false;setMsg('Schedule saved.','ok');refresh();}
  catch(err){setMsg(err.device?err.message:'Could not reach the device.','bad');}
  $('save').disabled=false;
});

$('reset').addEventListener('click',async()=>{
  if(!confirm("Reset today's doses and return the servo to the start?"))return;
  try{await api('/api/reset');refresh();}
  catch(e){setConn(false,e.device?e.message:OFFLINE);}
});

refresh();
setInterval(refresh,3000);
</script></body></html>)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// ---------------- SET DEVICE TIME API ---------------

void handleSetTime() {
  if (!server.hasArg("epoch")) {
    sendJson(400,
      "{\"ok\":false,\"error\":\"Missing epoch\"}");
    return;
  }

  time_t epoch = (time_t)server.arg("epoch").toInt();

  if (epoch < 1700000000) {
    sendJson(400,
      "{\"ok\":false,\"error\":\"Invalid epoch\"}");
    return;
  }

  setDeviceEpoch(epoch);
  statusStr = "Clock synchronized";

  sendJson(200,
    "{\"ok\":true,\"message\":\"Clock synchronized\"}");
}

// ---------------- SET SCHEDULE API ------------------

void handleSetSchedule() {
  int newHours[3];
  int newMinutes[3];

  for (int i = 0; i < 3; i++) {
    String hKey = "h" + String(i + 1);
    String mKey = "m" + String(i + 1);

    if (!server.hasArg(hKey) || !server.hasArg(mKey)) {
      sendJson(400,
        "{\"ok\":false,\"error\":\"Provide h1,m1,h2,m2,h3,m3\"}");
      return;
    }

    newHours[i] = server.arg(hKey).toInt();
    newMinutes[i] = server.arg(mKey).toInt();

    if (newHours[i] < 0 || newHours[i] > 23 ||
        newMinutes[i] < 0 || newMinutes[i] > 59) {
      sendJson(400,
        "{\"ok\":false,\"error\":\"Invalid time range\"}");
      return;
    }
  }

  for (int i = 0; i < 3; i++) {
    doses[i].hour = newHours[i];
    doses[i].minute = newMinutes[i];
  }

  saveDoseTimes();

  // Only the times change. Doses already given today stay given, so a
  // dose is never dispensed twice and the servo stays in step with
  // slotsLeft. "Reset today's doses" is the way to start over.

  statusStr = "Schedule saved";

  sendJson(200,
    "{\"ok\":true,\"message\":\"Schedule saved\"}");
}

// ---------------- RESET API --------------------------

void handleReset() {
  resetDoses();

  sendJson(200,
    "{\"ok\":true,\"message\":\"Doses reset\"}");
}

// ---------------- WEB SERVER SETUP ------------------

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);

  server.on("/api/status", HTTP_GET, []() {
    sendJson(200, statusJson());
  });

  server.on("/api/settime", HTTP_GET, handleSetTime);
  server.on("/api/schedule", HTTP_GET, handleSetSchedule);
  server.on("/api/reset", HTTP_GET, handleReset);

  server.onNotFound([]() {
    sendJson(404,
      "{\"ok\":false,\"error\":\"Not found\"}");
  });

  server.begin();
}

// ---------------- SETUP -----------------------------

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // GPIO35 is input-only and has no internal pull-up/down.
  pinMode(IR_SENSOR_PIN, INPUT);

  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();

  showLCD("Medicine System", "Starting...");

  // A weak 5 V supply resets the ESP32 when the servo starts. That also
  // wipes the clock, so the dose never completes. Make it visible.
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    Serial.println("Last reset: BROWNOUT. Power the servo from a 5 V 2 A supply.");
    showLCD("Power dip reset", "Check servo 5V");
    delay(3000);
  }

  loadDoseTimes();

  // Set the servo to zero degrees at startup.
  // Position before power-on is unknown, so this first move is full speed.
  currentServoAngle = SERVO_START_ANGLE;
  servoPos = SERVO_START_ANGLE;
  moveTarget = SERVO_START_ANGLE;

  dispenserServo.setPeriodHertz(50);
  dispenserServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
  dispenserServo.write(SERVO_START_ANGLE);

  delay(500);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);

  bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (apStarted) {
    Serial.println("\nWi-Fi access point started");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("Password: ");
    Serial.println(AP_PASSWORD);
    Serial.print("Dashboard: http://");
    Serial.println(WiFi.softAPIP());

    statusStr = "AP ready - sync time";
    showLCD("WiFi: Medicine", "192.168.4.1");
  } else {
    statusStr = "WiFi AP failed";
    showLCD("WiFi AP failed", "Restart device");
  }

  setupWebServer();

  Serial.println("Web server ready.");
  Serial.println("IR tablet-removal alarm enabled.");
}

// ---------------- MAIN LOOP -------------------------

unsigned long lastScheduleCheck = 0;

void loop() {
  server.handleClient();

  updateAlarm();
  updateServo();

  unsigned long now = millis();

  if (now - lastScheduleCheck >= 500) {
    lastScheduleCheck = now;
    checkSchedule();
  }
}