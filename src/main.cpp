#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <arduino-timer.h>

const int MOSFET_PIN = 32;
const int LED_PIN = 13;
const int TOUCH_PIN = T0; // GPIO4 on ESP32 Dev Module
const uint32_t TOUCH_HOLD_MS = 2000;
const uint32_t DEFAULT_NOON_EPOCH_SECONDS = 12UL * 60UL * 60UL;

const int BAT_ADC_PIN = 35;
const float VOLTAGE_DIVIDER_RATIO = 3.3f / (10.0f + 3.3f);

const int SAMPLE_WINDOW_SIZE = 255;
const int LOG_WINDOW_SIZE = 32;

struct DeltaLog {
 uint32_t timeMs;
 float v_idle;
 float v_zap;
 float delta;
};

float deltaSamples[SAMPLE_WINDOW_SIZE];
int samplePos = 0;
int sampleCount = 0;
float currentMean = 0.0f;
float currentStdDev = 0.0f;

DeltaLog logEntries[LOG_WINDOW_SIZE];
int logPos = 0;
int logCount = 0;

float pulseMs = 0.2f;
uint32_t intervalMs = 1500;
uint16_t activeStartMinute = 8 * 60;
uint16_t activeEndMinute = 18 * 60;
int32_t clientUtcOffsetMinutes = 0;
uint32_t syncedEpochSeconds = 0;
uint32_t syncMillisAt = 0;
bool hasTimeSync = false;
uint16_t touchBaseline = 0;
uint16_t touchThreshold = 0;
uint32_t touchPressedSince = 0;

WebServer server(80);
Preferences pref;
auto timer = timer_create_default();
Timer<>::Task pulseTask;

uint32_t pulseMsToUs(float ms) {
 return (uint32_t)(ms * 1000.0f + 0.5f);
}

int getValidSampleCount() {
 return sampleCount;
}

String formatMinuteOfDay(uint16_t totalMinutes) {
 uint16_t hours = (totalMinutes / 60) % 24;
 uint16_t minutes = totalMinutes % 60;
 char buffer[6];
 snprintf(buffer, sizeof(buffer), "%02u:%02u", hours, minutes);
 return String(buffer);
}

String formatSecondOfDay(uint32_t totalSeconds) {
 uint16_t hours = (totalSeconds / 3600UL) % 24;
 uint16_t minutes = (totalSeconds / 60UL) % 60;
 uint16_t seconds = totalSeconds % 60UL;
 char buffer[9];
 snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u", hours, minutes, seconds);
 return String(buffer);
}

uint16_t parseMinuteOfDay(const String &value, uint16_t fallbackValue) {
 int colonPos = value.indexOf(':');
 if (colonPos <= 0) {
 return fallbackValue;
 }

 int hours = value.substring(0, colonPos).toInt();
 int minutes = value.substring(colonPos + 1).toInt();
 if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
 return fallbackValue;
 }

 return (uint16_t)(hours * 60 + minutes);
}

void syncTimeFromClient(uint32_t epochSeconds, int32_t utcOffsetMinutes) {
 syncedEpochSeconds = epochSeconds;
 clientUtcOffsetMinutes = utcOffsetMinutes;
 syncMillisAt = millis();
 hasTimeSync = epochSeconds > 0;
}

void setDefaultNoonTime() {
 int64_t localNoonEpoch = (int64_t)DEFAULT_NOON_EPOCH_SECONDS - (int64_t)clientUtcOffsetMinutes * 60LL;
 if (localNoonEpoch < 0) {
  localNoonEpoch = 0;
 }
 syncedEpochSeconds = (uint32_t)localNoonEpoch;
 syncMillisAt = millis();
 hasTimeSync = true;
}

void calibrateTouchThreshold() {
 uint32_t sum = 0;
 for (int i = 0; i < 16; i++) {
  sum += touchRead(TOUCH_PIN);
  delay(10);
 }

 touchBaseline = (uint16_t)(sum / 16U);
 uint16_t threshold = (touchBaseline * 3U) / 4U;
 if (threshold == 0) {
  threshold = 1;
 }
 if (threshold >= touchBaseline && touchBaseline > 1) {
  threshold = touchBaseline - 1;
 }
 touchThreshold = threshold;

 Serial.print("Touch baseline: ");
 Serial.print(touchBaseline);
 Serial.print(", threshold: ");
 Serial.println(touchThreshold);
}

bool isTouchHeld() {
 if (touchThreshold == 0) {
  return false;
 }

 return touchRead(TOUCH_PIN) < touchThreshold;
}

uint32_t getCurrentLocalEpochSeconds();

