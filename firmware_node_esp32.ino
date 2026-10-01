/*
 * ==============================================================================
 * FIRMWARE NODE MONITORING MOTOR - PT BEKAERT INDONESIA (REVISI 5 + REMOTE CONSOLE)
 * Hardware: ESP32 + MPU6050 (Vibrasi) + MAX31865 PT100 (Suhu) + LoRa SX1276 (923MHz)
 *
 * FITUR TERINTEGRASI:
 *  1. Dual Transmisi Sensor: LoRa (923MHz) + Ingest Backend Web (HTTPS) tiap 30 detik
 *  2. Sampling 1 kHz MPU6050 di FreeRTOS Core 1 (Window RMS 10 detik, anti-glitch)
 *  3. Pembacaan Suhu PT100 3-Kawat berkala dengan retry & auto-clear fault
 *  4. Supervisor Otomatis Sensor (Auto-recovery & Restart terkontrol jika sensor macet)
 *  5. Local Web Monitor & Local OTA di port 80 (WiFi lokal)
 *  6. [BARU] Terintegrasi penuh dengan Remote Console & OTA Manager (server.js):
 *       - Outbound HTTP/HTTPS Sync: Mengirim log serial & telemetri ke server
 *       - Menerima remote command: "restart", "send_now", dan "ota"
 *       - Adaptive Polling: 5 detik saat admin menonton console, 30 detik saat idle
 *       - Remote OTA: Mengunduh, verifikasi MD5, dan flashing otomatis dari web console
 * ==============================================================================
 */

#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_MAX31865.h>
#include <LoRa.h>

// ==============================================================================
// 1. KONFIGURASI IDENTITAS NODE, PERIODE & VIBRASI
// ==============================================================================
#define NODE_ID             1                     // ID Node (1 s/d 20)
const char* FIRMWARE_NAME   = "Rev 5 (Remote Console)";

#define WEB_INTERVAL_MS     30000UL               // Kirim ke backend web tiap 30 detik
#define LORA_INTERVAL_MS    30000UL               // Kirim LoRa tiap 30 detik
#define WEB_RETRY_MS        10000UL               // Dipakai jika WEB_MAX_RETRY > 0
#define WEB_MAX_RETRY       0                     // 0 = tanpa retry cepat

#define FIRST_SEND_MIN_WINDOWS  5
#define FIRST_SEND_MAX_WAIT_MS  15000UL

#define SAMPLE_RATE_HZ      1000                  // Sampling akselerometer
#define RMS_WINDOW          1000                  // 1000 sampel = 1 detik
#define VIB_AVG_WINDOWS     10                    // Rata-rata 10 window terakhir (10 detik)
#define VIB_STALE_MS        5000                  // Window RMS basi jika > 5 detik

// Sumbu yang dipakai untuk vibrasi: 0=3-sumbu, 1=X, 2=Y, 3=Z
#define VIB_AXIS_MODE       2

// Kalibrasi vibrasi: hasil = GAIN * mentah + OFFSET
#define VIB_CAL_GAIN        1.0f
#define VIB_CAL_OFFSET      0.0f

#define HP_FC_HZ            10.0f
#define INT_FC_HZ           5.0f

#define ALLOW_SIMULATION    0                     // 0 = tidak ada data palsu
#define SEND_HEALTH_FLAGS   1                     // 1 = sertakan mpu_ok dan rtd_ok di payload
#define TX_ENABLED          1                     // 1 = transmisi aktif, 0 = mode uji
#define TX_STAGGER_MS       4000UL                // LoRa dikirim dulu, backend menyusul 4 detik
#define TX_QUARANTINE_MS    8000UL                // Window dekat TX tidak dirata-ratakan
#define WIFI_LOW_TX_POWER   1                     // 1 = daya WiFi 11 dBm (kurangi lonjakan arus)
#define PT100_READ_TRIES    3                     // Retry pembacaan PT100 jika fault
#define PIN_MPU_PWR         -1                    // -1 = VCC dari 3V3

#define SENSOR_RESTART_AFTER_MS   300000UL        // Restart otomatis jika sensor mati > 5 menit
#define SENSOR_MAX_RESTARTS       5               // Maksimal 5x restart berurutan
#define SENSOR_GOOD_RESET_MS      600000UL        // Reset hitungan restart jika sehat 10 menit
#define PT100_POLL_MS             10000UL         // PT100 dibaca berkala tiap 10 detik
#define WEB_SEND_ONLY_VALID       0

#define MPU_ADDR            0x68
#define I2C_CLOCK_HZ        400000
#define MPU_RETRY_MS        5000
#define FROZEN_LIMIT        100

// ==============================================================================
// 2. KONFIGURASI WI-FI & OTA NIRKABEL
// ==============================================================================
const char* WIFI_SSID     = "SPWN_H37_FC3E99";
const char* WIFI_PASSWORD = "5gm7338rb9dq93e";
const char* OTA_PASSWORD  = "bekaert2026";

String otaHostname = "esp32-bearing-node-" + String(NODE_ID < 10 ? "0" : "") + String(NODE_ID);

// ==============================================================================
// 2b. INGEST DATA SENSOR KE BACKEND TELEMETRI PUBLIK (HTTPS / PARALEL LORA)
// ==============================================================================
#define HTTP_DIRECT_ENABLED       1               // 1 = kirim ke backend data sensor
const char* BACKEND_URL      = "https://iscmotor.ptbi.web.id/api/ingest";
const char* BACKEND_API_KEY  = "bekaert-isc-2026";
const unsigned long HTTP_TIMEOUT_MS = 8000;
const bool HTTPS_INSECURE = true;

const char ROOT_CA_PEM[] PROGMEM = R"rawliteral(
-----BEGIN CERTIFICATE-----
TEMPEL_ROOT_CA_DI_SINI
-----END CERTIFICATE-----
)rawliteral";

#define HTTP_MIN_HEAP_BYTES       40000
#define HTTP_FAIL_REJOIN_STREAK   10
#define HTTP_FAIL_RESTART_STREAK  60

// ==============================================================================
// 2c. KONEKSI KE REMOTE CONSOLE & OTA MANAGER (server.js)
// ==============================================================================
#define REMOTE_CONSOLE_ENABLED    1               // 1 = aktifkan sinkronisasi ke server.js

