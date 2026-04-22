#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <arduino-timer.h>

// --- ตั้งค่าขาใช้งาน ---
// เปลี่ยนจาก 18 เป็น 32 ตามที่เราคุยกันว่าบัดกรีง่ายและปลอดภัย
const int MOSFET_PIN = 32; 
const int LED_PIN = 13; // ใช้ LED ในตัวบอร์ดเป็นสัญญาณแสดงสถานะ

// --- วัดแรงดันแบตเตอรี่ ---
const int BAT_ADC_PIN = 35; // ขา ADC สำหรับอ่านแรงดันจาก voltage divider
const float VOLTAGE_DIVIDER_RATIO = 100.0f / (470.0f + 100.0f); // R2 / (R1 + R2)

// --- สถิติ deltaV ---
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

// --- ตัวแปรสำหรับตั้งค่า (เก็บใน Preferences) ---
float pulseMs = 0.2f;      // ???????????????? ms ???????????????? us ??????????
uint32_t intervalMs = 1500; // จังหวะห่าง 1.5 วินาที

// --- วัตถุควบคุมระบบ ---
WebServer server(80);
Preferences pref;
auto timer = timer_create_default();
Timer<>::Task pulseTask; // ตัวแปรอ้างอิง Task เพื่อใช้ในการ Restart Timer

uint32_t pulseMsToUs(float ms) {
  return (uint32_t)(ms * 1000.0f + 0.5f);
}

// --- ฟังก์ชันช่วยเหลือสำหรับวัดแรงดันแบต ---
float readBatteryVoltage() {
  int raw = analogRead(BAT_ADC_PIN);
  float sensorV = raw * (3.3f / 4095.0f);
  return sensorV / VOLTAGE_DIVIDER_RATIO;
}

void updateDeltaStats() {
  if (sampleCount == 0) {
    currentMean = 0.0f;
    currentStdDev = 0.0f;
    return;
  }

  float sum = 0.0f;
  for (int i = 0; i < sampleCount; i++) {
    sum += deltaSamples[i];
  }
  currentMean = sum / sampleCount;

  float sq = 0.0f;
  for (int i = 0; i < sampleCount; i++) {
    float diff = deltaSamples[i] - currentMean;
    sq += diff * diff;
  }
  currentStdDev = sqrt(sq / sampleCount);
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
  uint32_t pulseUs = pulseMsToUs(pulseMs);
  float v_idle = readBatteryVoltage();
  digitalWrite(MOSFET_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH); // เปิด LED ตอน zap
  delayMicroseconds(pulseUs); // ปล่อย Pulse ตามค่าที่จูนไว้
  digitalWrite(MOSFET_PIN, LOW);
  digitalWrite(LED_PIN, LOW); // ปิด LED หลัง zap
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
  return true; // คืนค่า true เพื่อให้ Timer วนซ้ำต่อไป
}

String getLogSummaryHTML() {
  String html = "<h3>Abnormal Events</h3>";
  html += "<p>Mean diff.V: " + String(currentMean, 3) + " V, SD: " + String(currentStdDev, 3) + " V</p>";
  if (logCount == 0) {
    html += "<p>No abnormal events logged yet.</p>";
  } else {
    html += "<table style='width:100%; border-collapse:collapse;'><tr><th style='border-bottom:1px solid #ddd; text-align:left;'>#</th>";
    html += "<th style='border-bottom:1px solid #ddd; text-align:left;'>Time</th>";
    html += "<th style='border-bottom:1px solid #ddd; text-align:left;'>Idle V</th>";
    html += "<th style='border-bottom:1px solid #ddd; text-align:left;'>Zap V</th>";
    html += "<th style='border-bottom:1px solid #ddd; text-align:left;'>d.V</th></tr>";

    int start = (logPos - logCount + LOG_WINDOW_SIZE) % LOG_WINDOW_SIZE;
    int show = min(logCount, 5);
    for (int i = 0; i < show; i++) {
      int idx = (start + logCount - show + i) % LOG_WINDOW_SIZE;
      const DeltaLog &entry = logEntries[idx];
      html += "<tr><td>" + String(i + 1) + "</td>";
      html += "<td>" + String(entry.timeMs) + " ms</td>";
      html += "<td>" + String(entry.v_idle, 2) + " V</td>";
      html += "<td>" + String(entry.v_zap, 2) + " V</td>";
      html += "<td>" + String(entry.delta, 3) + " V</td></tr>";
    }
    html += "</table>";
    if (logCount > show) {
      html += "<p>Showing last " + String(show) + " of " + String(logCount) + " events.</p>";
    }
  }
  return html;
}

String getRawDataHTML() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif; background:#f4f4f4; padding:20px;} table{width:100%; border-collapse:collapse;} ";
  html += "th,td{border:1px solid #ddd; padding:8px; text-align:left;} th{background:#eee;} a.button{display:inline-block;padding:10px 18px;margin:10px 0;background:#e67e22;color:#fff;text-decoration:none;border-radius:6px;}";
  html += "</style></head><body>";
  html += "<div style='background:white;padding:20px;border-radius:16px;box-shadow:0 4px 10px rgba(0,0,0,0.15);'>";
  html += "<h2>Raw d.V Data</h2>";
  html += "<p>Mean d.V: " + String(currentMean, 3) + " V, SD: " + String(currentStdDev, 3) + " V</p>";
  html += "<a class='button' href='/'>Back</a>";
  html += "<table><tr><th>#</th><th>d.V (V)</th></tr>";
  int index = sampleCount;
  int pos = (samplePos - sampleCount + SAMPLE_WINDOW_SIZE) % SAMPLE_WINDOW_SIZE;
  for (int i = 0; i < sampleCount; i++) {
    html += "<tr><td>" + String(i + 1) + "</td>";
    html += "<td>" + String(deltaSamples[pos], 4) + "</td></tr>";
    pos = (pos + 1) % SAMPLE_WINDOW_SIZE;
  }
  html += "</table></div></body></html>";
  return html;
}