String getCurrentTimeText() {
 if (!hasTimeSync) {
  return String("waiting for client time sync");
 }

 uint32_t localEpoch = getCurrentLocalEpochSeconds();
 uint32_t secondsOfDay = localEpoch % 86400UL;
 return formatSecondOfDay(secondsOfDay);
}

void handleTouchBootstrap() {
 if (hasTimeSync) {
  touchPressedSince = 0;
  return;
 }

 if (isTouchHeld()) {
  if (touchPressedSince == 0) {
   touchPressedSince = millis();
  }

  if (millis() - touchPressedSince >= TOUCH_HOLD_MS) {
   if (!hasTimeSync) {
    setDefaultNoonTime();
    Serial.println("Touch held for 2s and time was not set. Defaulting clock to 12:00.");
   }
   touchPressedSince = 0;
  }
 } else {
  touchPressedSince = 0;
 }
}

uint32_t getCurrentLocalEpochSeconds() {
 if (!hasTimeSync) {
 return 0;
 }

 uint32_t elapsedSeconds = (millis() - syncMillisAt) / 1000UL;
 int64_t localEpoch = (int64_t)syncedEpochSeconds + elapsedSeconds + (int64_t)clientUtcOffsetMinutes * 60LL;
 if (localEpoch < 0) {
 return 0;
 }

 return (uint32_t)localEpoch;
}

uint16_t getCurrentMinuteOfDay() {
 uint32_t localEpoch = getCurrentLocalEpochSeconds();
 if (localEpoch == 0) {
 return 0;
 }

 return (uint16_t)((localEpoch % 86400UL) / 60UL);
}

bool isWithinActiveWindow(uint16_t minuteOfDay) {
 if (activeStartMinute == activeEndMinute) {
 return true;
 }

 if (activeStartMinute < activeEndMinute) {
 return minuteOfDay >= activeStartMinute && minuteOfDay < activeEndMinute;
 }

 return minuteOfDay >= activeStartMinute || minuteOfDay < activeEndMinute;
}

bool isFenceActiveNow() {
 if (!hasTimeSync) {
 return false;
 }

 return isWithinActiveWindow(getCurrentMinuteOfDay());
}

float readBatteryVoltage() {
 int raw = analogRead(BAT_ADC_PIN);
 float sensorV = raw * (3.3f / 4095.0f);
 return sensorV / VOLTAGE_DIVIDER_RATIO;
}

void updateDeltaStats() {
 int validSamples = getValidSampleCount();
 if (validSamples == 0) {
 currentMean = 0.0f;
 currentStdDev = 0.0f;
 return;
 }

 float sum = 0.0f;
 for (int i = 0; i < validSamples; i++) {
 sum += deltaSamples[i];
 }
 currentMean = sum / validSamples;

 float sq = 0.0f;
 for (int i = 0; i < validSamples; i++) {
 float diff = deltaSamples[i] - currentMean;
 sq += diff * diff;
 }
 currentStdDev = sqrt(sq / validSamples);
}

void addSample(float delta) {
 deltaSamples[samplePos] = delta;
 samplePos = (samplePos + 1) % SAMPLE_WINDOW_SIZE;
 if (sampleCount < SAMPLE_WINDOW_SIZE) {
 sampleCount++;
 }
 updateDeltaStats();
}

void addLog(float v_idle, float v_zap, float delta) {
 logEntries[logPos] = { millis(), v_idle, v_zap, delta };
 logPos = (logPos + 1) % LOG_WINDOW_SIZE;
 if (logCount < LOG_WINDOW_SIZE) {
 logCount++;
 }
}

bool triggerZap(void *) {
 if (!isFenceActiveNow()) {
 return true;
 }

 uint32_t pulseUs = pulseMsToUs(pulseMs);
 float v_idle = readBatteryVoltage();
 digitalWrite(MOSFET_PIN, HIGH);
 digitalWrite(LED_PIN, HIGH);
 delayMicroseconds(pulseUs);
 digitalWrite(MOSFET_PIN, LOW);
 digitalWrite(LED_PIN, LOW);
 float v_zap = readBatteryVoltage();
 float deltaV = v_idle - v_zap;

 addSample(deltaV);
 if (sampleCount >= 16) {
 if (deltaV > currentMean + 2.0f * currentStdDev || deltaV < currentMean - 2.0f * currentStdDev) {
  addLog(v_idle, v_zap, deltaV);
 }
 }

 Serial.print("Zap! ");
 Serial.print(pulseMs, 2);
 Serial.print(" ms (");
 Serial.print(pulseUs);
 Serial.print(" us), dV=");
 Serial.print(deltaV, 3);
 Serial.println(" V");
 return true;
}