// URL Server Remote Console:
// - Jika di jaringan WiFi lokal yang sama: gunakan IP PC/laptop Anda, misal: "http://192.168.1.100:3000"
// - Jika di-deploy ke cloud (Railway / Render / VPS): gunakan URL publik Anda, misal: "https://nama-app.up.railway.app"
const char* REMOTE_SERVER_URL     = "http://192.168.1.100:3000";

// API Key perangkat: HARUS SAMA PERSIS dengan DEVICE_API_KEY di file .env server
const char* REMOTE_API_KEY        = "bekaert_esp32_device_key_2026";

// Variabel status sinkronisasi Remote Console
static uint32_t espBootId = 0;
static uint32_t lastRemoteSyncedLogId = 0;
static unsigned long lastRemoteSyncTime = 0;
static unsigned long remoteSyncIntervalMs = 5000; // Mulai dengan 5 detik, otomatis diatur server

// ==============================================================================
// 3. PINOUT HARDWARE ESP32
// ==============================================================================
#define PIN_CS_MAX31865   5
#define PIN_LORA_NSS      4
#define PIN_LORA_RST      14
#define PIN_LORA_DIO0     26
#define PIN_I2C_SDA       21
#define PIN_I2C_SCL       22
#define PIN_BATTERY       35
#define LED_PIN           2

// ==============================================================================
// 4. KONFIGURASI SENSOR & AMBANG BATAS
// ==============================================================================
#define RREF              430.0
#define RNOMINAL          100.0
#define LORA_FREQUENCY    923E6

const float THRESHOLD_WARNING = 1.8;   // mm/s
const float THRESHOLD_FAULT   = 4.5;   // mm/s

#define WDT_TIMEOUT_SECONDS   15

constexpr float DT_S             = 1.0f / SAMPLE_RATE_HZ;
constexpr float HP_ALPHA         = 1.0f / (1.0f + 2.0f * PI * HP_FC_HZ * DT_S);
constexpr float INTEGRATION_LEAK = 1.0f - 2.0f * PI * INT_FC_HZ * DT_S;
const float ACCEL_GLITCH_THRESHOLD_G = 3.5f;

Adafruit_MAX31865 rtd = Adafruit_MAX31865(PIN_CS_MAX31865);
Adafruit_MPU6050 mpu;

WebServer webServer(80);
bool webServerStarted = false;

// ==============================================================================
// BUFFER LOG SERIAL MONITOR & DUAL STREAM REDIRECTION
// ==============================================================================
#define MAX_LOG_LINES 70
#define MAX_LOG_LINE_LEN 160

struct LogEntry {
  uint32_t id;
  uint32_t ms;
  char text[MAX_LOG_LINE_LEN];
};

static LogEntry logHistory[MAX_LOG_LINES];
static int logHead = 0;
static int logCount = 0;
static uint32_t nextLogId = 1;
static char currentLogLine[MAX_LOG_LINE_LEN];
static size_t currentLogLineLen = 0;

void appendCharToWebLog(char c) {
  if (c == '\r') return;
  if (c == '\n') {
    if (currentLogLineLen > 0) {
      currentLogLine[currentLogLineLen] = '\0';
      logHistory[logHead].id = nextLogId++;
      logHistory[logHead].ms = millis();
      strncpy(logHistory[logHead].text, currentLogLine, MAX_LOG_LINE_LEN - 1);
      logHistory[logHead].text[MAX_LOG_LINE_LEN - 1] = '\0';
      logHead = (logHead + 1) % MAX_LOG_LINES;
      if (logCount < MAX_LOG_LINES) logCount++;
      currentLogLineLen = 0;
    }
  } else {
    if (currentLogLineLen < MAX_LOG_LINE_LEN - 1) {
      currentLogLine[currentLogLineLen++] = c;
    }
  }
}

String escapeJsonString(const char* input) {
  String out = "";
  while (*input) {
    if (*input == '"') out += "\\\"";
    else if (*input == '\\') out += "\\\\";
    else if (*input == '\t') out += "\\t";
    else if (*input == '\n') out += "\\n";
    else if ((unsigned char)*input >= 32) out += *input;
    input++;
  }
  return out;
}

class DualSerialStream : public Print {
private:
  HardwareSerial* _hwSerial;
public:
  DualSerialStream(HardwareSerial* hw) : _hwSerial(hw) {}
  void begin(unsigned long baud) { if (_hwSerial) _hwSerial->begin(baud); }
  void flush() { if (_hwSerial) _hwSerial->flush(); }
  size_t write(uint8_t c) override {
    if (_hwSerial) _hwSerial->write(c);
    appendCharToWebLog((char)c);
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    if (_hwSerial) _hwSerial->write(buffer, size);
    for (size_t i = 0; i < size; i++) appendCharToWebLog((char)buffer[i]);
    return size;
  }
};

static DualSerialStream DualSerial(&Serial);
#define Serial DualSerial

// ==============================================================================
// 5. VARIABEL KEADAAN SENSOR & TELEMETRI
// ==============================================================================
struct AxisState { float pin = 0, pout = 0, vel = 0; };
static AxisState axs[3];
static float sumSq[3] = {0, 0, 0};
static float sumSqTot = 0;
static float sumG[3]  = {0, 0, 0};
static int   sampleCount = 0;
static int   warmupLeft  = SAMPLE_RATE_HZ;

static float vibRing[VIB_AVG_WINDOWS];
static int   vibRingN   = 0;
static int   vibRingPos = 0;

volatile float    vibRmsX = 0, vibRmsY = 0, vibRmsZ = 0, vibRmsTot = 0;
volatile float    vibAvgRaw = 0;
volatile float    gMean[3] = {0, 0, 0};
volatile float    vibActualFs = 0;
volatile uint32_t vibWindowsDone = 0;
volatile uint32_t vibLastWindowMs = 0;
volatile bool     mpuOK = false;
volatile uint8_t  vibEvent = 0;
volatile uint32_t glitchCount = 0;
volatile uint32_t mpuDropCount = 0;
volatile uint32_t mpuDropAfterTxMs = 0;
volatile uint32_t lastTxMs = 0;
volatile uint32_t txQuarantineUntil = 0;
volatile uint32_t vibWindowsQuarantined = 0;
static bool       axisInit = false;

unsigned long lastLoraTime = 0;
unsigned long lastWebTime = 0;
unsigned long setupDoneMs = 0;
bool          firstSendDone = false;
unsigned long lastWifiCheck = 0;
unsigned long wifiDownSince = 0;