// --- หน้าเว็บควบคุม (HTML) ---
String getHTML() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif; text-align:center; background:#f4f4f4; padding:20px;}";
  html += ".card{background:white; padding:30px; border-radius:15px; box-shadow:0 4px 10px rgba(0,0,0,0.2); max-width:500px; margin:auto;}";
  html += ".small-table{width:100%; border-collapse:collapse; margin-top:10px;}";
  html += "input{padding:12px; width:100px; margin:10px; font-size:18px; border:1px solid #ddd; border-radius:5px;}";
  html += "button{padding:15px 30px; background:#e67e22; color:white; border:none; border-radius:8px; font-size:18px; cursor:pointer;}";
  html += "button:hover{background:#d35400;}</style></head><body>";
  
  html += "<div class='card'><h2>Electric Fence V1</h2>";
  html += "<p style='color:#7f8c8d;'>Mode: AP Online</p>";
  html += "<p><b>Current Pulse:</b> " + String(pulseMs, 2) + " ms</p>";
  html += "<p><b>Current Interval:</b> " + String(intervalMs) + " ms</p>";
  html += "<p><b>Battery Sensor Pin:</b> ADC" + String(BAT_ADC_PIN) + "</p>";
  html += "<a href='/raw'><button type='button'>Show Raw d.V Data</button></a>";
  html += getLogSummaryHTML();
  html += "<hr><form action='/save' method='POST'>";
  html += "Pulse Time (ms):<br><input type='number' name='p' value='" + String(pulseMs, 2) + "' min='0.05' max='5.00' step='0.05'><br>";
  html += "Interval (ms):<br><input type='number' name='i' value='" + String(intervalMs) + "' min='500' max='10000'><br><br>";
  html += "<button type='submit'>Save & Update</button></form>";
  html += "<p style='font-size:12px; color:red;'>*Max Pulse is limited to 5.00ms for safety.</p></div></body></html>";
  return html;
}

void setup() {
  Serial.begin(115200);
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, LOW);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // ปิด LED ตอนเริ่มต้น
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

  // 1. โหลดค่าจากหน่วยความจำ (Preferences)
  pref.begin("fence-cfg", false);
  if (pref.isKey("p_ms")) {
    pulseMs = pref.getFloat("p_ms", 0.2f);
  } else {
    pulseMs = pref.getUInt("p", 200) / 1000.0f;
  }
  intervalMs = pref.getUInt("i", 1500);

  if (pulseMs > 5.0f) pulseMs = 5.0f;
  if (pulseMs < 0.05f) pulseMs = 0.05f;

  // 2. ตั้งค่า WiFi (AP Only)
  WiFi.mode(WIFI_AP);
  WiFi.softAP("FENCE_PRO", "admin1234"); // ชื่อและรหัสผ่านของ WiFi AP

  // 3. (ตัด mDNS ออก เพราะใช้ได้เฉพาะโหมด STA หรือ AP+STA)
  // ไม่ต้องตั้งค่า mDNS

  // 4. ตั้งค่า Web Server
  server.on("/", []() {
    server.send(200, "text/html", getHTML());
  });

  server.on("/save", HTTP_POST, []() {
    pulseMs = server.arg("p").toFloat();
    intervalMs = server.arg("i").toInt();

    // Safety Limit: กันพิมพ์ผิดจนคอยล์ไหม้
    if (pulseMs > 5.0f) pulseMs = 5.0f;
    if (pulseMs < 0.05f) pulseMs = 0.05f;
    if (intervalMs < 500) intervalMs = 500;

    // บันทึกค่าลง Preferences
    pref.putFloat("p_ms", pulseMs);
    pref.putUInt("i", intervalMs);

    // รีสตาร์ท Timer ด้วยค่าใหม่
    timer.cancel(pulseTask);
    pulseTask = timer.every(intervalMs, triggerZap);

    server.send(200, "text/html", "<h3>Updated Successfully!</h3><script>setTimeout(function(){window.location.href='/';},1500);</script>");
    Serial.println("Settings saved and Timer restarted!");
  });

  server.on("/raw", []() {
    server.send(200, "text/html", getRawDataHTML());
  });

  server.begin();

  // 5. ตั้งค่า OTA (อัปเดตโค้ดไร้สาย)
  ArduinoOTA.setHostname("Fence-ESP32");
  ArduinoOTA.begin();

  // 6. เริ่มการทำงานของ Timer
  pulseTask = timer.every(intervalMs, triggerZap);

  Serial.println(">>> System Ready! <<<");
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

void loop() {
  timer.tick();       // จัดการจังหวะการปล่อยไฟ
  server.handleClient(); // จัดการหน้าเว็บ
  ArduinoOTA.handle();   // จัดการ OTA
}