static const char log_summary_html[] PROGMEM = R"rawliteral(
<h3>Abnormal Events</h3>
<p>Mean Diff: %MEAN_DV% , SD: %STD_DEV% </p>
<p>Samples used: %VALID_SAMPLES% / %SAMPLE_WINDOW%</p>
%BODY%
)rawliteral";

static const char log_summary_no_events_html[] PROGMEM = R"rawliteral(
<p>No abnormal events logged yet.</p>
)rawliteral";

static const char log_summary_table_open_html[] PROGMEM = R"rawliteral(
<table style='width:100%; border-collapse:collapse;'>
  <tr>
    <th style='border-bottom:1px solid #ddd; text-align:left;'>#</th>
    <th style='border-bottom:1px solid #ddd; text-align:left;'>Time</th>
    <th style='border-bottom:1px solid #ddd; text-align:left;'>Idle</th>
    <th style='border-bottom:1px solid #ddd; text-align:left;'>Zap</th>
    <th style='border-bottom:1px solid #ddd; text-align:left;'>V</th>
  </tr>
)rawliteral";

static const char log_summary_more_html[] PROGMEM = R"rawliteral(
<p>Showing last %SHOW% of %COUNT% events.</p>
)rawliteral";

String getLogSummaryHTML() {
 String html = FPSTR(log_summary_html);
 String body;

 if (logCount == 0) {
  body = FPSTR(log_summary_no_events_html);
 } else {
  body = FPSTR(log_summary_table_open_html);

  int start = (logPos - logCount + LOG_WINDOW_SIZE) % LOG_WINDOW_SIZE;
  int show = min(logCount, 5);
  for (int i = 0; i < show; i++) {
   int idx = (start + logCount - show + i) % LOG_WINDOW_SIZE;
   const DeltaLog &entry = logEntries[idx];
   body += "<tr><td>" + String(i + 1) + "</td>";
   body += "<td>" + String(entry.timeMs) + " ms</td>";
   body += "<td>" + String(entry.v_idle, 2) + " V</td>";
   body += "<td>" + String(entry.v_zap, 2) + " V</td>";
   body += "<td>" + String(entry.delta, 3) + " V</td></tr>";
  }
  body += "</table>";

  if (logCount > show) {
   String moreLine = FPSTR(log_summary_more_html);
   moreLine.replace("%SHOW%", String(show));
   moreLine.replace("%COUNT%", String(logCount));
   body += moreLine;
  }
 }

 html.replace("%MEAN_DV%", String(currentMean, 3));
 html.replace("%STD_DEV%", String(currentStdDev, 3));
 html.replace("%VALID_SAMPLES%", String(getValidSampleCount()));
 html.replace("%SAMPLE_WINDOW%", String(SAMPLE_WINDOW_SIZE));
 html.replace("%BODY%", body);
 return html;
}

static const char raw_data_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
  <head>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <style>
      body {
        font-family: sans-serif;
        background: #f4f4f4;
        padding: 20px;
      }
      table {
        width: 100%;
        border-collapse: collapse;
      }
      th, td {
        border: 1px solid #ddd;
        padding: 8px;
        text-align: left;
      }
      th {
        background: #eee;
      }
      a.button {
        display: inline-block;
        padding: 10px 18px;
        margin: 10px 0;
        background: #e67e22;
        color: #fff;
        text-decoration: none;
        border-radius: 6px;
      }
    </style>
  </head>
  <body>
    <div style='background:white;padding:20px;border-radius:16px;box-shadow:0 4px 10px rgba(0,0,0,0.15);'>
      <h2>Raw Data</h2>
      <p>Mean: %MEAN_DV% , SD: %STD_DEV% </p>
      <p>Calculated from measured samples only: %VALID_SAMPLES% / %SAMPLE_WINDOW%</p>
      <a class='button' href='/'>Back</a>
      <table>
        <tr>
          <th>#</th>
          <th>V</th>
        </tr>
        %ROWS%
      </table>
    </div>
  </body>
</html>
)rawliteral";

static const char update_success_html[] PROGMEM = R"rawliteral(
<h3>Updated Successfully!</h3>
<script>
  setTimeout(function () {
    window.location.href = '/';
  }, 1500);
</script>
)rawliteral";