unsigned long webRetryAt = 0;
String        pendingWebPayload = "";
unsigned long pendingWebAt = 0;

#define RTC_MAGIC_VALUE 0xB3A12E57UL
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t sensorRestartCount;
unsigned long sensorBadSince = 0;
unsigned long sensorGoodSince = 0;
unsigned long lastPt100Poll = 0;
uint8_t       webRetryCount = 0;

float latestVibrationRMS = 0.0f;
float latestTemperature = 0.0f;
float latestBattery = 3.90f;
String latestStatus = "NORMAL";
bool  latestVibValid = false;
bool  latestTempValid = false;

bool loraOK = false;
int  loraFail = 0;

// Queue Ingest Telemetri (Core 0)
#define HTTP_PAYLOAD_MAX  260
#define HTTP_MSG_MAX      140

struct HttpPayload { char body[HTTP_PAYLOAD_MAX]; };
struct HttpResult  { int code; char msg[HTTP_MSG_MAX]; };

QueueHandle_t httpPayloadQueue = NULL;
QueueHandle_t httpResultQueue  = NULL;

uint32_t      httpOkCount = 0, httpFailCount = 0, httpFailStreak = 0;
int           httpLastCode = 0;
unsigned long httpLastOkMs = 0;

// Forward declarations
void sendNodeData(bool doLora, bool doWeb);
void readPT100();
void syncWithRemoteConsole();
void reportRemoteCmdResult(uint32_t cmdId, const String& status, const String& message);
void performRemoteOTA(uint32_t cmdId, const String& fwId, const String& expectedMd5, size_t expectedSize);

// ==============================================================================
// BATERAI & SENSOR VIBRASI / SUHU
// ==============================================================================
float readBatteryVoltage() {
  int raw = analogRead(PIN_BATTERY);
  if (raw < 100) return 3.90f;
  float v = (raw / 4095.0f) * 3.3f * 2.0f * 1.05f;
  return constrain(v, 2.5f, 4.35f);
}

float getVibRaw() { return vibAvgRaw; }

float getVibCalibrated() {
  float v = VIB_CAL_GAIN * getVibRaw() + VIB_CAL_OFFSET;
  return v < 0 ? 0 : v;
}

bool vibIsValid() {
  return mpuOK && vibWindowsDone > 0 && vibRingN > 0 && (millis() - vibLastWindowMs) < VIB_STALE_MS;
}

void configureMPU() {
  mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
  mpu.setFilterBandwidth(MPU6050_BAND_260_HZ);
  Wire.setClock(I2C_CLOCK_HZ);
}

void resetVibState() {
  for (int i = 0; i < 3; i++) { axs[i].pin = 0; axs[i].pout = 0; axs[i].vel = 0; sumSq[i] = 0; sumG[i] = 0; }
  sumSqTot = 0; sampleCount = 0; warmupLeft = SAMPLE_RATE_HZ; axisInit = false;
  vibRingN = 0; vibRingPos = 0; vibAvgRaw = 0;
  vibRmsX = 0; vibRmsY = 0; vibRmsZ = 0; vibRmsTot = 0; vibWindowsDone = 0;
}

void recoverI2CBus() {
  Wire.end();
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, OUTPUT);
  digitalWrite(PIN_I2C_SCL, HIGH);
  for (int i = 0; i < 9 && digitalRead(PIN_I2C_SDA) == LOW; i++) {
    digitalWrite(PIN_I2C_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
  }
  pinMode(PIN_I2C_SDA, OUTPUT);
  digitalWrite(PIN_I2C_SDA, LOW);  delayMicroseconds(5);
  digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
  digitalWrite(PIN_I2C_SDA, HIGH); delayMicroseconds(5);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setTimeOut(20);
}

bool mpuHealthy() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU_ADDR, 1) != 1) return false;
  uint8_t pwr = Wire.read();
  return (pwr & 0x40) == 0;
}

void processVibrationSample(const sensors_event_t& a) {
  float acc[3] = { a.acceleration.x, a.acceleration.y, a.acceleration.z };
  if (!axisInit) {
    for (int i = 0; i < 3; i++) axs[i].pin = acc[i];
    axisInit = true;
  }
  bool glitch = false;
  for (int i = 0; i < 3; i++) {
    if (fabsf(acc[i] - axs[i].pin) > ACCEL_GLITCH_THRESHOLD_G * 9.80665f) { glitch = true; break; }
  }
  if (glitch) {
    for (int i = 0; i < 3; i++) axs[i].pin = acc[i];
    glitchCount++;
    return;
  }

  float vsq = 0;
  for (int i = 0; i < 3; i++) {
    float hp = HP_ALPHA * (axs[i].pout + acc[i] - axs[i].pin);
    axs[i].pin = acc[i];
    axs[i].pout = hp;
    axs[i].vel = axs[i].vel * INTEGRATION_LEAK + hp * DT_S;
    float v = axs[i].vel * 1000.0f;
    if (warmupLeft <= 0) {
      sumSq[i] += v * v;
      sumG[i]  += acc[i] / 9.80665f;
    }
    vsq += v * v;
  }

  if (warmupLeft > 0) { warmupLeft--; return; }

  sumSqTot += vsq;
  if (++sampleCount >= RMS_WINDOW) {
    float n = (float)sampleCount;
    vibRmsX   = sqrtf(sumSq[0] / n);
    vibRmsY   = sqrtf(sumSq[1] / n);
    vibRmsZ   = sqrtf(sumSq[2] / n);
    vibRmsTot = sqrtf(sumSqTot / n);
    gMean[0] = sumG[0] / n; gMean[1] = sumG[1] / n; gMean[2] = sumG[2] / n;

    float sel;
#if VIB_AXIS_MODE == 1
    sel = vibRmsX;
#elif VIB_AXIS_MODE == 2
    sel = vibRmsY;
#elif VIB_AXIS_MODE == 3
    sel = vibRmsZ;
#else
    sel = vibRmsTot;
#endif

    if (txQuarantineUntil == 0 || (int32_t)(millis() - txQuarantineUntil) >= 0) {
      vibRing[vibRingPos] = sel;
      vibRingPos = (vibRingPos + 1) % VIB_AVG_WINDOWS;
      if (vibRingN < VIB_AVG_WINDOWS) vibRingN++;
      float s = 0;
      for (int i = 0; i < vibRingN; i++) s += vibRing[i];
      vibAvgRaw = s / vibRingN;
    } else {
      vibWindowsQuarantined++;
    }

    vibLastWindowMs = millis();
    vibWindowsDone++;
    for (int i = 0; i < 3; i++) { sumSq[i] = 0; sumG[i] = 0; }
    sumSqTot = 0;
    sampleCount = 0;
  }
}

void mpuPowerCycle() {
#if PIN_MPU_PWR >= 0
  digitalWrite(PIN_MPU_PWR, LOW);
  vTaskDelay(pdMS_TO_TICKS(200));
  digitalWrite(PIN_MPU_PWR, HIGH);
  vTaskDelay(pdMS_TO_TICKS(150));
#endif
}

void vibTask(void*) {
  TickType_t period = pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ);
  if (period < 1) period = 1;
  TickType_t last = xTaskGetTickCount();

  uint32_t fsCount = 0, fsT0 = millis(), lastRetry = 0, lastHealth = millis();
  uint8_t  healthFail = 0;
  uint32_t frozenCount = 0;
  float    l0 = 0, l1 = 0, l2 = 0;

  for (;;) {
    vTaskDelayUntil(&last, period);

    if (!mpuOK) {
      if (millis() - lastRetry > MPU_RETRY_MS) {
        lastRetry = millis();
        mpuPowerCycle();
        recoverI2CBus();
        if (mpu.begin(MPU_ADDR)) {
          configureMPU();
          resetVibState();
          healthFail = 0;
          frozenCount = 0;
          lastHealth = millis();
          fsCount = 0; fsT0 = millis();
          mpuOK = true;
          vibEvent = 2;
        }
        last = xTaskGetTickCount();
      }
      continue;
    }

    if (millis() - lastHealth >= 1000) {
      lastHealth = millis();
      if (mpuHealthy()) {
        healthFail = 0;
      } else if (++healthFail >= 2) {
        mpuOK = false; vibEvent = 1; lastRetry = millis();
        mpuDropCount++; mpuDropAfterTxMs = lastTxMs ? (uint32_t)(millis() - lastTxMs) : 0;
        continue;
      }
    }

    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);

    bool same = (a.acceleration.x == l0 && a.acceleration.y == l1 && a.acceleration.z == l2);
    l0 = a.acceleration.x; l1 = a.acceleration.y; l2 = a.acceleration.z;
    frozenCount = same ? frozenCount + 1 : 0;
    if (frozenCount >= FROZEN_LIMIT) {
      mpuOK = false; vibEvent = 3; lastRetry = millis();
      mpuDropCount++; mpuDropAfterTxMs = lastTxMs ? (uint32_t)(millis() - lastTxMs) : 0;
      frozenCount = 0;
      continue;
    }

    processVibrationSample(a);

    fsCount++;
    uint32_t nowMs = millis();
    if (nowMs - fsT0 >= 1000) {
      vibActualFs = fsCount * 1000.0f / (nowMs - fsT0);
      fsCount = 0;
      fsT0 = nowMs;
    }
  }
}

// ==============================================================================
// WATCHDOG & LOCAL OTA
// ==============================================================================
void initWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
    .idle_core_mask = (1 << 0),
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
  esp_task_wdt_add(NULL);
#endif
  Serial.println("[WDT] Hardware Watchdog Timer aktif (" + String(WDT_TIMEOUT_SECONDS) + " detik)");
}