String getRawDataHTML() {
 String html = FPSTR(raw_data_html);
 String rows;
 rows.reserve(sampleCount * 48);
 int pos = (samplePos - sampleCount + SAMPLE_WINDOW_SIZE) % SAMPLE_WINDOW_SIZE;
 for (int i = 0; i < sampleCount; i++) {
 rows += "<tr><td>" + String(i + 1) + "</td>";
 rows += "<td>" + String(deltaSamples[pos], 4) + "</td></tr>";
 pos = (pos + 1) % SAMPLE_WINDOW_SIZE;
 }
 html.replace("%MEAN_DV%", String(currentMean, 3));
 html.replace("%STD_DEV%", String(currentStdDev, 3));
 html.replace("%VALID_SAMPLES%", String(getValidSampleCount()));
 html.replace("%SAMPLE_WINDOW%", String(SAMPLE_WINDOW_SIZE));
 html.replace("%ROWS%", rows);
 return html;
}

static const char schedule_status_html[] PROGMEM = R"rawliteral(
<p><b>Active Window:</b> %ACTIVE_WINDOW%</p>
%CLOCK_LINE%
<p><b>Fence State:</b> %FENCE_STATE% | <b>Battery:</b> %BAT_VOLTAGE% V</p>
)rawliteral";

String getScheduleStatusHTML() {
 String html = FPSTR(schedule_status_html);
 String batteryVoltage = String(readBatteryVoltage(), 2);
 html.replace("%ACTIVE_WINDOW%", formatMinuteOfDay(activeStartMinute) + " - " + formatMinuteOfDay(activeEndMinute));
 html.replace("%BAT_VOLTAGE%", batteryVoltage);
 if (!hasTimeSync) {
 html.replace("%CLOCK_LINE%", "<p style='color:#c0392b;'><b>Clock:</b> waiting for client time sync</p>");
 html.replace("%FENCE_STATE%", "PAUSED");
 return html;
 }

 String clockLine = "<p><b>Clock:</b> " + formatMinuteOfDay(getCurrentMinuteOfDay()) + " (client offset ";
 if (clientUtcOffsetMinutes >= 0) {
 clockLine += "+";
 }
 clockLine += String(clientUtcOffsetMinutes);
 clockLine += " min)</p>";
 html.replace("%CLOCK_LINE%", clockLine);
 html.replace("%FENCE_STATE%", isFenceActiveNow() ? "ACTIVE" : "PAUSED");
 return html;
}


// HTML template in PROGMEM with placeholders
static const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
    <head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
        <style>
        body {
            font-family: sans-serif;
            text-align: center;
            background: #f4f4f4;
            padding: 20px;
        }
        .card {
            background: white;
            padding: 30px;
            border-radius: 15px;
            box-shadow: 0 4px 10px rgba(0, 0, 0, 0.2);
            max-width: 500px;
            margin: auto;
        }
        .small-table {
            width: 100%;
            border-collapse: collapse;
            margin-top: 10px;
        }
        input {
            padding: 12px;
            width: 140px;
            margin: 10px;
            font-size: 18px;
            border: 1px solid #ddd;
            border-radius: 5px;
            }
        button {
            padding: 15px 30px;
            background: #e67e22;
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 18px;
            cursor: pointer;
        }
        button:hover {
            background: #d35400;
        }
        </style>
    </head>
 <body>
    <div class="card">
    <h2>Electric Fence V1</h2>
    <p style="color: #7f8c8d">Mode: AP Online</p>
    <p><b>Current Pulse:</b> %PULSE_MS% ms</p>
    <p><b>Current Interval:</b> %INTERVAL_MS% ms</p>
    <p><b>ESP32 Time:</b> <span id="esp32-time">%ESP32_TIME%</span></p>
    %SCHEDULE_STATUS%
    <p><b>Battery Sensor Pin:</b> ADC%BAT_PIN%</p>
    <a href="/raw"><button type="button">Raw Data</button></a>
    %LOG_SUMMARY%
    <hr />
    <form action="/save" method="POST"  onsubmit="return applyClientTime(this);">
            Pulse Time (ms):<br />
            <input type="number" name="p" value="%PULSE_MS%" min="0.05" max="5.00" step="0.05" /><br />
            Interval (ms):<br />
            <input type="number"  name="i" value="%INTERVAL_MS%" min="500" max="10000"  /><br />
            Start Time:<br />
            <input type="time" name="open" value="%START_TIME%"  /><br />
            Stop Time:<br />
            <input type="time" name="close" value="%STOP_TIME%"  /><br /><br />
            <input type="hidden" name="client_epoch" value="0" />
            <input type="hidden" name="tz_offset" value="0" />
            <button type="submit">Save & Update</button>
    </form>
    <script>
    function applyClientTime(form) {
    var now = new Date();
    form.client_epoch.value = Math.floor(now.getTime() / 1000);
    form.tz_offset.value = -now.getTimezoneOffset();
    return true;
    }

    function refreshEsp32Time() {
    fetch('/time')
      .then(function (response) { return response.text(); })
      .then(function (text) {
        var timeEl = document.getElementById('esp32-time');
        if (timeEl) {
          timeEl.textContent = text;
        }
      })
      .catch(function () {});
    }

    refreshEsp32Time();
    setInterval(refreshEsp32Time, 5000);
    </script>
    <p style="font-size: 12px; color: red">
    *Max Pulse is limited to 5.00ms for safety.
    </p>
    </div>
 </body>