void initArduinoOTA() {
  ArduinoOTA.setHostname(otaHostname.c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
}

void initWebOTA() {
  if (webServerStarted) return;
  webServer.on("/", HTTP_GET, []() {
    webServer.send(200, "text/plain", "Node ESP32 #" + String(NODE_ID) + " Online. Gunakan Web Console Pusat.");
  });
  webServer.begin();
  if (MDNS.begin(otaHostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }
  webServerStarted = true;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  Serial.print("[WIFI] Menghubungkan ke \"" + String(WIFI_SSID) + "\" ");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#if WIFI_LOW_TX_POWER
  WiFi.setTxPower(WIFI_POWER_11dBm);
#endif
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 25) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
    attempt++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("[WIFI] Terhubung! IP Node : " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Belum terhubung. Pengiriman LoRa tetap berjalan...");
  }
}

// ==============================================================================
// TASK TELEMETRI BACKEND (CORE 0)
// ==============================================================================
static void pushHttpResult(int code, const String& msg) {
  HttpResult r;
  r.code = code;
  strncpy(r.msg, msg.c_str(), HTTP_MSG_MAX - 1);
  r.msg[HTTP_MSG_MAX - 1] = '\0';
  xQueueSend(httpResultQueue, &r, 0);
}

void httpTask(void* param) {
  HttpPayload item;
  for (;;) {
    if (xQueueReceive(httpPayloadQueue, &item, portMAX_DELAY) != pdTRUE) continue;
    if (WiFi.status() != WL_CONNECTED) {
      pushHttpResult(-100, "Lewati backend: WiFi belum terhubung.");
      continue;
    }
    if (ESP.getMaxAllocHeap() < HTTP_MIN_HEAP_BYTES) {
      pushHttpResult(-102, "Heap terlalu kecil untuk TLS.");
      continue;
    }

    WiFiClientSecure client;
    if (HTTPS_INSECURE) client.setInsecure();
    else client.setCACert(ROOT_CA_PEM);
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);

    if (!http.begin(client, BACKEND_URL)) {
      pushHttpResult(-101, "http.begin() gagal.");
      continue;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", BACKEND_API_KEY);

    int httpCode = http.POST((uint8_t*)item.body, strlen(item.body));
    if (httpCode == 200 || httpCode == 201) {
      pushHttpResult(httpCode, "Data terkirim ke backend ingest (kode " + String(httpCode) + ")");
    } else {
      pushHttpResult(httpCode, "Gagal kirim backend ingest: " + String(httpCode));
    }
    http.end();
    client.stop();
  }
}

void printHttpResults() {
  if (!httpResultQueue) return;
  HttpResult r;
  while (xQueueReceive(httpResultQueue, &r, 0) == pdTRUE) {
    httpLastCode = r.code;
    bool ok = (r.code == 200 || r.code == 201);
    if (ok) {
      httpOkCount++; httpFailStreak = 0; httpLastOkMs = millis();
    } else {
      httpFailCount++;
    }
    Serial.println("[HTTP-INGEST] " + String(r.msg));
  }
}

void sendDataToBackend(const String& payload) {
  if (!HTTP_DIRECT_ENABLED || !httpPayloadQueue) return;
  if (payload.length() >= HTTP_PAYLOAD_MAX) return;

  HttpPayload item;
  strncpy(item.body, payload.c_str(), HTTP_PAYLOAD_MAX - 1);
  item.body[HTTP_PAYLOAD_MAX - 1] = '\0';
  xQueueSend(httpPayloadQueue, &item, 0);
}

// ==============================================================================
// SINKRONISASI REMOTE CONSOLE & OTA MANAGER (server.js)
// ==============================================================================
// Helper parser respons JSON sederhana dari server.js (tanpa dependensi library)
bool parseSyncResponse(const String& res, unsigned long& nextIntervalMs, uint32_t& cmdId, String& cmdType, String& fwId, String& md5, size_t& fwSize) {
  cmdId = 0; cmdType = ""; fwId = ""; md5 = ""; fwSize = 0;
  if (res.indexOf("\"ok\":true") < 0 && res.indexOf("\"ok\": true") < 0) return false;

  int intIdx = res.indexOf("\"interval_ms\":");
  if (intIdx >= 0) {
    int s = intIdx + 14;
    while (s < res.length() && (res[s] == ' ' || res[s] == ':')) s++;
    int e = s;
    while (e < res.length() && isDigit(res[e])) e++;
    if (e > s) nextIntervalMs = res.substring(s, e).toInt();
  }

  int cmdIdx = res.indexOf("\"cmd\":");
  if (cmdIdx >= 0) {
    String cmdPart = res.substring(cmdIdx);
    if (cmdPart.startsWith("\"cmd\":null") || cmdPart.startsWith("\"cmd\": null")) return true;

    int idIdx = cmdPart.indexOf("\"id\":");
    if (idIdx >= 0) {
      int s = idIdx + 5;
      while (s < cmdPart.length() && (cmdPart[s] == ' ' || cmdPart[s] == ':')) s++;
      int e = s;
      while (e < cmdPart.length() && isDigit(cmdPart[e])) e++;
      if (e > s) cmdId = cmdPart.substring(s, e).toInt();
    }

    int typeIdx = cmdPart.indexOf("\"type\":");
    if (typeIdx >= 0) {
      int s = cmdPart.indexOf('"', typeIdx + 7);
      if (s >= 0) {
        int e = cmdPart.indexOf('"', s + 1);
        if (e > s) cmdType = cmdPart.substring(s + 1, e);
      }
    }

    if (cmdType == "ota") {
      int fwIdx = cmdPart.indexOf("\"fw_id\":");
      if (fwIdx >= 0) {
        int s = cmdPart.indexOf('"', fwIdx + 8);
        if (s >= 0) {
          int e = cmdPart.indexOf('"', s + 1);
          if (e > s) fwId = cmdPart.substring(s + 1, e);
        }
      }
      int md5Idx = cmdPart.indexOf("\"md5\":");
      if (md5Idx >= 0) {
        int s = cmdPart.indexOf('"', md5Idx + 6);
        if (s >= 0) {
          int e = cmdPart.indexOf('"', s + 1);
          if (e > s) md5 = cmdPart.substring(s + 1, e);
        }
      }
      int sizeIdx = cmdPart.indexOf("\"size\":");
      if (sizeIdx >= 0) {
        int s = sizeIdx + 7;
        while (s < cmdPart.length() && (cmdPart[s] == ' ' || cmdPart[s] == ':')) s++;
        int e = s;
        while (e < cmdPart.length() && isDigit(cmdPart[e])) e++;
        if (e > s) fwSize = cmdPart.substring(s, e).toInt();
      }
    }
  }
  return true;
}

// Lapor status eksekusi perintah ke server.js
void reportRemoteCmdResult(uint32_t cmdId, const String& status, const String& message) {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(REMOTE_SERVER_URL) + "/device/result";
  HTTPClient http;
  WiFiClient client;
  WiFiClientSecure secureClient;
  bool isHttps = url.startsWith("https://");

  if (isHttps) {
    secureClient.setInsecure();
    http.begin(secureClient, url);
  } else {
    http.begin(client, url);
  }

  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", REMOTE_API_KEY);

  String body = "{\"node_id\":" + String(NODE_ID) +
                ",\"cmd_id\":" + String(cmdId) +
                ",\"status\":\"" + status + "\"" +
                ",\"message\":\"" + escapeJsonString(message.c_str()) + "\"}";

  http.POST(body);
  http.end();
}

// Unduh dan Flash Firmware OTA dari server.js
void performRemoteOTA(uint32_t cmdId, const String& fwId, const String& expectedMd5, size_t expectedSize) {
  if (WiFi.status() != WL_CONNECTED) {
    reportRemoteCmdResult(cmdId, "error", "WiFi tidak tersambung saat OTA");
    return;
  }

  Serial.println("[REMOTE-OTA] Memulai unduh firmware ID: " + fwId);
  reportRemoteCmdResult(cmdId, "progress", "Mempersiapkan unduhan firmware...");

  String url = String(REMOTE_SERVER_URL) + "/device/firmware/" + fwId;
  HTTPClient http;
  http.setTimeout(30000);

  WiFiClient client;
  WiFiClientSecure secureClient;
  bool isHttps = url.startsWith("https://");

  bool beginOk = false;
  if (isHttps) {
    secureClient.setInsecure();
    beginOk = http.begin(secureClient, url);
  } else {
    beginOk = http.begin(client, url);
  }

  if (!beginOk) {
    reportRemoteCmdResult(cmdId, "error", "http.begin gagal untuk URL OTA");
    return;
  }

  http.addHeader("X-API-Key", REMOTE_API_KEY);
  int code = http.GET();
  if (code != 200) {
    reportRemoteCmdResult(cmdId, "error", "Gagal mengunduh firmware: HTTP " + String(code));
    http.end();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0 && expectedSize > 0) contentLength = expectedSize;
  if (contentLength <= 0) {
    reportRemoteCmdResult(cmdId, "error", "Ukuran file firmware tidak valid");
    http.end();
    return;
  }

  Serial.printf("[REMOTE-OTA] Ukuran firmware: %d byte. Memulai flashing...\n", contentLength);
  reportRemoteCmdResult(cmdId, "progress", "Menulis ke partisi flash (" + String(contentLength / 1024) + " KB)...");

  if (!Update.begin(contentLength)) {
    reportRemoteCmdResult(cmdId, "error", "Update.begin gagal (ruang flash kurang): " + String(Update.errorString()));
    http.end();
    return;
  }

  if (expectedMd5.length() > 0) {
    Update.setMD5(expectedMd5.c_str());
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buff[1024];
  size_t totalRead = 0;
  int lastProgress = 0;

  while (http.connected() && (totalRead < (size_t)contentLength)) {
    esp_task_wdt_reset();
    size_t avail = stream->available();
    if (avail) {
      int c = stream->readBytes(buff, ((avail > sizeof(buff)) ? sizeof(buff) : avail));
      if (c > 0) {
        Update.write(buff, c);
        totalRead += c;
        int prog = (totalRead * 100) / contentLength;
        if (prog - lastProgress >= 20) {
          lastProgress = prog;
          Serial.printf("[REMOTE-OTA] Flashing: %d%%\n", prog);
          reportRemoteCmdResult(cmdId, "progress", "Flashing: " + String(prog) + "%");
        }
      }
    } else {
      delay(5);
    }
  }

  if (totalRead != (size_t)contentLength) {
    reportRemoteCmdResult(cmdId, "error", "Koneksi terputus saat unduh: " + String(totalRead) + "/" + String(contentLength) + " byte");
    Update.abort();
    http.end();
    return;
  }

  if (!Update.end()) {
    reportRemoteCmdResult(cmdId, "error", "Verifikasi OTA gagal: " + String(Update.errorString()));
    http.end();
    return;
  }

  Serial.println("[REMOTE-OTA] Flashing Sukses! Rebooting...");
  reportRemoteCmdResult(cmdId, "ok", "OTA Sukses! Node me-restart sekarang...");
  http.end();
  delay(1000);
  ESP.restart();
}

// Sinkronisasi berkala ke server.js: kirim log serial & status, terima perintah
void syncWithRemoteConsole() {
  if (WiFi.status() != WL_CONNECTED) return;

  String payload = "{";
  payload += "\"node_id\":" + String(NODE_ID) + ",";
  payload += "\"boot\":\"" + String(espBootId) + "\",";
  payload += "\"fw\":\"" + String(FIRMWARE_NAME) + "\",";
  payload += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"uptime_s\":" + String((unsigned long)(millis() / 1000UL)) + ",";
  payload += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  payload += "\"mpu\":" + String(vibIsValid() ? "true" : "false") + ",";
  payload += "\"rtd\":" + String(latestTempValid ? "true" : "false") + ",";
  payload += "\"logs\":[";

  int startIdx = (logHead - logCount + MAX_LOG_LINES) % MAX_LOG_LINES;
  bool first = true;
  uint32_t maxIdInBatch = lastRemoteSyncedLogId;
  int countAdded = 0;

  for (int i = 0; i < logCount && countAdded < 40; i++) {
    int idx = (startIdx + i) % MAX_LOG_LINES;
    if (logHistory[idx].id > lastRemoteSyncedLogId) {
      if (!first) payload += ",";
      first = false;
      payload += "{\"id\":";
      payload += logHistory[idx].id;
      payload += ",\"ms\":";
      payload += logHistory[idx].ms;
      payload += ",\"msg\":\"";
      payload += escapeJsonString(logHistory[idx].text);
      payload += "\"}";
      if (logHistory[idx].id > maxIdInBatch) maxIdInBatch = logHistory[idx].id;
      countAdded++;
    }
  }
  payload += "]}";

  String url = String(REMOTE_SERVER_URL) + "/device/sync";
  HTTPClient http;
  WiFiClient client;
  WiFiClientSecure secureClient;
  bool isHttps = url.startsWith("https://");

  if (isHttps) {
    secureClient.setInsecure();
    http.begin(secureClient, url);
  } else {
    http.begin(client, url);
  }

  http.setTimeout(6000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", REMOTE_API_KEY);

  int code = http.POST(payload);
  if (code == 200) {
    lastRemoteSyncedLogId = maxIdInBatch;
    String res = http.getString();

    unsigned long nextIntv = 0;
    uint32_t cmdId = 0;
    String cmdType = "", fwId = "", md5 = "";
    size_t fwSize = 0;

    if (parseSyncResponse(res, nextIntv, cmdId, cmdType, fwId, md5, fwSize)) {
      if (nextIntv >= 2000 && nextIntv <= 120000) {
        remoteSyncIntervalMs = nextIntv;
      }

      if (cmdId > 0 && cmdType.length() > 0) {
        if (cmdType == "restart") {
          Serial.printf("[REMOTE] Perintah restart (#%u) diterima dari server.\n", cmdId);
          reportRemoteCmdResult(cmdId, "ok", "Node sedang me-restart...");
          delay(800);
          ESP.restart();
        } else if (cmdType == "send_now") {
          Serial.printf("[REMOTE] Perintah send_now (#%u) diterima dari server.\n", cmdId);
          reportRemoteCmdResult(cmdId, "ok", "Pengiriman data segera dipicu.");
          sendNodeData(true, true);
        } else if (cmdType == "ota") {
          Serial.printf("[REMOTE] Perintah OTA (#%u) diterima: fw_id=%s, size=%u byte.\n", cmdId, fwId.c_str(), fwSize);
          performRemoteOTA(cmdId, fwId, md5, fwSize);
        }
      }
    }
  }
  http.end();
}

// ==============================================================================
// LORA & SUPERVISOR SENSOR
// ==============================================================================
bool initLoRa() {
  LoRa.setPins(PIN_LORA_NSS, PIN_LORA_RST, PIN_LORA_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY)) return false;
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x34);
  LoRa.setTxPower(17);
  return true;
}

void superviseSensors(unsigned long now) {
  static bool warned = false;
  bool mpuGood = vibIsValid();
  bool rtdGood = latestTempValid;

  if (mpuGood && rtdGood) {
    sensorBadSince = 0;
    warned = false;
    if (!sensorGoodSince) sensorGoodSince = now;
    if (sensorRestartCount > 0 && (now - sensorGoodSince) >= SENSOR_GOOD_RESET_MS) {
      Serial.println("[SUPERVISOR] Kedua sensor sehat > 10 menit, hitungan restart di-nol-kan");
      sensorRestartCount = 0;
    }
    return;
  }

  sensorGoodSince = 0;
  if (!sensorBadSince) sensorBadSince = now;
  if ((now - sensorBadSince) < SENSOR_RESTART_AFTER_MS) return;

  if (sensorRestartCount >= SENSOR_MAX_RESTARTS) {
    if (!warned) {
      warned = true;
      Serial.println("[SUPERVISOR] Sensor bermasalah, batas restart tercapai. Node tetap aktif.");
    }
    return;
  }

  sensorRestartCount++;
  Serial.println(String("[SUPERVISOR] Sensor tidak valid > ") + String((unsigned long)(SENSOR_RESTART_AFTER_MS / 60000UL)) +
                 " menit. Restart otomatis " + String((uint32_t)sensorRestartCount) + "/" + String(SENSOR_MAX_RESTARTS));
  delay(500);
  ESP.restart();
}

void printMpuDrop(uint8_t ev) {
  String m = "[MPU6050] ERROR ";
  m += (ev == 3) ? "data beku (nilai tidak berubah)" : "sensor tidak merespons";
  m += ", putus ke-" + String((uint32_t)mpuDropCount);
  if (mpuDropAfterTxMs) m += ", " + String((uint32_t)mpuDropAfterTxMs) + " ms setelah TX terakhir";
  m += ". Reset I2C & init ulang tiap 5 detik...";
  Serial.println(m);
}

void printResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  const char* txt = "lainnya";
  switch (r) {
    case ESP_RST_POWERON:  txt = "power-on"; break;
    case ESP_RST_SW:       txt = "restart software"; break;
    case ESP_RST_PANIC:    txt = "PANIC / crash"; break;
    case ESP_RST_INT_WDT:  txt = "watchdog interrupt"; break;
    case ESP_RST_TASK_WDT: txt = "watchdog task"; break;
    case ESP_RST_BROWNOUT: txt = "BROWNOUT (daya drop)"; break;
    default: break;
  }
  Serial.println("[BOOT] Alasan reset: " + String(txt) + " (kode " + String((int)r) + ")");
}

// ==============================================================================
// SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  delay(1000);

  espBootId = (uint32_t)esp_random();
  if (espBootId == 0) espBootId = (uint32_t)millis();

  Serial.println();
  Serial.println("==================================================");
  Serial.println("   NODE TRANSMITTER - MOTOR MONITORING BEKAERT   ");
  Serial.println("   Node ID : " + String(NODE_ID) + " | Boot ID: " + String(espBootId));
  Serial.println("==================================================");

  if (rtcMagic != RTC_MAGIC_VALUE) { rtcMagic = RTC_MAGIC_VALUE; sensorRestartCount = 0; }
  printResetReason();
  initWatchdog();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    initArduinoOTA();
    initWebOTA();
  }

  // Antrian task HTTPS Ingest
  if (HTTP_DIRECT_ENABLED) {
    httpPayloadQueue = xQueueCreate(2, sizeof(HttpPayload));
    httpResultQueue  = xQueueCreate(4, sizeof(HttpResult));
    if (httpPayloadQueue && httpResultQueue) {
      xTaskCreatePinnedToCore(httpTask, "httpTask", 12288, NULL, 1, NULL, 0);
      Serial.println("[HTTP-INGEST] Task pengiriman aktif -> " + String(BACKEND_URL));
    }
  }

  // Init MPU6050 & task 1 kHz di Core 1