</html>
)rawliteral";


String getHTML() {
 String html = FPSTR(index_html);
 html.replace("%PULSE_MS%", String(pulseMs, 2));
 html.replace("%INTERVAL_MS%", String(intervalMs));
 html.replace("%ESP32_TIME%", getCurrentTimeText());
 html.replace("%SCHEDULE_STATUS%", getScheduleStatusHTML());
 html.replace("%BAT_PIN%", String(BAT_ADC_PIN));
 html.replace("%LOG_SUMMARY%", getLogSummaryHTML());
 html.replace("%START_TIME%", formatMinuteOfDay(activeStartMinute));
 html.replace("%STOP_TIME%", formatMinuteOfDay(activeEndMinute));
 return html;
}

void setup() {
 Serial.begin(115200);
 pinMode(MOSFET_PIN, OUTPUT);
 digitalWrite(MOSFET_PIN, LOW);
 pinMode(LED_PIN, OUTPUT);
 digitalWrite(LED_PIN, LOW);
 analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
 calibrateTouchThreshold();

 pref.begin("fence-cfg", false);
 if (pref.isKey("p_ms")) {
 pulseMs = pref.getFloat("p_ms", 0.2f);
 } else {
 pulseMs = pref.getUInt("p", 200) / 1000.0f;
 }
 intervalMs = pref.getUInt("i", 1500);
 activeStartMinute = (uint16_t)pref.getUInt("open_min", 8 * 60);
 activeEndMinute = (uint16_t)pref.getUInt("close_min", 18 * 60);
 clientUtcOffsetMinutes = pref.getInt("tz_min", 0);

 if (pulseMs > 5.0f) pulseMs = 5.0f;
 if (pulseMs < 0.05f) pulseMs = 0.05f;
 if (intervalMs < 500) intervalMs = 500;

 WiFi.mode(WIFI_AP);
 WiFi.softAP("FENCE_PRO", "admin1234");

 if (!hasTimeSync) {
  Serial.println("Time is not set.");
  Serial.println("Hold the touch pad for 2 seconds to initialize the clock.");
 }

 server.on("/", []() {
 server.send(200, "text/html", getHTML());
 });

 server.on("/time", []() {
 server.send(200, "text/plain", getCurrentTimeText());
 });

 server.on("/save", HTTP_POST, []() {
 pulseMs = server.arg("p").toFloat();
 intervalMs = server.arg("i").toInt();
 activeStartMinute = parseMinuteOfDay(server.arg("open"), activeStartMinute);
 activeEndMinute = parseMinuteOfDay(server.arg("close"), activeEndMinute);

 uint32_t clientEpochSeconds = (uint32_t)server.arg("client_epoch").toInt();
 int32_t tzOffsetMinutes = server.arg("tz_offset").toInt();

 if (pulseMs > 5.0f) pulseMs = 5.0f;
 if (pulseMs < 0.05f) pulseMs = 0.05f;
 if (intervalMs < 500) intervalMs = 500;

 pref.putFloat("p_ms", pulseMs);
 pref.putUInt("i", intervalMs);
 pref.putUInt("open_min", activeStartMinute);
 pref.putUInt("close_min", activeEndMinute);

 if (clientEpochSeconds > 0) {
  syncTimeFromClient(clientEpochSeconds, tzOffsetMinutes);
  pref.putInt("tz_min", clientUtcOffsetMinutes);
 }

 timer.cancel(pulseTask);
 pulseTask = timer.every(intervalMs, triggerZap);

 server.send(200, "text/html", FPSTR(update_success_html));
 Serial.println("Settings saved and timer restarted.");
 });

 server.on("/raw", []() {
 server.send(200, "text/html", getRawDataHTML());
 });

 server.begin();

 ArduinoOTA.setHostname("Fence-ESP32");
 ArduinoOTA.begin();

 pulseTask = timer.every(intervalMs, triggerZap);

 Serial.println(">>> System Ready! <<<");
 Serial.print("AP IP: ");
 Serial.println(WiFi.softAPIP());
}

void loop() {
 handleTouchBootstrap();
 timer.tick();
 server.handleClient();
 ArduinoOTA.handle();
}