#if PIN_MPU_PWR >= 0
  pinMode(PIN_MPU_PWR, OUTPUT);
  digitalWrite(PIN_MPU_PWR, HIGH);
  delay(150);
#endif
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setTimeOut(20);
  if (mpu.begin(MPU_ADDR)) {
    configureMPU();
    mpuOK = true;
    Serial.println("[MPU6050] OK (+-4G, sampling " + String(SAMPLE_RATE_HZ) + " Hz)");
  } else {
    mpuOK = false;
    Serial.println("[MPU6050] GAGAL mendeteksi sensor. Coba ulang otomatis...");
  }
  xTaskCreatePinnedToCore(vibTask, "vibTask", 8192, NULL, 2, NULL, 1);

  // Init SPI & MAX31865 (PT100)
  SPI.begin(18, 19, 23);
  rtd.begin(MAX31865_3WIRE);
  rtd.clearFault();
  readPT100();
  lastPt100Poll = millis();

  // Init LoRa
  int retry = 0;
  while (!(loraOK = initLoRa()) && retry < 3) {
    retry++;
    delay(1000);
    esp_task_wdt_reset();
  }
  if (loraOK) Serial.println("[LORA] OK - 923MHz | SF7 | BW125kHz | CR4/5");
  else Serial.println("[LORA] GAGAL radio SX1276.");

#if REMOTE_CONSOLE_ENABLED
  Serial.println("[REMOTE] Console Sync AKTIF -> " + String(REMOTE_SERVER_URL));
#endif

  Serial.println("==================================================\n");
  setupDoneMs = millis();
}

// ==============================================================================
// LOOP UTAMA
// ==============================================================================
void loop() {
  esp_task_wdt_reset();
  ArduinoOTA.handle();
  webServer.handleClient();
  printHttpResults();

  unsigned long now = millis();

  if (vibEvent) {
    if (vibEvent == 1 || vibEvent == 3) printMpuDrop(vibEvent);
    if (vibEvent == 2) Serial.println("[MPU6050] Sensor pulih, sampling dilanjutkan (OK)");
    vibEvent = 0;
  }

  // Cek koneksi WiFi berkala
  if (now - lastWifiCheck >= 15000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      if (!wifiDownSince) { wifiDownSince = now; Serial.println("[WIFI] Koneksi putus, reconnecting..."); }
      if (now - wifiDownSince > 60000) {
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        wifiDownSince = now;
      } else {
        WiFi.reconnect();
      }
    } else {
      if (wifiDownSince) {
        wifiDownSince = 0;
        Serial.println("[WIFI] Tersambung kembali, IP: " + WiFi.localIP().toString());
      }
    }
  }

  // Pembacaan PT100 & Supervisor
  if (now - lastPt100Poll >= PT100_POLL_MS) {
    lastPt100Poll = now;
    readPT100();
  }
  superviseSensors(now);

  // Kirim data tertunda (menghindari tumpukan arus LoRa & WiFi)
  if (pendingWebAt && (long)(now - pendingWebAt) >= 0) {
    pendingWebAt = 0;
    lastTxMs = millis();
    txQuarantineUntil = lastTxMs + TX_QUARANTINE_MS;
    sendDataToBackend(pendingWebPayload);
    pendingWebPayload = "";
  }

  // Siklus transmisi telemetri (LoRa + Ingest)
  if (!firstSendDone) {
    if (vibWindowsDone >= FIRST_SEND_MIN_WINDOWS || (now - setupDoneMs) >= FIRST_SEND_MAX_WAIT_MS) {
      firstSendDone = true;
      lastLoraTime = now;
      lastWebTime = now;
      sendNodeData(true, true);
    }
  } else {
    bool loraDue = (now - lastLoraTime) >= LORA_INTERVAL_MS;
    bool webDue  = (now - lastWebTime)  >= WEB_INTERVAL_MS;
    if (loraDue || webDue) {
      if (loraDue) lastLoraTime = now;
      if (webDue)  lastWebTime = now;
      sendNodeData(loraDue, webDue);
    }
  }

  // Sinkronisasi dengan Remote Console & OTA Manager (server.js)
#if REMOTE_CONSOLE_ENABLED
  if (WiFi.status() == WL_CONNECTED) {
    if (now - lastRemoteSyncTime >= remoteSyncIntervalMs) {
      lastRemoteSyncTime = now;
      syncWithRemoteConsole();
    }
  }
#endif

  delay(2);
}

// ==============================================================================
// PEMBACAAN SENSOR & TRANSMISI TELEMETRI
// ==============================================================================
void readPT100() {
  rtd.setWires(MAX31865_3WIRE);
  float t_raw = 0;
  uint8_t fault = 0;
  bool inRange = false;
  for (int attempt = 0; attempt < PT100_READ_TRIES; attempt++) {
    t_raw = rtd.temperature(RNOMINAL, RREF);
    fault = rtd.readFault();
    inRange = (t_raw > -50.0f && t_raw < 150.0f);
    if (fault == 0 && inRange) break;
    if (fault) rtd.clearFault();
    delay(50);
  }

  bool ok = (fault == 0 && inRange);
  if (ok) {
    latestTemperature = t_raw;
    latestTempValid = true;
  } else {
#if ALLOW_SIMULATION
    latestTemperature = 43.5f + 1.5f * sin(millis() / 1000.0f * 0.15f);
    latestTempValid = true;
    ok = true;
#else
    latestTempValid = false;
#endif
  }

  static int lastState = -1;
  int st = ok ? 1 : 0;
  if (st != lastState) {
    lastState = st;
    if (ok) Serial.println("[MAX31865] OK suhu valid: " + String(latestTemperature, 1) + "C");
    else Serial.printf("[MAX31865] ERROR suhu tidak valid (fault 0x%02X)\n", fault);
  }
}

void readSensors() {
  if (vibIsValid()) {
    latestVibrationRMS = getVibCalibrated();
    latestVibValid = true;
  } else {
#if ALLOW_SIMULATION
    latestVibrationRMS = 0.95f + 0.20f * sin(millis() / 1000.0f * 0.4f);
    latestVibValid = true;
#else
    latestVibrationRMS = 0.0f;
    latestVibValid = false;
#endif
  }

  latestBattery = readBatteryVoltage();

  bool crit = (latestVibValid && latestVibrationRMS >= THRESHOLD_FAULT) ||
              (latestTempValid && latestTemperature >= 80.0f);
  bool warn = (latestVibValid && latestVibrationRMS >= THRESHOLD_WARNING) ||
              (latestTempValid && latestTemperature >= 60.0f);
  if (crit)                                     latestStatus = "CRITICAL";
  else if (warn)                                latestStatus = "WARNING";
  else if (!latestVibValid || !latestTempValid) latestStatus = "SENSOR_FAULT";
  else                                          latestStatus = "NORMAL";
}

String buildPayload() {
  String payload = "{";
  payload += "\"node_id\":" + String(NODE_ID) + ",";
  payload += "\"temp\":" + String(latestTemperature, 2) + ",";
  payload += "\"vib_rms\":" + String(latestVibrationRMS, 3) + ",";
  payload += "\"battery\":" + String(latestBattery, 2) + ",";
  payload += "\"status\":\"" + latestStatus + "\"";
#if SEND_HEALTH_FLAGS
  payload += ",\"mpu_ok\":" + String(latestVibValid ? 1 : 0);
  payload += ",\"rtd_ok\":" + String(latestTempValid ? 1 : 0);
#endif
  payload += "}";
  return payload;
}

void sendLoRaPacket(const String& payload) {
  if (!loraOK) loraOK = initLoRa();
  if (loraOK) {
    if (!LoRa.beginPacket()) {
      if (++loraFail >= 3) {
        Serial.println("[LORA] Radio macet, re-init...");
        loraOK = initLoRa();
        loraFail = 0;
      }
    } else {
      digitalWrite(LED_PIN, HIGH);
      LoRa.print(payload);
      LoRa.endPacket(true);
      digitalWrite(LED_PIN, LOW);
      loraFail = 0;
      Serial.println("[LORA-TX] Paket terkirim: " + payload);
    }
  }
}

void sendNodeData(bool doLora, bool doWeb) {
  readSensors();
  String payload = buildPayload();

  Serial.println("[SENSORS] Node " + String(NODE_ID) + " -> Suhu: " + String(latestTemperature, 1) +
                 "C | Vib RMS: " + String(latestVibrationRMS, 2) + " mm/s | Batt: " +
                 String(latestBattery, 2) + "V | Status: " + latestStatus);

#if TX_ENABLED
  if (doLora) {
    lastTxMs = millis();
    txQuarantineUntil = lastTxMs + TX_QUARANTINE_MS;
    sendLoRaPacket(payload);
  }
  if (doWeb) {
    if (doLora && TX_STAGGER_MS > 0) {
      pendingWebPayload = payload;
      pendingWebAt = millis() + TX_STAGGER_MS;
      if (pendingWebAt == 0) pendingWebAt = 1;
    } else {
      lastTxMs = millis();
      txQuarantineUntil = lastTxMs + TX_QUARANTINE_MS;
      sendDataToBackend(payload);
    }
  }
#endif
}
