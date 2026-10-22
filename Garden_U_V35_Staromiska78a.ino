
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>

// =====================================================
// V28 MULTI XIAOMI x4 + ROOM NAMES + FAST BLE + TIME SYNC + SETPOINT LCD: Xiaomi MJWSD05MMC (pvvx/BTE v5.9, BTHome v2) — основний кімнатний датчик
// Діагностика: друк сирого BTHome payload при кожній зміні advertising-пакета для визначення обох фізичних кнопок
// Автосинхронізація дати/часу Xiaomi з ESP32 через pvvx CMD 0x23
// Час ESP32: NTP + Europe/Kyiv (EET/EEST), синхронізація після старту та кожні 24 години
// Додатковий DS18B20 можна вмикати/вимикати з меню Налаштування
// Безперервне passive BLE scanning (window=interval) + діагностика інтервалу температурних пакетів
//      Фізичні кнопки термометра керують уставкою опалення: верхня +0.5 C, нижня -0.5 C
// DS18B20 = резерв; MQTT = другий резерв; BLE діагностика в /api/status
// ЦІЛЬОВА ПЛАТА
// =====================================================
// Прошивка адаптована під ESP32-S3 Dev Board N16R8:
// 16 MB Flash + 8 MB PSRAM.
// Arduino IDE: ESP32S3 Dev Module / ESP32-S3 DevKitC-1.
// PSRAM: OPI PSRAM, якщо така опція доступна.


// =====================================================
// ОСНОВНІ НАЛАШТУВАННЯ
// =====================================================

// Wi-Fi за замовчуванням.
// Після першого запуску SSID і пароль можна змінювати через
// веб-меню "Налаштування Wi-Fi" без перепрошивки.
const char* DEFAULT_WIFI_SSID = "WAVLINK";
const char* DEFAULT_WIFI_PASSWORD = "28041975";

String wifiSSID;
String wifiPassword;

// Аварійна точка доступу, якщо ESP32 не може підключитися до роутера.
const char* WIFI_AP_SSID = "ESP32-S3-Garden-Setup";
const char* WIFI_AP_PASSWORD = "garden1234";

bool wifiAPMode = false;

// Ім'я пристрою в локальній мережі
const char* DEVICE_NAME = "esp32-s3-garden";

// -----------------------------------------------------
// MQTT / HOME ASSISTANT
// -----------------------------------------------------
// Якщо MQTT не потрібен — залиште MQTT_HOST порожнім: ""
// Для Home Assistant зазвичай вкажіть IP вашого MQTT broker
// (наприклад Mosquitto add-on у Home Assistant).

// Віддалений доступ через вихідне MQTT-з'єднання.
// Нічого на роутері відкривати не потрібно.
// Налаштовується з веб-меню: Налаштування -> Віддалений доступ.
bool remoteAccessEnabled = false;
bool remoteTlsEnabled = true;

String remoteMqttHost = "";
uint16_t remoteMqttPort = 8883;
String remoteMqttUser = "";
String remoteMqttPassword = "";
String remoteMqttBase = "garden/controller";

// Home Assistant MQTT Discovery
const bool HA_DISCOVERY_ENABLED = true;

// -----------------------------------------------------
// ПЕРІОДИ
// -----------------------------------------------------

const unsigned long WIFI_RECONNECT_INTERVAL = 10000;
const unsigned long MQTT_RECONNECT_INTERVAL = 10000;
const unsigned long MQTT_STATE_INTERVAL = 15000;

// =====================================================
// MICROSD / ЖУРНАЛ
// =====================================================

// На показаній ESP32-S3 N16R8 вбудованого microSD-слота немає.
// Для журналу використовується зовнішній SPI microSD-модуль.

// microSD / E-mail вимкнені у Lite-збірці для економії Flash.
const bool SD_ENABLED = false;
bool sdReady = false;
const char* smtpRecipient = "";

// =====================================================
// GPIO — ESP32-S3 N16R8
// =====================================================
//
// Вибрані GPIO не зачіпають native USB GPIO19/20
// та strapping-піни GPIO0/3/45/46.

// 4 клапани поливу
const uint8_t WATER_PINS[4] = {4, 5, 6, 7};

// Датчик дощу
const uint8_t RAIN_PIN = 8;

// 3 канали освітлення
const uint8_t LIGHT_PINS[3] = {9, 10, 11};
const uint8_t LIGHT_COUNT = 3;

// Реле котла
const uint8_t BOILER_PIN = 12;

// DS18B20
const uint8_t TEMP_PIN = 14;

// Зовнішній microSD-модуль по SPI
const uint8_t SD_SCK_PIN  = 39;
const uint8_t SD_MISO_PIN = 40;
const uint8_t SD_MOSI_PIN = 41;
const uint8_t SD_CS_PIN   = 42;

// =====================================================
// ЛОГІКА РЕЛЕ / ДАТЧИКА
// =====================================================

// Більшість relay board активні LOW.
// Якщо у вас Active HIGH — поміняйте місцями LOW/HIGH.
const uint8_t RELAY_ON  = LOW;
const uint8_t RELAY_OFF = HIGH;

// Для герконового датчика / FC-37 часто LOW = дощ
const uint8_t RAIN_ACTIVE_LEVEL = LOW;

// =====================================================
// ОБ'ЄКТИ
// =====================================================

WebServer server(80);
Preferences prefs;

WiFiClient mqttPlainClient;
WiFiClientSecure mqttSecureClient;
PubSubClient mqttClient(mqttPlainClient);

unsigned long lastWiFiReconnectAttempt = 0;
unsigned long lastMQTTReconnectAttempt = 0;
unsigned long lastMQTTStatePublish = 0;

OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensors(&oneWire);
bool extraTempSensorEnabled = false; // DS18B20 вимкнений за замовчуванням

// =====================================================
// XIAOMI MJWSD05MMC — BLE / BTHome v2
// =====================================================
const char* XIAOMI_BLE_MAC = "A4:C1:38:F9:68:BF";
const char* XIAOMI_NAME_HINT = "BTE_F968BF";
const unsigned long XIAOMI_TEMP_TIMEOUT = 180000UL; // 3 хв

float xiaomiTemperature = -127.0;
float xiaomiHumidity = -1.0;
uint8_t xiaomiBattery = 0;
int xiaomiRSSI = -127;
unsigned long lastXiaomiPacketMillis = 0;
unsigned long lastXiaomiTemperaturePacketMillis = 0;
unsigned long xiaomiTemperaturePacketIntervalMs = 0;
bool xiaomiSeen = false;
uint8_t xiaomiBleAddrType = 0; // NimBLE 2.x: 0=public, 1=random; оновлюється зі scan result

// V28: до 4 Xiaomi BLE датчиків. Слот 1 — існуючий датчик, слоти 2..4 налаштовуються у меню Опалення.
const uint8_t XIAOMI_SENSOR_COUNT = 4;
String xiaomiMacs[XIAOMI_SENSOR_COUNT] = {"A4:C1:38:F9:68:BF", "", "", ""};
String xiaomiRoomNames[XIAOMI_SENSOR_COUNT] = {"Перший поверх", "Другий поверх", "Кімната 3", "Кімната 4"};
bool xiaomiSensorEnabled[XIAOMI_SENSOR_COUNT] = {true, false, false, false};
float xiaomiTemperatures[XIAOMI_SENSOR_COUNT] = {-127,-127,-127,-127};
float xiaomiHumidities[XIAOMI_SENSOR_COUNT] = {-1,-1,-1,-1};
uint8_t xiaomiBatteries[XIAOMI_SENSOR_COUNT] = {0,0,0,0};
int xiaomiRSSIs[XIAOMI_SENSOR_COUNT] = {-127,-127,-127,-127};
unsigned long xiaomiLastPacket[XIAOMI_SENSOR_COUNT] = {0,0,0,0};
bool xiaomiSensorSeen[XIAOMI_SENSOR_COUNT] = {false,false,false,false};
// 0..3 = конкретний датчик, 4 = середнє всіх доступних
uint8_t heatingXiaomiSource = 0;

// V34.1 VERIFIED PROTOCOL: 0x10 (power) і 0x11 (opening) — різні BTHome objects.
bool xiaomiPowerKnown = false;
bool xiaomiPowerState = false;
bool xiaomiOpeningKnown = false;
bool xiaomiOpeningState = false;
uint8_t xiaomiButtonEvent = 0;

// V34 + custom pvvx: upper physical button is exported as BTHome 0x09 count8.
// Every counter increment = one real TOP-button press. This works repeatedly
// even while the Xiaomi is already in fast advertising mode.
bool xiaomiUpCounterKnown = false;
uint8_t xiaomiUpCounter = 0;
volatile uint8_t xiaomiUpperPressPendingCount = 0;

// Події фізичних кнопок Xiaomi обробляємо поза BLE callback.
// У pvvx MJWSD05MMC нижня кнопка передається як BTHome Switch,
// а події Button (якщо їх віддає поточна збірка pvvx) використовуємо для верхньої кнопки.
volatile bool xiaomiUpperButtonPending = false;
volatile bool xiaomiLowerButtonPending = false;
bool xiaomiDownCounterKnown = false;
uint8_t xiaomiDownCounter = 0;
volatile uint8_t xiaomiDownPressPendingCount = 0;
volatile uint8_t xiaomiPendingButtonEvent = 0;
uint8_t xiaomiPacketId = 0;
bool xiaomiPacketIdKnown = false;
uint8_t xiaomiLastHandledButtonPacketId = 0;
bool xiaomiLastHandledButtonPacketKnown = false;
unsigned long lastXiaomiThermostatActionMillis = 0;
const unsigned long XIAOMI_BUTTON_DEBOUNCE = 350UL;

// V30: експериментальне визначення верхньої кнопки через fast advertising.
// pvvx MJWSD05MMC: коротке натискання верхньої кнопки вмикає швидкий advertising приблизно на 80 с.
unsigned long xiaomiLastLowerButtonMillis = 0;
unsigned long xiaomiLastTopHeuristicMillis = 0;
unsigned long xiaomiPrevTemperatureIntervalMs = 0;
volatile bool xiaomiTopHeuristicPending = false;
const unsigned long XIAOMI_TOP_FAST_MAX_MS = 9000UL;
const unsigned long XIAOMI_TOP_SLOW_MIN_MS = 18000UL;
const unsigned long XIAOMI_TOP_LOCKOUT_MS = 90000UL;
const unsigned long XIAOMI_TOP_AFTER_LOWER_GUARD_MS = 6000UL;
// V33: верхню кнопку підтверджуємо ДВОМА послідовними fast-інтервалами.
// Це прибирає випадковий +0.5 через один короткий BLE/temperature interval.
bool xiaomiTopCandidate = false;
uint8_t xiaomiTopFastConfirmCount = 0;
unsigned long xiaomiTopCandidateMillis = 0;
const unsigned long XIAOMI_TOP_CONFIRM_WINDOW_MS = 15000UL;

NimBLEScan* xiaomiBleScan = nullptr;
static const NimBLEUUID BTHOME_UUID((uint16_t)0xFCD2);
static const NimBLEUUID PVVX_SERVICE_UUID((uint16_t)0x1F10);
static const NimBLEUUID PVVX_COMMAND_UUID((uint16_t)0x1F1F);

// V26: автоматична синхронізація годинника MJWSD05MMC збережена з V25.
// pvvx CMD 0x23 приймає 32-бітний local_seconds у little-endian.
bool xiaomiTimeSynced = false;
unsigned long lastXiaomiTimeSyncMillis = 0;
unsigned long lastXiaomiTimeSyncAttemptMillis = 0;
const unsigned long XIAOMI_TIME_SYNC_INTERVAL = 24UL * 60UL * 60UL * 1000UL;
const unsigned long XIAOMI_TIME_SYNC_RETRY = 5UL * 60UL * 1000UL;

// V34.1 VERIFIED PROTOCOL: верхня кнопка = BTHome 0x09 press counter from custom pvvx -> +0.5 C;
// нижня кнопка = BTHome 0x11 CLOSED edge -> -0.5 C. Fast-advertising heuristic is disabled.
// Збережено V31 FAST SYNC, Xiaomi x4, room names, time sync та debounced LCD setpoint sync.
// V30 EXPERIMENTAL: Xiaomi x4 + room names + unified setpoint sync + safe button handling
// Нижня кнопка: BTHome Switch -> -0.5 C.
// Верхня кнопка MJWSD05MMC у pvvx/BTE 5.9 штатно є Connect і не має окремого BTHome event.
// Тому +0.5 C визначається експериментально за переходом у fast-advertising після довшої паузи.
// Якщо евристика не підтвердиться на конкретному датчику, верхню кнопку треба реалізувати у кастомній прошивці Xiaomi.
// V27: коротке відображення заданої температури на LCD Xiaomi через pvvx CMD 0x22.
volatile bool xiaomiSetpointDisplayPending = false;
float xiaomiSetpointToDisplay = 0.0f;
unsigned long xiaomiSetpointQueuedAt = 0;
const unsigned long XIAOMI_LCD_DEBOUNCE_MS = 80; // збираємо швидкі +/- в одну BLE-синхронізацію
const uint16_t XIAOMI_SETPOINT_LCD_SECONDS = 3;
unsigned long xiaomiLcdLastAttempt = 0;
const unsigned long XIAOMI_LCD_RETRY_MS = 1200;

bool xiaomiTemperatureValid() {
  if (xiaomiTemperature < -50.0 || xiaomiTemperature > 80.0) return false;
  if (!xiaomiSeen) return false;
  return (millis() - lastXiaomiPacketMillis) < XIAOMI_TEMP_TIMEOUT;
}

static uint16_t le16u(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static int16_t le16s(const uint8_t* p) { return (int16_t)le16u(p); }

bool xiaomiSlotValid(uint8_t idx) {
  if (idx >= XIAOMI_SENSOR_COUNT || !xiaomiSensorEnabled[idx]) return false;
  if (!xiaomiSensorSeen[idx]) return false;
  if (xiaomiTemperatures[idx] < -50.0f || xiaomiTemperatures[idx] > 80.0f) return false;
  return (millis() - xiaomiLastPacket[idx]) < XIAOMI_TEMP_TIMEOUT;
}

float xiaomiHeatingTemperature() {
  if (heatingXiaomiSource < XIAOMI_SENSOR_COUNT) {
    return xiaomiSlotValid(heatingXiaomiSource) ? xiaomiTemperatures[heatingXiaomiSource] : -127.0f;
  }
  float sum=0; int count=0;
  for (uint8_t i=0;i<XIAOMI_SENSOR_COUNT;i++) if (xiaomiSlotValid(i)) { sum += xiaomiTemperatures[i]; count++; }
  return count ? sum / count : -127.0f;
}

int findXiaomiSlotByMac(String mac) {
  mac.toUpperCase();
  for (uint8_t i=0;i<XIAOMI_SENSOR_COUNT;i++) {
    if (!xiaomiSensorEnabled[i] || xiaomiMacs[i].length()<11) continue;
    String m=xiaomiMacs[i]; m.toUpperCase();
    if (mac==m) return i;
  }
  return -1;
}

void parseAdditionalXiaomiBTHome(uint8_t slot, const std::string& serviceData, int rssi) {
  if (slot>=XIAOMI_SENSOR_COUNT || serviceData.size()<2) return;
  const uint8_t* d=(const uint8_t*)serviceData.data(); size_t n=serviceData.size();
  uint8_t info=d[0]; if (((info>>5)&7)!=2 || (info&1)) return;
  bool useful=false; size_t i=1;
  while(i<n) {
    uint8_t id=d[i++];
    switch(id) {
      case 0x00: if(i+1>n)return; i++; break;
      case 0x01: if(i+1>n)return; xiaomiBatteries[slot]=d[i++]; useful=true; break;
      case 0x02: if(i+2>n)return; xiaomiTemperatures[slot]=(float)le16s(&d[i])*0.01f; i+=2; useful=true; break;
      case 0x03: if(i+2>n)return; xiaomiHumidities[slot]=(float)le16u(&d[i])*0.01f; i+=2; useful=true; break;
      case 0x09: case 0x10: case 0x11: case 0x15: case 0x16: case 0x17: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x3A:
        if(i+1>n)return; i++; break;
      case 0x0C: if(i+2>n)return; i+=2; break;
      default: return;
    }
  }
  if(useful) {
    xiaomiRSSIs[slot]=rssi; xiaomiLastPacket[slot]=millis(); xiaomiSensorSeen[slot]=true;
    Serial.print("XIAOMI "); Serial.print(slot+1); Serial.print(" ["); Serial.print(xiaomiRoomNames[slot]); Serial.print("] ");
    Serial.print(xiaomiTemperatures[slot],1); Serial.print(" C, "); Serial.print(xiaomiHumidities[slot],0); Serial.print(" %, RSSI "); Serial.println(rssi);
  }
}

// V26: друкуємо BTHome payload лише коли він змінився, щоб Serial Monitor не засипало дублями.
// Під час тесту натисни верхню кнопку, зачекай 2 с, потім нижню — і надішли ці рядки.
void printXiaomiRawPayloadIfChanged(const std::string& serviceData, int rssi) {
  static std::string lastPayload;
  if (serviceData == lastPayload) return;
  lastPayload = serviceData;

  Serial.print("XIAOMI RAW [RSSI ");
  Serial.print(rssi);
  Serial.print("] len=");
  Serial.print(serviceData.size());
  Serial.print(" : ");
  for (size_t k = 0; k < serviceData.size(); ++k) {
    uint8_t b = (uint8_t)serviceData[k];
    if (b < 0x10) Serial.print('0');
    Serial.print(b, HEX);
    if (k + 1 < serviceData.size()) Serial.print(' ');
  }
  Serial.println();
}

void parseXiaomiBTHome(const std::string& serviceData, int rssi) {
  printXiaomiRawPayloadIfChanged(serviceData, rssi);
  if (serviceData.size() < 2) return;
  const uint8_t* d = (const uint8_t*)serviceData.data();
  size_t n = serviceData.size();
  uint8_t info = d[0];
  uint8_t version = (info >> 5) & 0x07;
  bool encrypted = (info & 0x01) != 0;
  if (version != 2 || encrypted) return;
  bool gotUsefulData = false;
  bool gotTemperature = false;
  bool packetIdWasSeen = false;
  uint8_t packetIdThisFrame = 0;
  size_t i = 1;
  while (i < n) {
    uint8_t id = d[i++];
    switch (id) {
      case 0x00:
        if (i+1>n) return;
        xiaomiPacketId=d[i++];
        xiaomiPacketIdKnown=true;
        packetIdWasSeen=true;
        packetIdThisFrame=xiaomiPacketId;
        break;
      case 0x01: if (i+1>n) return; xiaomiBattery=d[i++]; gotUsefulData=true; break;
      case 0x02:
        if (i+2>n) return;
        xiaomiTemperature=(float)le16s(&d[i])*0.01f;
        i+=2;
        gotUsefulData=true;
        gotTemperature=true;
        break;
      case 0x03: if (i+2>n) return; xiaomiHumidity=(float)le16u(&d[i])*0.01f; i+=2; gotUsefulData=true; break;
      case 0x09: // V34 custom pvvx: TOP-button press counter (count8)
        if (i+1>n) return;
        {
          uint8_t newCount = d[i++];
          if (!xiaomiUpCounterKnown) {
            xiaomiUpCounterKnown = true;
            xiaomiUpCounter = newCount;
            Serial.print("XIAOMI UP COUNTER: baseline "); Serial.println(newCount);
          } else if (newCount != xiaomiUpCounter) {
            uint8_t diff = (uint8_t)(newCount - xiaomiUpCounter); // modulo-256, wrap 255->0 = 1
            xiaomiUpCounter = newCount;
            // A Xiaomi reboot can reset the counter and create a huge modulo jump.
            // Accept only a realistic burst of physical presses; otherwise just resync.
            if (diff >= 1 && diff <= 60) {
              uint16_t pending = (uint16_t)xiaomiUpperPressPendingCount + diff;
              xiaomiUpperPressPendingCount = (pending > 60) ? 60 : (uint8_t)pending;
              Serial.print("XIAOMI TOP DIRECT: +"); Serial.print(diff);
              Serial.print(" press(es), counter="); Serial.println(newCount);
            } else {
              Serial.print("XIAOMI UP COUNTER: resync "); Serial.println(newCount);
            }
          }
        }
        gotUsefulData=true; break;
      case 0x3D: // UPDOWN V2 custom DOWN count8 (not a standard BTHome object)
        if (i+1>n) return;
        {
          uint8_t next=d[i++];
          if (!xiaomiDownCounterKnown) {
            xiaomiDownCounterKnown=true; xiaomiDownCounter=next;
          } else if (next!=xiaomiDownCounter) {
            uint8_t diff=(uint8_t)(next-xiaomiDownCounter);
            xiaomiDownCounter=next;
            if (diff<=60) {
              uint16_t total=(uint16_t)xiaomiDownPressPendingCount+diff;
              xiaomiDownPressPendingCount=(total>60)?60:(uint8_t)total;
            }
          }
        }
        gotUsefulData=true; break;
      case 0x0C: if (i+2>n) return; i+=2; break;
      case 0x10: // BTHome power — НЕ кнопка MJWSD05MMC
        if (i+1>n) return;
        {
          bool ns=d[i++]!=0;
          if (!xiaomiPowerKnown || ns!=xiaomiPowerState) {
            xiaomiPowerKnown=true; xiaomiPowerState=ns;
            Serial.print("XIAOMI POWER: "); Serial.println(ns?"ON":"OFF");
          }
        }
        gotUsefulData=true; break;
      case 0x11: // BTHome opening — стан нижньої фізичної кнопки у нашій pvvx конфігурації
        if (i+1>n) return;
        {
          bool ns=d[i++]!=0;
          if (!xiaomiOpeningKnown || ns!=xiaomiOpeningState) {
            bool hadPreviousState = xiaomiOpeningKnown;
            xiaomiOpeningKnown=true; xiaomiOpeningState=ns;
            Serial.print("XIAOMI OPENING: "); Serial.println(ns?"OPEN":"CLOSED");
            // Перший пакет тільки фіксує базовий стан.
            // V32.2: одне фізичне натискання дає дві фази: OPEN->CLOSED (натиснули)
            // і CLOSED->OPEN (відпустили). Команду -0.5 C формуємо ТІЛЬКИ на CLOSED,
            // щоб одне натискання не зменшувало уставку двічі.
            if (false && hadPreviousState && !ns) { // V34.2: down now uses 0x3D count8
              // V34: direct lower-button edge. Upper button is independent via BTHome 0x09 counter.
              xiaomiLowerButtonPending = true;
              xiaomiLastLowerButtonMillis = millis();
              Serial.println("XIAOMI LOWER EDGE: CLOSED -> DOWN -0.5 pending");
            }
          }
        }
        gotUsefulData=true; break;
      case 0x15: case 0x16: case 0x17: case 0x23: case 0x24: case 0x25:
      case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
        if (i+1>n) return; i+=1; break;
      case 0x3A: // Ignore generic BTHome button events in the custom UPDOWN protocol.
        // UP is exclusively count8 0x09; prevents a second +0.5 for one press.
        if (i+1>n) return;
        i++;
        gotUsefulData=true; break;
      default: return;
    }
  }
  if (gotUsefulData) {
    unsigned long now = millis();
    xiaomiRSSI=rssi;
    lastXiaomiPacketMillis=now;
    xiaomiSeen=true;

    // V23: показуємо реальний інтервал між новими температурними пакетами Xiaomi.
    // Повтори того самого advertising packet не вважаємо новим вимірюванням.
    if (gotTemperature) {
      static uint8_t lastTempPacketId = 0;
      static bool lastTempPacketIdKnown = false;
      bool isNewTempPacket = !xiaomiPacketIdKnown || !lastTempPacketIdKnown || (xiaomiPacketId != lastTempPacketId);
      if (isNewTempPacket) {
        if (lastXiaomiTemperaturePacketMillis != 0) {
          xiaomiTemperaturePacketIntervalMs = now - lastXiaomiTemperaturePacketMillis;

          // V34: fast-advertising timing is informational only.
          // TOP-button detection no longer depends on advertising intervals.
          xiaomiPrevTemperatureIntervalMs = xiaomiTemperaturePacketIntervalMs;

          Serial.print("XIAOMI TEMP: ");
          Serial.print(xiaomiTemperature, 2);
          Serial.print(" C | interval: ");
          Serial.print(xiaomiTemperaturePacketIntervalMs);
          Serial.print(" ms | RSSI: ");
          Serial.println(xiaomiRSSI);
        } else {
          Serial.print("XIAOMI TEMP: ");
          Serial.print(xiaomiTemperature, 2);
          Serial.print(" C | first packet | RSSI: ");
          Serial.println(xiaomiRSSI);
        }
        lastXiaomiTemperaturePacketMillis = now;
        if (xiaomiPacketIdKnown) {
          lastTempPacketId = xiaomiPacketId;
          lastTempPacketIdKnown = true;
        }
      }
    }
  }
}

class XiaomiScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!dev) return;
    String mac=dev->getAddress().toString().c_str(); mac.toUpperCase();
    int slot=findXiaomiSlotByMac(mac);
    if(slot<0) return;
    std::string sd=dev->getServiceData(BTHOME_UUID);
    if(sd.empty()) return;
    if(slot==0) {
      xiaomiBleAddrType=dev->getAddressType();
      parseXiaomiBTHome(sd,dev->getRSSI());
      // Дзеркало першого датчика в масив V28
      xiaomiTemperatures[0]=xiaomiTemperature; xiaomiHumidities[0]=xiaomiHumidity; xiaomiBatteries[0]=xiaomiBattery;
      xiaomiRSSIs[0]=xiaomiRSSI; xiaomiLastPacket[0]=lastXiaomiPacketMillis; xiaomiSensorSeen[0]=xiaomiSeen;
    } else parseAdditionalXiaomiBTHome((uint8_t)slot,sd,dev->getRSSI());
  }
};

XiaomiScanCallbacks xiaomiScanCallbacks;

void startXiaomiBle() {
  NimBLEDevice::init("");
  xiaomiBleScan=NimBLEDevice::getScan();
  xiaomiBleScan->setScanCallbacks(&xiaomiScanCallbacks,true);
  xiaomiBleScan->setActiveScan(false);
  xiaomiBleScan->setInterval(160);
  xiaomiBleScan->setWindow(160); // V23: 100% scan duty cycle, не пропускаємо BTHome advertisements
  xiaomiBleScan->setMaxResults(0);
  xiaomiBleScan->start(0,false,true);
  Serial.println("XIAOMI BLE: BTHome FAST scan started (interval=160, window=160)");
}

// Повертає локальні секунди для дисплея Xiaomi.
// pvvx не має окремого timezone: на дисплей треба записати саме місцевий час.
uint32_t getKyivLocalSecondsForXiaomi() {
  time_t now;
  time(&now);
  if (now < 1700000000) return 0; // NTP ще не синхронізував ESP32

  struct tm lt;
  localtime_r(&now, &lt);
  // Для Europe/Kyiv: UTC+2 взимку, UTC+3 влітку.
  long offsetSeconds = (lt.tm_isdst > 0) ? 3L * 3600L : 2L * 3600L;
  return (uint32_t)((uint64_t)now + offsetSeconds);
}

bool syncXiaomiClockNow() {
  uint32_t localSeconds = getKyivLocalSecondsForXiaomi();
  if (localSeconds == 0) {
    Serial.println("XIAOMI TIME: NTP not ready");
    return false;
  }

  Serial.println("XIAOMI TIME: connecting for clock sync...");

  if (xiaomiBleScan && xiaomiBleScan->isScanning()) {
    xiaomiBleScan->stop();
    delay(80);
  }

  NimBLEClient* client = NimBLEDevice::createClient();
  if (!client) {
    Serial.println("XIAOMI TIME: cannot create BLE client");
    if (xiaomiBleScan) xiaomiBleScan->start(0,false,true);
    return false;
  }

  bool ok = false;
  NimBLEAddress target(std::string(xiaomiMacs[0].c_str()), xiaomiBleAddrType);

  if (client->connect(target)) {
    NimBLERemoteService* svc = client->getService(PVVX_SERVICE_UUID);
    if (svc) {
      NimBLERemoteCharacteristic* chr = svc->getCharacteristic(PVVX_COMMAND_UUID);
      if (chr && (chr->canWrite() || chr->canWriteNoResponse())) {
        uint8_t cmd[5];
        cmd[0] = 0x23;
        cmd[1] = (uint8_t)(localSeconds & 0xFF);
        cmd[2] = (uint8_t)((localSeconds >> 8) & 0xFF);
        cmd[3] = (uint8_t)((localSeconds >> 16) & 0xFF);
        cmd[4] = (uint8_t)((localSeconds >> 24) & 0xFF);

        ok = chr->writeValue(cmd, sizeof(cmd), false); // write without response
        if (ok) {
          time_t now; time(&now);
          struct tm lt; localtime_r(&now, &lt);
          char tb[32];
          strftime(tb, sizeof(tb), "%d.%m.%Y %H:%M:%S", &lt);
          Serial.print("XIAOMI TIME: synced -> " );
          Serial.println(tb);
        } else {
          Serial.println("XIAOMI TIME: write failed");
        }
      } else {
        Serial.println("XIAOMI TIME: characteristic 0x1F1F not found/writable");
      }
    } else {
      Serial.println("XIAOMI TIME: service 0x1F10 not found");
    }
    client->disconnect();
  } else {
    Serial.println("XIAOMI TIME: BLE connect failed");
  }

  NimBLEDevice::deleteClient(client);
  delay(80);
  if (xiaomiBleScan && !xiaomiBleScan->isScanning()) {
    xiaomiBleScan->start(0,false,true);
  }
  return ok;
}

// V28: 4 Xiaomi sensors + room names + heating source selector; V27.1 NimBLE 2.x fix retained
// V27: показує уставку великими цифрами на дисплеї Xiaomi на кілька секунд.
// pvvx external_data_t для MJWSD05MMC: s32 number x0.01, u16 validity seconds, u8 flags.
// flags: bits 4..7 = temp symbol; 0x03 = °C, тому 0x30.
bool showXiaomiSetpointOnLcdNow(float setpointC, uint16_t seconds) {
  Serial.print("XIAOMI LCD: connecting to show setpoint ");
  Serial.print(setpointC, 1);
  Serial.println(" C...");

  if (xiaomiBleScan && xiaomiBleScan->isScanning()) {
    xiaomiBleScan->stop();
    delay(10);
  }

  NimBLEClient* client = NimBLEDevice::createClient();
  if (!client) {
    Serial.println("XIAOMI LCD: cannot create BLE client");
    if (xiaomiBleScan) xiaomiBleScan->start(0,false,true);
    return false;
  }

  bool ok = false;
  NimBLEAddress target(std::string(xiaomiMacs[0].c_str()), xiaomiBleAddrType);
  if (client->connect(target)) {
    NimBLERemoteService* svc = client->getService(PVVX_SERVICE_UUID);
    if (svc) {
      NimBLERemoteCharacteristic* chr = svc->getCharacteristic(PVVX_COMMAND_UUID);
      if (chr && (chr->canWrite() || chr->canWriteNoResponse())) {
        int32_t number = (int32_t)lroundf(setpointC * 100.0f);
        uint8_t cmd[8];
        cmd[0] = 0x22; // Get/Set show LCD ext.data
        cmd[1] = (uint8_t)(number & 0xFF);
        cmd[2] = (uint8_t)((number >> 8) & 0xFF);
        cmd[3] = (uint8_t)((number >> 16) & 0xFF);
        cmd[4] = (uint8_t)((number >> 24) & 0xFF);
        cmd[5] = (uint8_t)(seconds & 0xFF);
        cmd[6] = (uint8_t)((seconds >> 8) & 0xFF);
        cmd[7] = 0x30; // smiley=0, battery=0, temp_symbol=0x03 (°C)

        ok = chr->writeValue(cmd, sizeof(cmd), false);
        if (ok) {
          Serial.print("XIAOMI LCD: setpoint shown for ");
          Serial.print(seconds);
          Serial.println(" s");
        } else {
          Serial.println("XIAOMI LCD: write failed");
        }
      } else {
        Serial.println("XIAOMI LCD: characteristic 0x1F1F not found/writable");
      }
    } else {
      Serial.println("XIAOMI LCD: service 0x1F10 not found");
    }
    client->disconnect();
  } else {
    Serial.println("XIAOMI LCD: BLE connect failed");
  }

  NimBLEDevice::deleteClient(client);
  delay(10);
  if (xiaomiBleScan && !xiaomiBleScan->isScanning()) {
    xiaomiBleScan->start(0,false,true);
  }
  return ok;
}

void processXiaomiSetpointDisplay() {
  if (!xiaomiSetpointDisplayPending) return;
  // V31: не блокуємо ESP32 BLE-підключенням після кожного швидкого кліку.
  // Чекаємо коротку паузу після ОСТАННЬОЇ зміни уставки і показуємо вже фінальне значення.
  if (millis() - xiaomiSetpointQueuedAt < XIAOMI_LCD_DEBOUNCE_MS) return;
  // Do not discard a pending update if the BLE connection failed.
  if (xiaomiLcdLastAttempt != 0 && millis() - xiaomiLcdLastAttempt < XIAOMI_LCD_RETRY_MS) return;
  float value = xiaomiSetpointToDisplay;
  unsigned long queuedAt = xiaomiSetpointQueuedAt;
  xiaomiLcdLastAttempt = millis();
  bool ok = showXiaomiSetpointOnLcdNow(value, XIAOMI_SETPOINT_LCD_SECONDS);
  // A newer web/button command may have arrived while the BLE write was blocking.
  if (queuedAt == xiaomiSetpointQueuedAt && fabsf(value - xiaomiSetpointToDisplay) < 0.01f) {
    xiaomiSetpointDisplayPending = !ok;
  }
}

// V29: єдина точка постановки уставки в чергу на LCD Xiaomi.
// Викликається після будь-якої зміни уставки: WEB, MQTT, кнопка Xiaomi, повернення до розкладу.
void queueXiaomiSetpointDisplay(float setpointC) {
  if (setpointC < 5.0f || setpointC > 30.0f) return;
  xiaomiSetpointToDisplay = setpointC;
  xiaomiSetpointQueuedAt = millis();
  xiaomiSetpointDisplayPending = true;
}

void processXiaomiTimeSync() {
  if (xiaomiSetpointDisplayPending) return; // LCD update takes priority over clock sync
  unsigned long nowMs = millis();

  // Не пробуємо, доки датчик ще жодного разу не побачено.
  if (!xiaomiSeen) return;

  // Перша синхронізація — одразу після появи коректного NTP часу.
  bool due = !xiaomiTimeSynced || (nowMs - lastXiaomiTimeSyncMillis >= XIAOMI_TIME_SYNC_INTERVAL);
  if (!due) return;

  if (lastXiaomiTimeSyncAttemptMillis != 0 &&
      nowMs - lastXiaomiTimeSyncAttemptMillis < XIAOMI_TIME_SYNC_RETRY) return;

  lastXiaomiTimeSyncAttemptMillis = nowMs;
  if (syncXiaomiClockNow()) {
    xiaomiTimeSynced = true;
    lastXiaomiTimeSyncMillis = millis();
  }
}

// =====================================================
// ПОЛИВ
// =====================================================

struct WaterZone {
  bool enabled;
  uint8_t hour;
  uint8_t minute;
  uint16_t duration; // хвилини
  uint8_t days;      // bit0=Нд ... bit6=Сб
  bool intervalEnabled;
  uint16_t intervalHours;
  uint8_t intervalMinutes;
};

WaterZone zones[4];

bool watering = false;
bool waterAllActive = false;
bool waterSimultaneousActive = false;
bool waterSimZoneActive[4] = {false, false, false, false};
unsigned long waterSimStartMillis = 0;
int currentZone = -1;

unsigned long zoneStartMillis = 0;
unsigned long zoneDurationMillis = 0;

int waterQueue[4];
int queueCount = 0;
int queuePosition = 0;

long lastWaterRunKey[4] = {-1, -1, -1, -1};
uint64_t lastWaterEpoch[4] = {0, 0, 0, 0};

// =====================================================
// ДОЩ
// =====================================================

bool rainProtectionEnabled = true;
uint16_t rainDelayHours = 12;
String personalMemo = "";

bool previousRainState = false;
uint64_t rainBlockUntil = 0;

// =====================================================
// ОСВІТЛЕННЯ
// =====================================================

struct LightChannel {
  bool enabled;
  uint8_t onHour;
  uint8_t onMinute;
  uint8_t offHour;
  uint8_t offMinute;
  uint8_t days;
};

LightChannel lights[LIGHT_COUNT];

long lastLightOnKey[LIGHT_COUNT]  = {-1, -1, -1};
long lastLightOffKey[LIGHT_COUNT] = {-1, -1, -1};

// =====================================================
// ТЕСТОВИЙ РЕЖИМ 8-КАНАЛЬНОГО РЕЛЕЙНОГО БЛОКА
// =====================================================
bool relayTestMode = false;
bool relayTestState[7] = {false,false,false,false,false,false,false};


// =====================================================
// КОТЕЛ
// =====================================================

enum BoilerMode {
  BOILER_OFF = 0,
  BOILER_MANUAL = 1,
  BOILER_AUTO = 2
};

BoilerMode boilerMode = BOILER_OFF;

bool boilerState = false;

float roomTemperature = -127.0;

// Бездротова температура через MQTT/Home Assistant.
// Наприклад Shelly BLU H&T може передаватися в HA через Bluetooth Proxy,
// а HA публікує температуру в MQTT topic:
// garden/controller/boiler/external_temperature/set
float externalRoomTemperature = -127.0;
unsigned long lastExternalTempMillis = 0;
const unsigned long EXTERNAL_TEMP_TIMEOUT = 300000UL; // 5 хв

float boilerHysteresis = 0.5;

unsigned long lastTempRead = 0;
const unsigned long TEMP_READ_INTERVAL = 5000;

// Денний режим
uint8_t boilerDayHour = 6;
uint8_t boilerDayMinute = 30;
float boilerDayTemp = 21.5;

// Нічний режим
uint8_t boilerNightHour = 23;
uint8_t boilerNightMinute = 0;
float boilerNightTemp = 19.0;

// Активна уставка
float boilerCurrentSetpoint = 21.5;

// Ручна корекція температури з екрана "Опалення"
bool heatingSetpointOverride = false;
float heatingSetpoint = 21.5;

// =====================================================
// ВІДПУСТКА
// =====================================================

bool vacationMode = false;
float vacationTemperature = 15.0;

bool vacationEndEnabled = false;
uint64_t vacationEndEpoch = 0;

// =====================================================
// ПРОТОТИПИ
// =====================================================

void stopWatering();
void setLight(int channel, bool state);
void allWaterOff();
void setBoiler(bool state);
void saveSettings();
void updateBoilerSetpointBySchedule();
void handleHeatingTemperature();
void saveHeatingSetpointOnly();
void processXiaomiThermostatButtons();
void logEvent(const String& source, const String& event, const String& details = "");
String readLogFile();
bool clearLogFile();
bool sendLogByEmail();

void ensureWiFi();
void ensureMQTT();

void loadWiFiSettings();
void saveWiFiSettings(const String& ssid, const String& password);
bool connectWiFi(unsigned long timeoutMs = 20000UL);
void startWiFiSetupAP();
void handleWiFiSettings();
void handleWiFiSave();

void loadRemoteSettings();
void saveRemoteSettings();
void configureMqttTransport();
void handleRemoteSettings();
void handleRemoteSave();

void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishMQTTState(bool retained = true);
void publishHomeAssistantDiscovery();

float getEffectiveRoomTemperature();
bool externalTemperatureValid();

String buildStatusJson();
void handleApiStatus();
void handleApiWater();
void handleWaterAllTogether();
void handleApiLight();
void handleApiBoiler();
void handleApiVacation();
void handleApiExternalTemperature();
void handleExtraTempSensor();
void handleApiLogClear();
void handleApiLogEmail();
void handleManifest();
void handlePwaInstall();
void handlePwaIcon180();
void handlePwaIcon512();
void handleServiceWorker();
void handleRelayTest();
void handleRelayTestSet();
void handleRelayTestExit();
void setRelayTestChannel(int relayIndex, bool state);
void allRelayTestOff();

// =====================================================
// XIAOMI — ФІЗИЧНІ КНОПКИ ТЕРМОСТАТА
// =====================================================
void applyXiaomiHeatingDelta(float delta, const char* source) {
  // Захист від випадкового дублювання одного натискання кількома BLE advertisement.
  unsigned long now = millis();
  if (now - lastXiaomiThermostatActionMillis < XIAOMI_BUTTON_DEBOUNCE) return;
  lastXiaomiThermostatActionMillis = now;

  if (vacationMode) {
    vacationTemperature += delta;
    if (vacationTemperature < 5.0f) vacationTemperature = 5.0f;
    if (vacationTemperature > 30.0f) vacationTemperature = 30.0f;
    boilerMode = BOILER_AUTO;
    boilerCurrentSetpoint = vacationTemperature;
    logEvent("HEATING", "XIAOMI_BUTTON_VACATION", String(source) + " -> " + String(vacationTemperature, 1) + " C");
  } else {
    if (!heatingSetpointOverride) {
      updateBoilerSetpointBySchedule();
      heatingSetpoint = boilerCurrentSetpoint;
    } else if (boilerCurrentSetpoint >= 5.0f && boilerCurrentSetpoint <= 30.0f &&
               fabsf(heatingSetpoint - boilerCurrentSetpoint) > 0.01f) {
      // V32: єдина актуальна уставка — boilerCurrentSetpoint; не працюємо зі старим/пошкодженим значенням.
      heatingSetpoint = boilerCurrentSetpoint;
    }
    heatingSetpoint += delta;
    if (heatingSetpoint < 5.0f) heatingSetpoint = 5.0f;
    if (heatingSetpoint > 30.0f) heatingSetpoint = 30.0f;
    heatingSetpointOverride = true;
    boilerMode = BOILER_AUTO;
    boilerCurrentSetpoint = heatingSetpoint;
    logEvent("HEATING", "XIAOMI_BUTTON_SETPOINT", String(source) + " -> " + String(heatingSetpoint, 1) + " C");
  }

  // V28: 4 Xiaomi sensors + room names + heating source selector; V27.1 NimBLE 2.x fix retained
// V27: після зміни уставки коротко покажемо її на LCD Xiaomi.
  queueXiaomiSetpointDisplay(boilerCurrentSetpoint);

  saveHeatingSetpointOnly();
  publishMQTTState(true);
  Serial.print("XIAOMI THERMOSTAT "); Serial.print(source);
  Serial.print(": "); Serial.print(boilerCurrentSetpoint, 1); Serial.println(" C");
}

void processXiaomiThermostatButtons() {
  // V34: direct TOP button from custom pvvx BTHome 0x09 count8.
  // If several presses happened between two received advertisements, apply all of them at once.
  uint8_t upPresses = 0;
  noInterrupts();
  upPresses = xiaomiUpperPressPendingCount;
  xiaomiUpperPressPendingCount = 0;
  interrupts();
  if (upPresses) {
    applyXiaomiHeatingDelta(0.5f * (float)upPresses, "UP DIRECT");
  }

  // V34: BTHome 0x11 transition to CLOSED -> lower button -> -0.5 C
  uint8_t downPresses=0;
  noInterrupts();
  downPresses=xiaomiDownPressPendingCount;
  xiaomiDownPressPendingCount=0;
  xiaomiLowerButtonPending=false;
  interrupts();
  if (downPresses) applyXiaomiHeatingDelta(-0.5f*(float)downPresses, "DOWN COUNT");
}


// PWA icons stored directly in ESP32 Flash
const uint8_t PWA_ICON_180[] PROGMEM = {
  0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0xB4,
  0x00,0x00,0x00,0xB4,0x08,0x02,0x00,0x00,0x00,0xB2,0xAF,0x91,0x65,0x00,0x00,0x0B,0xAF,0x49,0x44,0x41,
  0x54,0x78,0xDA,0xED,0x9D,0x79,0x54,0x54,0xD7,0x1D,0xC7,0xDF,0x0C,0x8F,0x59,0x98,0x61,0xD8,0x11,0x04,
  0x64,0x73,0x61,0x13,0xDC,0xC1,0x5D,0x1B,0x54,0x50,0x71,0xC1,0x68,0xC2,0xB1,0xB8,0x24,0x7A,0x4A,0xAA,
  0x55,0x13,0x6D,0x13,0x93,0x9A,0xA3,0x27,0x69,0x4F,0xBA,0x1C,0x9B,0xB8,0x24,0x56,0x13,0x93,0xA3,0xD1,
  0xB4,0x21,0x51,0xC1,0xA6,0xEE,0x56,0x89,0xA9,0xB1,0x82,0x01,0x44,0xA5,0x51,0x24,0x11,0x91,0x45,0x50,
  0xC6,0x01,0x87,0x81,0x59,0xFA,0x07,0x3D,0x66,0x7C,0x6F,0x98,0x8D,0x79,0x33,0x6F,0xF9,0x7E,0xFF,0xD2,
  0x7B,0xEE,0xBD,0xF3,0x96,0xCF,0xFB,0xFE,0x7E,0xF7,0xDE,0xC7,0x7D,0x22,0x9F,0x37,0xC7,0x11,0x10,0x64,
  0x49,0x62,0x5C,0x02,0x08,0x70,0x40,0x80,0x03,0x02,0x1C,0x10,0xE0,0x80,0x00,0x07,0x04,0x38,0x20,0xC0,
  0x01,0x01,0x0E,0x08,0x70,0x40,0x80,0x03,0x82,0x00,0x07,0x04,0x38,0xA0,0xBE,0x8B,0xF4,0xE0,0x6F,0x7B,
  0x87,0x2B,0x71,0x03,0xEC,0x51,0x77,0x43,0xBB,0x20,0xE0,0x00,0x10,0x7D,0xBC,0x68,0xEE,0x04,0x85,0x04,
  0x13,0x1C,0x05,0xC5,0x0D,0x94,0x90,0xC0,0x82,0xD3,0x94,0x30,0x8A,0x08,0x09,0x2C,0x80,0x88,0xBB,0x47,
  0x2B,0x20,0x83,0x07,0x99,0x1C,0x09,0x2C,0x60,0x21,0x6E,0x72,0x0E,0x90,0xC1,0x27,0x0B,0x11,0x83,0x0C,
  0xF0,0xC1,0x38,0x1C,0x20,0x83,0x7F,0x7C,0x88,0x41,0x06,0xF8,0x60,0x10,0x0E,0x90,0xC1,0x57,0x3E,0xC4,
  0x20,0x03,0x7C,0x30,0x02,0x07,0xC8,0xE0,0x37,0x1F,0x58,0xB2,0x87,0x18,0x80,0x03,0xB6,0xC1,0x7B,0xF3,
  0x10,0x83,0x0C,0xF0,0x81,0xB0,0x02,0x39,0x2C,0xD2,0x6D,0x18,0x32,0xAA,0x6F,0x0B,0x76,0xD9,0xAC,0x93,
  0xB1,0xAB,0x40,0xE0,0xE6,0xE1,0xE8,0xCA,0x0B,0xC9,0xDD,0xB3,0xB5,0x07,0x88,0xDE,0xEA,0x0B,0x1C,0x14,
  0x3B,0x25,0x72,0x74,0x67,0x1F,0x36,0xD8,0x86,0xA3,0x58,0xC0,0x4B,0x9E,0xC8,0x21,0xF3,0xE0,0x98,0x73,
  0xB8,0x0A,0x0B,0xF3,0xDE,0xE0,0x22,0x9C,0x87,0xC3,0xB5,0x58,0x00,0x11,0xD7,0x87,0x15,0x8F,0xC4,0x14,
  0xE6,0xB0,0x70,0x2E,0xD0,0xB4,0x15,0x9C,0xF0,0xDF,0x35,0x43,0x08,0x91,0x45,0x2C,0x58,0xC3,0x70,0xEE,
  0xE7,0xDA,0x0A,0x4E,0x08,0xC7,0x39,0xC4,0x20,0xC3,0xFE,0x1F,0xED,0x21,0x83,0xD3,0xB6,0xC1,0x87,0x9C,
  0xC3,0x23,0x58,0x58,0xCF,0x42,0x04,0xE5,0x19,0x0E,0x3B,0x87,0xDB,0x12,0x0E,0xCF,0x92,0x61,0xF1,0x30,
  0xF8,0x44,0x86,0xFD,0xF7,0x91,0x75,0x61,0x85,0x25,0x64,0x98,0x1F,0x8C,0x39,0x19,0xC2,0x89,0x29,0xAC,
  0x83,0x83,0x55,0x64,0x08,0x36,0x9A,0x70,0x66,0xB4,0x02,0x01,0x0E,0x96,0xDA,0x46,0x02,0x11,0x6B,0xFE,
  0x5F,0x41,0xC5,0x14,0x16,0xC1,0xC1,0x7E,0x32,0xE0,0x1C,0x20,0x03,0x64,0x20,0xE7,0x00,0x19,0x9C,0x80,
  0x83,0x6D,0xB6,0xD1,0x1B,0x19,0xD5,0x44,0x2D,0x0B,0x1D,0x0E,0xCE,0x01,0xCF,0x10,0x24,0x1C,0xAC,0x7A,
  0x16,0xED,0x21,0x43,0x50,0xE6,0x01,0xE7,0xB0,0x8B,0x8C,0x6A,0xA2,0x16,0xCE,0x21,0x50,0xDB,0x70,0x28,
  0x9A,0x08,0xC7,0x3C,0xE0,0x1C,0xC8,0x33,0x00,0x47,0x1F,0xC8,0x10,0x66,0x4C,0xF1,0x24,0x1C,0x6C,0x30,
  0x67,0xA7,0x3D,0x43,0x20,0x91,0x05,0x61,0x05,0x02,0x1C,0xCE,0xDA,0x86,0x60,0x63,0x8A,0x70,0xE1,0x40,
  0x12,0xCA,0x5E,0x38,0x3C,0x1B,0xB3,0x5D,0x42,0x86,0x10,0xD2,0x0E,0xC1,0x39,0x87,0x43,0x64,0x08,0x39,
  0xA6,0x08,0x0E,0x0E,0x44,0x13,0xC0,0x01,0x32,0x00,0x07,0xC3,0x64,0x08,0x3C,0xA6,0x08,0x05,0x0E,0x78,
  0x06,0xE0,0x00,0x19,0x80,0xC3,0x2D,0x64,0x20,0xA6,0xF0,0x1C,0x0E,0x78,0x06,0xE0,0x80,0x00,0x07,0x6C,
  0x03,0x70,0xB0,0x81,0x0C,0x24,0x1C,0x9E,0x84,0x83,0xD1,0x0D,0xB8,0xDC,0xE3,0x19,0x42,0xD8,0x43,0x8C,
  0x6F,0xCE,0x81,0x68,0x02,0x38,0x18,0x24,0x03,0x31,0x85,0x87,0x70,0xC0,0x33,0xF8,0x03,0x87,0x6B,0x63,
  0xB6,0x9B,0xC9,0x10,0xC8,0xA6,0xA5,0x7C,0x70,0x0E,0x17,0x92,0x81,0x98,0xC2,0x2B,0x38,0x10,0x4D,0xF8,
  0x09,0x47,0xDF,0xCD,0xD9,0x23,0x64,0x08,0x67,0x23,0x6C,0x0E,0x3B,0x87,0xCB,0xC9,0x40,0x4C,0x61,0x17,
  0x1C,0x4E,0x3F,0x85,0x9E,0x8A,0x26,0x82,0xDA,0x3F,0x1F,0x0B,0x6F,0x10,0x8B,0xE1,0x70,0xE2,0x59,0x64,
  0xC2,0x36,0xEC,0x89,0x29,0x42,0xFB,0xEC,0x06,0xF7,0x9C,0x03,0xC3,0x13,0x61,0xC1,0x61,0xFF,0x13,0xE9,
  0x41,0x32,0x04,0xF8,0xB5,0x1E,0xB6,0x38,0x87,0x3D,0x97,0x9E,0x39,0x32,0x6C,0xC6,0x14,0x61,0x7E,0xC7,
  0x89,0x45,0x61,0xC5,0xFA,0x0D,0x80,0x67,0x20,0xE7,0x40,0x9E,0x01,0x38,0x1C,0x7C,0x46,0xAB,0x89,0x5A,
  0xE6,0x66,0xA8,0xAC,0xF7,0x2C,0xE4,0x0F,0x03,0xB2,0xCE,0x39,0xAC,0xDC,0x8C,0x1E,0x44,0xDC,0x39,0x8F,
  0x29,0xF0,0x4F,0x46,0xB2,0x31,0xAC,0xD8,0xBC,0x25,0xEE,0x41,0x04,0x1F,0x13,0x65,0xE9,0x37,0xDE,0x7A,
  0x6E,0x8C,0xF5,0x3D,0x30,0x7A,0xF8,0x60,0x22,0x1D,0x01,0x16,0x1C,0x48,0x48,0xED,0xB9,0x49,0x7D,0x74,
  0x11,0x7A,0x5B,0x90,0xC1,0x99,0xD1,0x8A,0x9D,0xB7,0xCA,0x55,0xE9,0x08,0xC8,0xE0,0x40,0x58,0x71,0x34,
  0xC4,0xF4,0x3D,0xD6,0x00,0x0B,0xBA,0x1C,0xF8,0x5C,0xB9,0x47,0xBE,0x55,0x4E,0x91,0x43,0x3B,0x71,0xD9,
  0x44,0xA4,0x87,0x24,0x01,0x62,0x61,0xE7,0x17,0xCB,0x39,0x06,0x87,0x6B,0x11,0x11,0xDA,0x37,0xDB,0x1C,
  0x85,0x83,0xE4,0xE2,0xB9,0x39,0x11,0x68,0x08,0x4C,0xB3,0xF2,0x32,0xE7,0xB0,0x27,0x4B,0xB0,0x07,0x94,
  0x1E,0x9F,0x78,0xF2,0x9D,0x58,0xC1,0xDA,0x86,0x20,0xE0,0x70,0x2E,0x9D,0xA4,0x20,0x02,0x71,0x78,0x28,
  0xCB,0x90,0x60,0x1B,0x80,0x03,0x02,0x1C,0x10,0xE0,0x80,0x00,0x07,0x04,0x38,0x20,0xC0,0x01,0x01,0x0E,
  0x08,0x70,0x40,0x80,0x03,0x82,0x00,0x07,0x04,0x38,0x20,0xC0,0x01,0x01,0x0E,0x08,0x70,0x40,0x9E,0x16,
  0x29,0xE4,0x93,0x0F,0x94,0xA9,0xB2,0xA2,0x33,0xC6,0x87,0x0F,0x1D,0x1C,0x10,0x15,0xA5,0x0C,0x55,0x4A,
  0x7C,0xE4,0xA4,0x54,0x67,0xE8,0x6A,0xEF,0xD6,0x36,0x76,0xB4,0x56,0x3F,0xBC,0x53,0xDA,0x74,0xE3,0xAB,
  0x1F,0x2E,0xD6,0xB7,0xDF,0x17,0xE6,0xF5,0x61,0xF5,0x3B,0xA4,0x4B,0x12,0xB3,0xB7,0x4D,0x5E,0xD7,0x97,
  0x1E,0xC2,0xF6,0xE4,0x74,0x1A,0xBA,0xE8,0xE5,0xA1,0xF2,0x80,0x8D,0xA3,0xF3,0xF3,0x13,0xB2,0x48,0xB1,
  0x97,0xCD,0x4E,0x8A,0x6F,0x5F,0xD8,0x72,0x69,0x6F,0x8D,0xBA,0x9E,0x37,0x77,0xDD,0xCE,0x77,0x48,0x85,
  0x18,0x56,0x92,0x02,0x63,0xCF,0x3D,0xBB,0x63,0x79,0xD2,0x2C,0x7B,0xC8,0x20,0x08,0x62,0x4E,0xDC,0x84,
  0x73,0x0B,0x76,0x4C,0x89,0x1C,0x8E,0x9C,0x83,0xE7,0x52,0x7A,0xCB,0x3F,0xCB,0xDE,0xDC,0x5F,0x11,0xEC,
  0x50,0x2B,0x5F,0x89,0xCF,0xFE,0xE9,0x6F,0x46,0x2A,0x43,0x01,0x07,0x9F,0xB5,0x22,0x25,0x27,0xDA,0x37,
  0xCC,0x89,0x86,0xBE,0x12,0x9F,0xF5,0x23,0x9E,0x07,0x1C,0x3C,0xD1,0xF5,0x07,0xB5,0xF4,0x84,0x23,0x27,
  0x76,0x02,0xBD,0x66,0x47,0xB7,0x76,0xF3,0xA5,0x8F,0x86,0x1D,0x5C,0x16,0xB6,0x27,0x27,0xE5,0xD3,0xFC,
  0x57,0x4A,0xB6,0xB5,0x76,0xAA,0xE9,0xD5,0x66,0xC7,0x8E,0xC7,0x68,0x85,0x2D,0xDA,0x77,0xE3,0xD8,0xBE,
  0x1B,0xC7,0x6C,0x56,0x93,0x7A,0x79,0x57,0x2E,0xDE,0xDF,0xCF,0x27,0x80,0x52,0xBE,0xAD,0xBC,0x90,0x5E,
  0x39,0x56,0x15,0x4E,0x2F,0x7C,0xB9,0x64,0xDB,0xE7,0x37,0xCF,0xF6,0xFC,0xFB,0x6E,0x7B,0xF3,0xDE,0xEB,
  0x5F,0xD5,0xA8,0xEF,0x15,0xE5,0xBC,0x43,0xA9,0x16,0x22,0xF7,0xF7,0x21,0x65,0x8F,0xF5,0x9D,0x80,0x83,
  0x33,0x5A,0x34,0xE8,0x19,0x3A,0x19,0x77,0xDB,0x9B,0xBF,0xB8,0x75,0x8E,0x5E,0x59,0x46,0x4A,0x28,0x25,
  0x46,0x93,0xA9,0xB8,0xF6,0x1B,0x4A,0xE1,0xF9,0xFA,0xEF,0xDA,0x74,0xED,0xFE,0x52,0x25,0xBD,0x79,0x6F,
  0x70,0x04,0xCB,0xFD,0x9E,0x89,0x1A,0x35,0x2A,0x34,0x21,0x39,0x28,0x36,0x46,0x15,0xAE,0x92,0x28,0xE4,
  0xA4,0xB4,0xA3,0x5B,0xAB,0xE9,0x7A,0x5C,0xDF,0xD1,0x52,0xD5,0x7A,0xFB,0x3F,0x8D,0xD7,0x8E,0xD6,0xFE,
  0xBB,0xA3,0x5B,0x0B,0x38,0xDC,0x35,0x16,0x27,0x44,0xAB,0xD3,0x16,0xD0,0xCB,0x77,0x56,0x1E,0xD2,0x1B,
  0x0D,0xF4,0xF2,0x3A,0x4D,0xF3,0x90,0x80,0x01,0xB4,0x4E,0x2C,0xF6,0x6C,0x21,0xFA,0xB4,0xE9,0x34,0x94,
  0x42,0x39,0x29,0x9D,0x17,0x3F,0x69,0x69,0x62,0x56,0x7A,0x58,0xB2,0x88,0xD6,0x48,0x25,0x51,0xA8,0x24,
  0x8A,0x08,0x65,0xC8,0x98,0x7E,0x89,0x2F,0x24,0xCD,0xDA,0xAA,0xEF,0xFC,0xB0,0xEA,0xE8,0xEF,0x4B,0xF7,
  0x77,0xEA,0x75,0xC8,0x39,0x18,0xD7,0x8C,0xE8,0x74,0xFA,0xCD,0x6E,0xD3,0xB5,0xEF,0xBB,0x71,0xDC,0x62,
  0xFD,0xD3,0x75,0x97,0xA9,0x97,0x40,0x24,0x9A,0x13,0x47,0x4D,0x44,0xA6,0x46,0x8E,0xF0,0xA3,0xD9,0xC6,
  0xE9,0xBA,0x52,0xA3,0xC9,0x44,0xCD,0x70,0x93,0x73,0x3E,0x98,0xBA,0x21,0x23,0x2C,0x45,0x64,0x99,0xB1,
  0xA7,0xE4,0x43,0xCA,0xD6,0x0C,0x5B,0x78,0x2E,0x77,0x7B,0x80,0xD4,0x17,0x70,0x30,0xAE,0x35,0xC3,0x9E,
  0xA5,0x17,0xEE,0xA9,0x2A,0xEE,0xCD,0xBD,0x77,0x54,0x1C,0x52,0xEB,0xA8,0x53,0x40,0x5B,0x27,0xFE,0x6A,
  0xED,0xB0,0x45,0x31,0xAA,0x70,0xA9,0x97,0x77,0x84,0x32,0x64,0x79,0xD2,0xAC,0x0F,0x33,0x5F,0xA3,0xD4,
  0xE9,0x32,0xEA,0xFF,0x58,0x76,0xC0,0x25,0xC7,0x9C,0x10,0x18,0xBD,0x6F,0xFA,0x26,0x84,0x15,0x66,0x35,
  0x32,0x74,0xC8,0xB8,0xF0,0xA1,0x94,0xC2,0x4E,0x43,0xD7,0xEE,0xAA,0xA2,0xDE,0x9A,0x34,0x74,0xB4,0xE4,
  0x9F,0x7C,0xEB,0x60,0xD6,0x66,0xA5,0xB7,0xFC,0x49,0xA1,0xC2,0x5B,0xBE,0x25,0xE3,0xC5,0x2D,0x19,0x2F,
  0xF6,0xD6,0xAA,0xCB,0xA8,0xFF,0xC5,0x99,0x3F,0x5C,0x6B,0x75,0xD9,0x46,0x64,0x13,0x23,0xD2,0xA6,0x0D,
  0x18,0x7D,0xEA,0xCE,0x65,0xC0,0xC1,0x9C,0x6D,0x2C,0xA4,0x17,0x1E,0xA8,0x3E,0x79,0x5F,0xDB,0x66,0xA5,
  0x55,0x49,0x7D,0xF9,0x84,0xC2,0x97,0xDE,0x1E,0xBB,0x72,0x56,0xEC,0x38,0x7B,0x62,0xC1,0xBF,0xEE,0x5E,
  0xF9,0xED,0xC5,0xDD,0xD6,0xC9,0xD0,0x19,0xBA,0xCF,0xD6,0x95,0x9E,0xA9,0x2B,0xAB,0x6C,0xB9,0x75,0x47,
  0xD3,0xA4,0xEE,0xEA,0xE8,0x32,0x74,0x2B,0xBC,0xE5,0x03,0x7C,0xFB,0x4D,0xE8,0x9F,0xB6,0x2A,0x35,0x37,
  0xCA,0x97,0x3A,0x81,0xB6,0x3C,0x69,0x16,0xCB,0xE1,0xE0,0xE4,0xFE,0x1C,0x3D,0x8A,0x51,0x85,0x97,0xE5,
  0xED,0xF5,0x12,0x3D,0x15,0x19,0x0D,0x26,0xE3,0xA8,0xCF,0x5E,0xA8,0x7D,0xD4,0x60,0xB3,0xB9,0x4A,0xA2,
  0xF8,0x65,0x6A,0xEE,0x86,0x11,0x79,0x56,0x26,0xD1,0x4D,0x84,0xE9,0xAF,0x57,0x8B,0xFE,0x7C,0xE5,0x60,
  0x8B,0x56,0xDD,0x5B,0x9D,0x65,0x49,0x33,0xFB,0x2B,0x82,0xDF,0xAF,0x3C,0xD4,0xA6,0xEB,0x75,0xC1,0x22,
  0x58,0xEE,0x77,0xE9,0xB9,0x3D,0x41,0x32,0x3F,0xF3,0xC2,0x87,0x3A,0x4D,0xDC,0xC7,0x0B,0x4D,0x84,0xC9,
  0xFD,0x97,0x8E,0xCF,0xFB,0x73,0xF4,0x68,0x55,0x6A,0x2E,0x85,0x0C,0x82,0x20,0x8A,0x6F,0x5F,0xB0,0x49,
  0x86,0x58,0x24,0x5A,0x95,0xBA,0x60,0xC3,0x88,0x3C,0x7A,0xCA,0x49,0x1F,0x0A,0x15,0x0C,0x9D,0xB7,0x34,
  0x31,0x7B,0x7B,0xC5,0x17,0x7F,0x2A,0x3B,0xD8,0x6D,0xD4,0xD3,0xEB,0x7C,0x72,0xFD,0x9F,0x36,0x0F,0xB5,
  0x45,0xAB,0x3E,0x5F,0x5F,0x9E,0x1B,0x3F,0xD9,0xBC,0x30,0x40,0xEA,0x1B,0xAE,0x08,0xBA,0xD7,0xD1,0x82,
  0xB0,0xE2,0x62,0x05,0xCA,0x54,0x3F,0x4F,0xB0,0xF0,0x97,0xF2,0xEF,0x95,0x7F,0x6E,0xC3,0xFF,0xC4,0xE4,
  0xC7,0xD3,0x5E,0x77,0x68,0xAE,0x53,0x4E,0x4A,0x7F,0x33,0x72,0xF1,0xD8,0xB0,0x94,0x45,0xC7,0x36,0x69,
  0x7B,0x19,0x82,0x92,0x62,0xAF,0x8C,0xB0,0xE4,0xCC,0xA8,0xD1,0x09,0x81,0xD1,0xF1,0x7E,0x11,0x81,0x32,
  0x5F,0x05,0x29,0x93,0x91,0x52,0xEB,0x3D,0x07,0xC8,0x7C,0x01,0x87,0xEB,0xB5,0x22,0x39,0x47,0x4E,0xBB,
  0xF4,0xE7,0xEB,0xBF,0x2B,0xBF,0x7F,0xD3,0x7A,0xC3,0x8D,0xA3,0xF3,0xE9,0x64,0x34,0x76,0xB4,0xBE,0x75,
  0xF9,0x93,0x33,0x75,0x65,0x2D,0xDA,0x36,0x7F,0xA9,0x72,0x42,0xFF,0xB4,0x37,0x46,0x2F,0x1D,0xE4,0x1F,
  0x49,0x49,0x21,0xDF,0x19,0xFF,0xD2,0xDA,0xF3,0xEF,0xD2,0xFB,0x5C,0x9C,0x30,0x7D,0xE3,0xA8,0x7C,0x27,
  0x96,0xE5,0x54,0x12,0x05,0x86,0xB2,0x2E,0x96,0xCC,0x4B,0xB2,0x32,0x65,0x8E,0x25,0xDB,0x28,0xB4,0xDE,
  0x30,0x58,0xEE,0xB7,0x2A,0x75,0x01,0x7D,0x52,0x24,0xF3,0xF0,0xBA,0x03,0xD5,0x27,0x1B,0x3B,0x5A,0xF5,
  0x46,0x43,0x8B,0x56,0x7D,0xA4,0xA6,0x64,0xDA,0xE1,0xB5,0x77,0xDB,0x9B,0x29,0x35,0x97,0x24,0x66,0xC5,
  0xFB,0x45,0x50,0x7F,0x74,0xF2,0xBA,0x9D,0x53,0xD6,0x3B,0xB7,0x60,0x4B,0x0F,0x8B,0x80,0xA3,0xAF,0xCA,
  0x1B,0x32,0x2D,0x44,0xEE,0x4F,0x29,0xBC,0xDA,0x52,0x73,0xB6,0xAE,0xCC,0x7A,0xC3,0x19,0xD1,0xE9,0x52,
  0x2F,0x6F,0x4A,0xE1,0xFE,0xEA,0xE3,0x74,0x0E,0xDA,0x74,0xED,0xBB,0xAE,0x1E,0xA1,0xA7,0x20,0x39,0x71,
  0xE3,0x9F,0xC6,0x25,0x7B,0x69,0x62,0x36,0xC1,0x53,0x71,0x2F,0xAC,0x88,0x45,0xA2,0x55,0x69,0xB9,0x4E,
  0xD8,0x06,0x41,0x10,0xC9,0x81,0x16,0xF6,0x8C,0xAB,0xB8,0x7F,0xCB,0x62,0xE5,0xCA,0x96,0x1A,0x4B,0x3D,
  0xC4,0x99,0xFF,0xF7,0x95,0xE1,0xCF,0xD1,0xEB,0x5C,0xB8,0x57,0xB9,0xBD,0xA2,0xF0,0x4A,0xF3,0xF7,0x0F,
  0x75,0x9A,0x27,0x53,0xF8,0xBB,0x7E,0xF6,0xEB,0xE7,0x07,0x67,0x02,0x0E,0x66,0x95,0x1D,0x33,0x76,0xA0,
  0x5F,0x24,0xA5,0xF0,0x47,0x4D,0xE3,0xE1,0x9A,0xF3,0x36,0xDB,0x9A,0x4F,0x7C,0xD9,0x94,0xC5,0x41,0xA6,
  0xAF,0xE4,0xA7,0x1E,0x06,0xF9,0x47,0xC6,0xD0,0xD6,0x78,0xAB,0x5A,0x6F,0xCF,0x3D,0xFA,0xAA,0xC1,0x64,
  0xA4,0x94,0xD3,0xAD,0x8E,0x03,0xCF,0x21,0xE7,0x8E,0x78,0x4D,0x9A,0x85,0x89,0xAF,0x9D,0x15,0x5F,0xD2,
  0xEF,0x07,0x5D,0x0F,0x69,0xCB,0x66,0x04,0x41,0x0C,0x0D,0x8E,0xB3,0x58,0x39,0x35,0x68,0xA0,0x85,0x1E,
  0x3A,0x7F,0xEA,0xC1,0x62,0x9E,0xF1,0x6D,0xE3,0x35,0xFA,0x91,0x04,0xC9,0xFC,0xE8,0x33,0xB9,0x80,0xC3,
  0xC5,0x1A,0xD3,0x2F,0x31,0x3D,0x2C,0x89,0x52,0xD8,0xDA,0xA9,0xDE,0x5F,0x6D,0xD7,0xEE,0x80,0x75,0x9A,
  0x66,0x7A,0xE1,0x92,0xC4,0x6C,0xFA,0x5B,0x83,0xFE,0x52,0x65,0xC1,0xD0,0x79,0xF4,0xCA,0x77,0xCC,0xB2,
  0x13,0x89,0x97,0x05,0xDF,0x1D,0x19,0x3A,0x44,0x2C,0x12,0x51,0x06,0xCF,0x3B,0xA6,0xBC,0x2C,0xB7,0x35,
  0xAC,0x05,0x1C,0x7D,0xB6,0x0D,0x4B,0xF3,0xE5,0x7B,0xAA,0x8A,0xB5,0xF6,0xAD,0x80,0x5B,0x9C,0xAE,0x0E,
  0x90,0xFA,0x9E,0x9A,0xFF,0x6E,0xDE,0x90,0x69,0x61,0x3E,0x81,0xA4,0xD8,0x2B,0x50,0xA6,0x9A,0x1B,0x37,
  0xF1,0xD4,0xFC,0xF7,0xE8,0x13,0xDE,0x04,0x41,0x9C,0x36,0xEB,0xA1,0xA1,0xE3,0x01,0xBD,0xC2,0xF0,0x90,
  0xC1,0x1F,0x65,0xBE,0x9E,0x1A,0x3C,0x50,0x4E,0x4A,0x55,0x12,0x45,0x56,0x74,0xFA,0x89,0x79,0x7F,0xC9,
  0x8E,0x19,0xCB,0xF3,0x84,0xB4,0xBB,0xA1,0xDD,0xB3,0x33,0xE8,0xF1,0x7E,0x11,0x33,0x63,0xA8,0x93,0xFD,
  0x5A,0xBD,0x6E,0x77,0x55,0xB1,0x9D,0x3D,0xFC,0xA8,0x69,0x3C,0x5D,0x57,0x9A,0x19,0x35,0x8A,0x52,0x1E,
  0xA1,0x0C,0xF9,0x60,0xEA,0x06,0x9B,0xCD,0x2B,0x5B,0x6E,0x95,0x36,0x55,0x9B,0xA5,0x17,0x35,0xAD,0x9D,
  0x6A,0xCA,0xA4,0x38,0x41,0x10,0xF3,0xE3,0x27,0xCD,0x8F,0x9F,0xC4,0xDA,0x5B,0x6E,0xE7,0xDC,0x39,0xC7,
  0x9C,0x63,0x75,0xDA,0x02,0x8A,0x63,0x13,0x04,0xB1,0xAF,0xFA,0xF8,0x83,0xCE,0x47,0xF6,0x77,0xF2,0xEA,
  0x85,0xF7,0xDB,0x9D,0x7A,0x17,0xAB,0xCB,0xA8,0x5F,0xFF,0xF5,0x0E,0xF3,0x2C,0xD5,0x68,0x32,0xD1,0x87,
  0xBB,0x16,0xD5,0xA2,0x55,0x9F,0xA9,0x2B,0x45,0x58,0x61,0x4A,0xC1,0x72,0xBF,0x3C,0xDA,0x50,0x50,0x6F,
  0x34,0xEC,0xAC,0xF8,0xD2,0xA1,0x7E,0x6A,0xD4,0xF5,0xF3,0x8E,0xBE,0x66,0x31,0x33,0xB5,0x22,0xAD,0x5E,
  0xB7,0xF8,0xF8,0xE6,0xCB,0x4D,0x37,0x28,0xE5,0xDB,0xCA,0x0B,0x2F,0x36,0x54,0xD9,0x6E,0x7B,0x62,0x73,
  0xF3,0xE3,0x87,0x80,0x83,0x29,0xAD,0x4C,0x99,0x4B,0x5F,0xAA,0x38,0x5C,0x53,0x72,0x47,0xD3,0xE4,0x68,
  0x57,0xA5,0xCD,0xD5,0x63,0xFE,0xB6,0xE2,0xD3,0xEA,0x13,0x5D,0x96,0x16,0xD2,0x28,0x32,0x98,0x8C,0xC5,
  0xB7,0x2F,0x8C,0xF9,0xFB,0x0A,0x8B,0xF9,0x8A,0xCE,0xD0,0xBD,0xE8,0xD8,0xA6,0x23,0x35,0x25,0xBD,0x35,
  0xAF,0x7D,0xD4,0x30,0xB3,0x68,0xC3,0xA5,0xC6,0xEB,0x3C,0xCF,0x39,0x3C,0x98,0x76,0xC8,0x48,0xE9,0x8A,
  0xE4,0xD9,0xF4,0x72,0x9B,0xCB,0x6C,0xBD,0xE9,0xBE,0xB6,0x6D,0xF5,0xB9,0xAD,0x6F,0x5C,0xDC,0x3D,0x3B,
  0x76,0x5C,0x7A,0xBF,0xA4,0xA4,0xA0,0xD8,0x48,0x45,0xC8,0x93,0xBF,0x95,0xD5,0x74,0x69,0x9B,0x1E,0xB7,
  0x5E,0x7F,0xF0,0x43,0x59,0xF3,0x7F,0x8B,0x6A,0xBE,0x6E,0xD6,0x5A,0x7B,0xE8,0x35,0x5D,0x8F,0x97,0x9D,
  0xFA,0x5D,0xC6,0xD5,0xA2,0xFC,0x84,0x19,0x19,0xE1,0xC9,0xE1,0x8A,0x20,0x11,0x21,0x6A,0xE8,0x68,0xFD,
  0xBE,0xAD,0xAE,0xF0,0xE6,0xD9,0x7F,0xD4,0x7E,0xA3,0x33,0x74,0x73,0x31,0xE1,0x20,0x1C,0x7A,0x9F,0xE3,
  0xFF,0x03,0x33,0x96,0xBD,0xD5,0x01,0x31,0x07,0x07,0xB6,0x60,0x00,0x19,0xAE,0x83,0xC3,0xD1,0x1F,0x80,
  0xB8,0x2B,0x38,0x07,0xE4,0x52,0x38,0x60,0x1E,0x42,0x88,0x29,0x70,0x0E,0x90,0xC1,0x00,0x1C,0x30,0x0F,
  0x84,0x15,0xF0,0x21,0x5C,0xDB,0x40,0x58,0x01,0x19,0x8C,0xC1,0x01,0xF3,0x40,0x58,0x01,0x1F,0x42,0xB4,
  0x0D,0xD7,0x84,0x15,0xF0,0xC1,0x4B,0x32,0x5C,0x96,0x73,0x80,0x0F,0xFE,0x91,0xE1,0xCA,0x84,0x14,0x7C,
  0xF0,0x8C,0x0C,0x17,0x8F,0x56,0xC0,0x07,0x9F,0xC8,0x70,0xFD,0x50,0x16,0x7C,0x78,0x16,0x0B,0xD7,0x5E,
  0x7F,0x31,0xFB,0x0F,0x11,0xF2,0xD4,0x63,0x29,0xE6,0xD0,0xB1,0x42,0x6E,0x7E,0x1A,0xC5,0x5C,0x3C,0x68,
  0xC8,0x3D,0x0F,0x21,0xE9,0x9E,0xA3,0xC7,0xCB,0x85,0x5C,0x34,0x66,0xD2,0xCD,0x27,0x03,0x4A,0x38,0x14,
  0xAF,0x49,0x0F,0x9E,0x1B,0x40,0x61,0x79,0x02,0x47,0x0A,0xF0,0x9C,0x21,0xCF,0x27,0xA4,0x10,0xE0,0x80,
  0x00,0x07,0x04,0x38,0x20,0x08,0x70,0x40,0x80,0x03,0x02,0x1C,0x10,0xE0,0x80,0x00,0x07,0x04,0x38,0x20,
  0xC0,0x01,0x01,0x0E,0x88,0x77,0xFA,0x1F,0x0C,0xF7,0x3F,0x34,0x5A,0x3B,0x7E,0xBF,0x00,0x00,0x00,0x00,
  0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
};

const uint8_t PWA_ICON_512[] PROGMEM = {
  0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x02,0x00,
  0x00,0x00,0x02,0x00,0x08,0x02,0x00,0x00,0x00,0x7B,0x1A,0x43,0xAD,0x00,0x00,0x23,0x73,0x49,0x44,0x41,
  0x54,0x78,0xDA,0xED,0xDD,0x79,0x78,0x54,0xD5,0xFD,0xC7,0xF1,0x3B,0x93,0x99,0x4C,0xF6,0x7D,0x21,0x09,
  0x09,0x84,0x25,0x24,0x90,0xB0,0x83,0x10,0x10,0x45,0x76,0x94,0x4D,0x50,0x11,0x2B,0x62,0x5B,0x34,0x5A,
  0x94,0xD6,0x2E,0x62,0xDB,0x1F,0x0F,0x55,0x69,0xA5,0x8A,0xA5,0x5A,0x2D,0x16,0x6A,0xAD,0x0A,0xA2,0x82,
  0x5A,0xF6,0x55,0x44,0x65,0x0F,0x3B,0x21,0x09,0x5B,0x58,0x42,0x36,0x42,0x92,0xC9,0x36,0x99,0x4C,0x26,
  0xF9,0xFD,0xE1,0xF3,0x50,0x8A,0x48,0x26,0xE7,0xCE,0x7E,0xDE,0xAF,0x3F,0x35,0x97,0x39,0x73,0x97,0xEF,
  0xE7,0x9C,0x73,0xEF,0x9C,0xAB,0x09,0x98,0x9F,0xA9,0x00,0x00,0xE4,0xA3,0x65,0x17,0x00,0x00,0x01,0x00,
  0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x40,
  0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x01,0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,0x80,0x00,0x00,
  0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x40,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x01,0x00,0x00,0x20,
  0x00,0x00,0x00,0x04,0x00,0x00,0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x40,0x00,0x00,
  0x00,0x01,0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,
  0x00,0x00,0x40,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x01,0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,
  0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x40,0x00,0x00,0x00,0x08,0x00,0x00,0x80,0x1D,
  0xE9,0xD8,0x05,0xCE,0xA7,0x8F,0x0B,0x62,0x27,0x00,0xDF,0x67,0x29,0xAE,0x65,0x27,0x10,0x00,0xD4,0x7A,
  0x80,0xEB,0x85,0x54,0x20,0x00,0xA8,0xF8,0x00,0xD7,0x14,0x61,0x40,0x00,0x50,0xF4,0x01,0xAE,0x35,0xC2,
  0x80,0x00,0xA0,0xEE,0x03,0x5C,0x80,0x84,0x01,0x01,0x40,0xDD,0x07,0xB8,0x2A,0xE3,0x82,0x88,0x01,0x02,
  0x80,0xD2,0x0F,0x70,0x85,0x92,0x04,0x04,0x00,0x75,0x1F,0x20,0x09,0x48,0x02,0x02,0x80,0xD2,0x0F,0xC8,
  0x7C,0x09,0x13,0x03,0x04,0x00,0xA5,0x1F,0x20,0x06,0x40,0x00,0x50,0xFA,0x01,0x62,0x00,0x04,0x00,0xA5,
  0x1F,0x20,0x06,0x08,0x00,0xCE,0x09,0x00,0xC4,0x80,0x5C,0x58,0x0D,0x94,0xEA,0x0F,0x70,0xED,0x33,0x02,
  0xE0,0xF0,0x03,0x60,0x28,0xC0,0x08,0x40,0x86,0xA3,0x4E,0xF5,0x07,0x20,0x79,0x5F,0x50,0xCB,0xC1,0x06,
  0x00,0x39,0x3B,0x85,0x5A,0x09,0x0F,0x33,0xE7,0x3A,0x00,0xEA,0x83,0x22,0xD5,0x3D,0x00,0x4A,0x3F,0x00,
  0x1B,0x0B,0x85,0x24,0x77,0x05,0xB4,0x52,0x1D,0x54,0x00,0xA0,0x62,0xC8,0x15,0x00,0x54,0x7F,0x00,0xD4,
  0x0D,0xE9,0x02,0x80,0xA7,0x7D,0x00,0x90,0x01,0x32,0x06,0x00,0xA5,0x1F,0x00,0x9D,0x48,0x19,0x03,0x80,
  0xEA,0x0F,0x80,0x7A,0x22,0x63,0x00,0x50,0xFD,0x01,0x50,0x55,0x64,0x0C,0x00,0xAA,0x3F,0x00,0x6A,0x8B,
  0x8C,0x01,0x40,0xF5,0x07,0x40,0x85,0x91,0x31,0x00,0xA8,0xFE,0x00,0xA8,0x33,0x32,0x06,0x00,0xD5,0x1F,
  0x00,0xD5,0x46,0xC6,0x00,0xA0,0xFA,0x03,0xA0,0xE6,0xC8,0x18,0x00,0x54,0x7F,0x00,0x54,0x1E,0x19,0x03,
  0x80,0xEA,0x0F,0x80,0xFA,0x23,0x63,0x00,0x50,0xFD,0x01,0x50,0x85,0x64,0x0C,0x00,0xAA,0x3F,0x00,0xC8,
  0x3B,0x02,0x00,0x00,0x3A,0xA3,0xD2,0x05,0x00,0xDD,0x7F,0x00,0x54,0x24,0x19,0x03,0x80,0xEA,0x0F,0x80,
  0xBA,0x24,0x63,0x00,0x50,0xFD,0x01,0x50,0x9D,0xE4,0x1D,0x01,0x00,0x00,0xA4,0x0B,0x00,0xBA,0xFF,0x00,
  0xA8,0x51,0x32,0x06,0x00,0xD5,0x1F,0x00,0x95,0x4A,0xC6,0x00,0xA0,0xFA,0x03,0x80,0xBC,0x23,0x00,0x00,
  0xA0,0xC3,0x2A,0x5D,0x00,0xD0,0xFD,0x07,0x40,0x06,0x30,0x02,0x00,0x00,0x48,0x13,0x00,0x74,0xFF,0x01,
  0x30,0x08,0x90,0x31,0x00,0xA8,0xFE,0x00,0xC8,0x00,0x79,0x47,0x00,0x00,0x00,0xE9,0x02,0x80,0xEE,0x3F,
  0x00,0x06,0x01,0x8C,0x00,0x00,0x00,0xD2,0x04,0x00,0xDD,0x7F,0x00,0x0C,0x02,0x1C,0x4A,0xC7,0xB1,0x81,
  0x5D,0xEC,0xCB,0x5A,0x2A,0xB6,0xE1,0xA0,0xA5,0x59,0xEC,0x3D,0xC0,0x25,0x34,0x01,0xF3,0x33,0x09,0x4C,
  0x38,0xA7,0xD6,0x93,0x0A,0x90,0x99,0xA5,0xB8,0x96,0x11,0x00,0xA8,0xF8,0x6D,0xFE,0x44,0xF2,0x00,0x90,
  0x65,0x04,0x40,0xF7,0x5F,0xDA,0xBA,0xCF,0xC8,0x00,0x0C,0x02,0x18,0x01,0x80,0xA2,0xDF,0x7A,0xF3,0x08,
  0x03,0xC0,0xDB,0x46,0x00,0x74,0xFF,0x29,0xFD,0x8C,0x09,0xC0,0x20,0x80,0x11,0x00,0xA8,0xFB,0x6D,0x68,
  0x3F,0x49,0x00,0x10,0x00,0x90,0xA5,0xF4,0xDF,0xF2,0xEB,0x10,0x03,0x80,0xED,0xDC,0x6B,0x0A,0x88,0xF9,
  0x1F,0xEA,0xBE,0xBD,0x90,0x04,0x70,0x5B,0xEE,0x33,0x0B,0xC4,0x08,0x80,0xD2,0xEF,0xCD,0x5F,0x96,0x18,
  0x00,0x3C,0x63,0x04,0x40,0xF7,0x9F,0xD2,0xCF,0x68,0x00,0x0C,0x02,0x9C,0x89,0xC5,0xE0,0xA8,0xFE,0xEC,
  0x01,0x40,0x52,0x4C,0x01,0x51,0xF8,0x24,0xDA,0x15,0x0C,0x05,0x80,0x1B,0xB9,0xCB,0x14,0x10,0xF3,0x3F,
  0x94,0x7E,0xA7,0x21,0x06,0xE0,0x0E,0xDC,0x61,0x16,0x88,0x29,0x20,0xAA,0x3F,0xFB,0x07,0x90,0x14,0x53,
  0x40,0x94,0x36,0x79,0x77,0x94,0x33,0x87,0x02,0x55,0x59,0x5B,0xC2,0x96,0x8E,0x61,0xCF,0xC3,0xAD,0xB8,
  0xC5,0x08,0x80,0xF9,0x1F,0xAA,0xBF,0x77,0xEF,0xB1,0xAA,0xAC,0x2D,0xEC,0x6D,0xB8,0x61,0xDD,0x63,0x04,
  0x40,0x21,0x63,0xD7,0x39,0x76,0x28,0xF0,0x5D,0xF5,0xA7,0xFB,0x0F,0x46,0x00,0xA0,0xFA,0xCB,0xB5,0x0F,
  0xE9,0xFB,0x83,0x00,0x00,0xD5,0x5F,0xC6,0x3D,0x49,0xF5,0x87,0x9B,0x73,0xFD,0x63,0xA0,0xDC,0x00,0xA0,
  0xF4,0xBB,0x15,0x7B,0x4D,0x07,0xDD,0x58,0xFD,0x99,0xFF,0xC1,0x0F,0x71,0xED,0xC3,0xA0,0x8C,0x00,0xA8,
  0xFE,0xB0,0xFF,0xBE,0xA5,0xEF,0x0F,0x8F,0x40,0x00,0x50,0xFD,0x61,0xE7,0x3D,0x4C,0xF5,0x07,0x01,0x00,
  0xAA,0x3F,0xFB,0x59,0x51,0x98,0xFF,0x01,0x01,0x00,0xAA,0xBF,0x24,0x7B,0x9B,0xEE,0x3F,0x08,0x00,0x5B,
  0x71,0x07,0x98,0xEA,0xEF,0x4D,0xFB,0x9C,0xEA,0x0F,0xCF,0xAA,0x81,0x8C,0x00,0xA8,0xFE,0xB0,0xCF,0x9E,
  0xA7,0xFA,0x83,0x11,0x00,0xA8,0xFE,0x32,0xEE,0xFF,0x1F,0xAA,0xFE,0xDC,0x00,0x00,0x01,0x00,0xAA,0xBF,
  0x37,0x1F,0x05,0xFA,0xFE,0x20,0x00,0x40,0xF5,0x97,0xF1,0x58,0x50,0xFD,0x41,0x00,0x80,0xEA,0x2F,0xE3,
  0x11,0xB9,0x7D,0xF5,0x67,0xFE,0x07,0x04,0x00,0xA8,0xFE,0xDE,0x79,0x5C,0xE8,0xFB,0x83,0x00,0x10,0xC7,
  0x33,0xA0,0x54,0x7F,0xCF,0x3D,0x3A,0x54,0x7F,0x78,0x41,0x25,0x64,0x04,0x40,0xF5,0x47,0x9B,0xD9,0x52,
  0xFD,0x99,0xFF,0x01,0x23,0x00,0x50,0xFD,0xBD,0x4D,0xAA,0x92,0xCC,0x4E,0x00,0x01,0x00,0x50,0xFD,0x01,
  0x02,0x00,0x74,0xFF,0xA9,0xFE,0x37,0x60,0xFE,0x07,0x04,0x00,0xA8,0xFE,0x00,0x08,0x00,0x50,0xFD,0xA5,
  0xE9,0xFE,0x03,0x04,0x00,0x20,0x69,0xF5,0x67,0xFE,0x07,0x04,0x00,0xE8,0xFE,0xD3,0xF7,0x07,0x08,0x00,
  0x50,0xFD,0xA9,0xFE,0x00,0x01,0x00,0x50,0xFD,0x01,0x02,0x00,0x74,0xFF,0xBD,0xB0,0xFA,0x73,0x03,0x00,
  0x04,0x00,0xA8,0xFE,0xF4,0xFD,0x01,0x02,0x00,0xA0,0xFA,0x03,0x04,0x00,0xE8,0xFE,0x7B,0x77,0xF5,0xCF,
  0x53,0x0A,0x38,0xB2,0x20,0x00,0x40,0xF5,0x97,0xB7,0xEF,0xCF,0xF1,0x05,0x01,0x00,0xC8,0x58,0xFD,0x01,
  0x02,0x00,0x74,0x0F,0xA5,0xAB,0xFE,0x79,0x4A,0x01,0x47,0x19,0x04,0x00,0x40,0xDF,0x1F,0x20,0x00,0x40,
  0xF7,0x9F,0x63,0x0D,0x10,0x00,0x80,0x17,0x77,0xFF,0x6F,0x9C,0xFF,0x01,0x08,0x00,0xD0,0x25,0x94,0xA5,
  0xFA,0x73,0xC4,0x41,0x00,0x00,0x54,0x7F,0x80,0x00,0x00,0x9D,0x41,0x99,0xAA,0xFF,0x6D,0xE6,0x7F,0x38,
  0xEE,0x20,0x00,0x00,0xFA,0xFE,0x00,0x01,0x00,0x50,0xFD,0x01,0x02,0x40,0x1E,0xCC,0x03,0xC8,0x5C,0xFD,
  0x39,0xFA,0x20,0x00,0x00,0xEF,0xAC,0xFE,0x3C,0x00,0x0A,0x02,0x00,0x74,0x00,0xE9,0xFB,0x73,0x0E,0x80,
  0x00,0x00,0xA8,0xFE,0x00,0x01,0x00,0x78,0x77,0xF5,0x67,0xFE,0x07,0x04,0x00,0x18,0xFB,0xD3,0xF7,0xE7,
  0x4C,0x00,0x01,0x00,0x50,0xFD,0x01,0x02,0x00,0xF0,0xEE,0xEA,0xCF,0xFC,0x0F,0x08,0x00,0x30,0xEA,0x07,
  0xE7,0x03,0x08,0x00,0x40,0x9A,0xEE,0x3F,0x40,0x00,0x00,0x92,0x56,0x7F,0xE6,0x7F,0x40,0x00,0x80,0xF1,
  0x3E,0x7D,0x7F,0xCE,0x0A,0x10,0x00,0x00,0xD5,0x1F,0x20,0x00,0x00,0xEF,0xAE,0xFE,0xCC,0xFF,0x80,0x00,
  0x00,0xE8,0xFB,0x03,0x04,0x00,0xFE,0x17,0x53,0xBD,0x54,0x7F,0xCE,0x0D,0x10,0x00,0x00,0xD5,0x1F,0x20,
  0x00,0x00,0xAF,0xA8,0xFE,0xDC,0x00,0x00,0x01,0x00,0xD0,0xF7,0x07,0x08,0x00,0x80,0xEA,0x0F,0x10,0x00,
  0x50,0xB8,0xCB,0xE7,0xD5,0xD5,0xDF,0x2E,0xF3,0x3F,0x9C,0x21,0x20,0x00,0x00,0xFA,0xFE,0x00,0x01,0x00,
  0x00,0x20,0x00,0x00,0xB7,0xED,0xFE,0xF3,0xFC,0x0F,0x08,0x00,0x40,0xC6,0xEA,0x0F,0x10,0x00,0x00,0xD5,
  0x1F,0x20,0x00,0x00,0x99,0xAA,0x3F,0xF3,0x3F,0x20,0x00,0xD0,0x0A,0x9E,0xF0,0xA3,0xEF,0xCF,0x79,0x02,
  0x02,0x00,0xA0,0xFA,0x03,0x04,0x00,0xE0,0xE1,0xD5,0x9F,0xF9,0x1F,0x10,0x00,0x00,0x7D,0x7F,0x80,0x00,
  0x00,0xA8,0xFE,0x00,0x01,0x00,0x50,0xFD,0x01,0x02,0x00,0xF0,0xC2,0xEA,0xCF,0x0D,0x00,0x10,0x00,0x00,
  0x7D,0x7F,0x80,0x00,0x00,0xA8,0xFE,0x00,0x01,0x00,0x78,0x37,0xE6,0x7F,0x40,0x00,0x00,0x74,0xFF,0x01,
  0x02,0x00,0xA0,0xFA,0x03,0x04,0x00,0xE0,0xDD,0xD5,0x9F,0xF9,0x1F,0x10,0x00,0x00,0x7D,0x7F,0x80,0x00,
  0x00,0xA8,0xFE,0x00,0x01,0x00,0x78,0x77,0xF5,0x67,0xFE,0x07,0x04,0x00,0x40,0xDF,0x1F,0x20,0x00,0x00,
  0xAA,0x3F,0x40,0x00,0x00,0xDE,0x5D,0xFD,0x99,0xFF,0x01,0x01,0x00,0xD0,0xF7,0x07,0x08,0x00,0xD8,0x66,
  0xD0,0xD2,0x2C,0x76,0x02,0xD5,0x9F,0xF3,0x04,0x04,0x00,0x40,0xF5,0x07,0x08,0x00,0x50,0xFD,0x3D,0x07,
  0x37,0x00,0x40,0x00,0x00,0x00,0x08,0x00,0x40,0x9A,0xEE,0x3F,0x40,0x00,0x00,0x92,0x56,0x7F,0xE6,0x7F,
  0x40,0x00,0x00,0xF4,0xFD,0x01,0x02,0x00,0x6D,0xC4,0x13,0x7E,0x54,0x7F,0xCE,0x10,0x10,0x00,0xA0,0xFA,
  0x7B,0x18,0xE6,0x7F,0x40,0x00,0x00,0xF4,0xFD,0x01,0x02,0x00,0xA0,0xFA,0x03,0x04,0x00,0xE0,0xDD,0xD5,
  0x9F,0xF9,0x1F,0x10,0x00,0xB0,0x03,0x39,0xEF,0xF2,0xD1,0xF7,0xE7,0xDC,0x00,0x01,0x00,0xFA,0xFE,0x00,
  0x08,0x00,0x50,0xFD,0x3D,0x04,0xF3,0x3F,0x20,0x00,0x00,0xFA,0xFE,0x00,0x01,0x00,0x75,0x24,0x99,0xEA,
  0xA5,0xFA,0x73,0x56,0x80,0x00,0x00,0x7D,0x7F,0x00,0x04,0x00,0xE0,0x69,0xB8,0x01,0x00,0x02,0x00,0x8C,
  0xF7,0xE9,0xFE,0x73,0x3E,0x80,0x00,0x00,0xA8,0xFE,0x00,0x01,0x00,0x78,0x77,0xF5,0x67,0xFE,0x07,0x04,
  0x00,0x18,0xF5,0xD3,0xF7,0xE7,0x4C,0x00,0x01,0x00,0x50,0xFD,0x01,0x02,0x00,0xF0,0xEE,0xEA,0xCF,0xFC,
  0x0F,0x08,0x00,0x30,0xF6,0xA7,0xEF,0xCF,0x39,0x00,0x02,0x00,0xA0,0xFA,0x03,0x04,0x00,0xE8,0x00,0x7A,
  0x77,0xF5,0x77,0xCE,0xFC,0x0F,0xDD,0x7F,0x10,0x00,0xA0,0xFA,0x03,0x20,0x00,0x40,0xF5,0x07,0x40,0x00,
  0xC8,0xC3,0xE3,0xE6,0x01,0xBC,0xBE,0xFA,0x33,0xFF,0x03,0x10,0x00,0xA0,0xEF,0x0F,0x10,0x00,0xA0,0x33,
  0x08,0x8E,0x38,0x08,0x00,0xD0,0xFD,0x07,0x40,0x00,0x40,0xAE,0x2E,0xA1,0x24,0xD5,0xDF,0x09,0x37,0x00,
  0xE8,0xFE,0x83,0x00,0x00,0x7D,0x7F,0x00,0x04,0x00,0xDC,0xBB,0x63,0x48,0xF5,0xA7,0xFB,0x0F,0x02,0x00,
  0xF4,0xFD,0xBD,0x1C,0x0B,0xC0,0x01,0x04,0x00,0xDD,0x43,0xFA,0xFE,0x74,0xFF,0x41,0x00,0x40,0xE2,0x1A,
  0x41,0xF5,0xA7,0xFA,0x83,0x00,0x80,0x8C,0x24,0xAC,0xFE,0xCC,0xFF,0x00,0x04,0x00,0x5D,0x45,0xFA,0xFE,
  0x74,0xFF,0x41,0x00,0x40,0xCA,0x7A,0x41,0xF5,0xA7,0xFA,0x03,0x04,0x80,0x8C,0xA4,0xAD,0xFE,0xCC,0xFF,
  0x00,0x04,0x80,0xD4,0xDD,0x46,0xFA,0xFE,0x74,0xFF,0x01,0x02,0x40,0xC6,0xDA,0x41,0xF5,0xA7,0xFA,0x03,
  0x04,0x00,0xA4,0xC3,0xFC,0x0F,0x40,0x00,0xC8,0xDB,0x85,0xA4,0xFB,0x4F,0xF7,0x1F,0x20,0x00,0x64,0xAC,
  0x23,0x54,0x7F,0xAA,0x3F,0x40,0x00,0xC8,0x58,0x4D,0xA8,0xFE,0x54,0x7F,0x80,0x00,0x90,0x11,0xD5,0x5F,
  0xE1,0x06,0x00,0x40,0x00,0x48,0xD8,0xA9,0xA4,0xFA,0xD3,0xFD,0x07,0x08,0x00,0x19,0x2B,0x0B,0xD5,0x9F,
  0xEA,0x0F,0x10,0x00,0x32,0xD6,0x17,0xAA,0xFF,0x75,0xF6,0x9D,0xFF,0xA1,0xFA,0x83,0x00,0x80,0xBB,0x67,
  0x40,0x9E,0x52,0xC0,0xC4,0x37,0x7D,0x7F,0x80,0x00,0x90,0xB7,0xD6,0x10,0x03,0x54,0x7F,0x80,0x00,0x90,
  0xBA,0xE2,0xC8,0x1C,0x03,0xF6,0xFA,0xE2,0x54,0x7F,0x10,0x00,0xF0,0xE0,0x5E,0xE7,0x77,0x31,0xC0,0x80,
  0x80,0xEA,0x0F,0x10,0x00,0xF2,0x56,0x1F,0x62,0x80,0xEA,0x0F,0x10,0x00,0x52,0xD7,0x20,0x19,0x62,0x40,
  0xFD,0x17,0xA4,0xFA,0x83,0x00,0x80,0xD7,0xF6,0x43,0x19,0x0D,0x50,0xFD,0x41,0x00,0x40,0xEA,0x7A,0x44,
  0x0C,0x50,0xFD,0x41,0x00,0x40,0xEA,0xAA,0xE4,0x65,0x31,0xA0,0xE6,0xBB,0x50,0xFD,0x41,0x00,0x40,0xC6,
  0x9E,0x29,0xA3,0x01,0xAA,0x3F,0x08,0x00,0x48,0x5D,0xA1,0xA4,0x7D,0x66,0x94,0xEA,0x0F,0x79,0xE8,0xD8,
  0x05,0x1E,0x5D,0xA7,0xF6,0x65,0x2D,0x75,0x42,0x12,0x28,0x72,0xAC,0x2C,0x44,0xE9,0x07,0x23,0x00,0x50,
  0xB3,0x7E,0x70,0x40,0xE0,0x41,0x7B,0xA6,0xAD,0xAD,0xA5,0xFA,0x83,0x00,0x00,0x19,0xE0,0x55,0x31,0x40,
  0xF5,0x07,0x6E,0x83,0x29,0x20,0xEF,0xA9,0x5F,0x4E,0x98,0x0E,0xBA,0xB1,0x73,0xED,0x1D,0x93,0x42,0x94,
  0x7E,0x30,0x02,0x00,0xDD,0x58,0xAF,0x1A,0x0D,0xD8,0xD8,0x30,0xAA,0x3F,0x18,0x01,0x80,0xA1,0x80,0x74,
  0xA3,0x01,0x4A,0x3F,0xC0,0x08,0x80,0xA1,0x80,0x3D,0x47,0x03,0x9E,0x72,0x7B,0x80,0xEA,0x0F,0x7C,0x47,
  0x13,0x30,0x3F,0xD3,0x55,0x9F,0xAD,0x8F,0x0B,0xE2,0x00,0x38,0x94,0x33,0x87,0x02,0x37,0x72,0xED,0x80,
  0xE0,0x36,0x39,0x44,0xE9,0x87,0x7B,0xB2,0x14,0xD7,0xBA,0xE4,0x73,0x99,0x02,0xF2,0xFE,0xA1,0x80,0xF3,
  0x63,0xC0,0x0D,0xE7,0x85,0x28,0xFD,0xC0,0xF7,0x31,0x05,0x24,0x4B,0x0C,0xB8,0xA4,0x27,0xEE,0x26,0x93,
  0x42,0x54,0x7F,0xE0,0x96,0x98,0x02,0x92,0x88,0xAB,0x66,0x84,0x9C,0x39,0x1A,0xB8,0x29,0x72,0x28,0xFD,
  0xF0,0x08,0x4C,0x01,0xC1,0x49,0x1D,0x61,0x97,0xC4,0x80,0xF3,0x27,0x85,0x28,0xFD,0x00,0x23,0x00,0xB8,
  0xDD,0x80,0xC0,0xA1,0x31,0x10,0xB6,0x74,0x0C,0x47,0x16,0x8C,0x00,0x08,0x00,0x48,0x17,0x03,0x54,0x7F,
  0x10,0x00,0x6D,0xC2,0x14,0x90,0xEC,0xAE,0x4F,0x95,0x38,0x39,0x09,0xAE,0x4F,0xD6,0xCB,0xB0,0xCE,0x28,
  0xE0,0x9E,0x78,0x0A,0x08,0xFF,0x4D,0x02,0x97,0xCC,0x9B,0xF3,0xFE,0x19,0xC0,0x55,0x18,0x01,0xE0,0xD6,
  0x03,0x02,0x85,0x25,0x25,0x00,0x6F,0xC7,0x3D,0x00,0xB4,0xCE,0xC9,0xB3,0x43,0x62,0x31,0xC0,0x0D,0x00,
  0x78,0x2E,0xEE,0x01,0xC0,0x33,0x86,0x05,0x4E,0xC8,0x83,0x3C,0xA5,0x60,0xD0,0xD2,0xAC,0xAA,0xAC,0x2D,
  0xEC,0x79,0x80,0x00,0x80,0x5B,0xE7,0x81,0xCA,0x54,0xF8,0xA1,0x1B,0x0F,0xDF,0xF5,0xE8,0x89,0x01,0xC0,
  0x71,0x98,0x02,0x82,0x07,0x68,0x35,0x06,0x98,0xFF,0x81,0x47,0x63,0x0A,0x08,0x68,0xBD,0xBE,0x33,0x20,
  0x00,0xEC,0x88,0xC7,0x40,0xE1,0x61,0x49,0x40,0x67,0x1F,0x20,0x00,0x40,0x0C,0xDC,0x3C,0x3E,0x00,0x40,
  0x00,0x80,0xD1,0x00,0x00,0x02,0x00,0x12,0xC4,0x00,0x3B,0x01,0x20,0x00,0x00,0x00,0x04,0x00,0x00,0x80,
  0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x08,0x00,0x76,0x01,0x00,0x10,0x00,0x00,0x00,0x02,
  0x00,0x00,0x40,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x01,0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,
  0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x40,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x01,
  0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,
  0x40,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x01,0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,0x10,0x00,
  0x00,0x00,0x02,0x00,0x00,0x40,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x01,0x00,0x00,0x20,0x00,0x00,0x00,
  0x04,0x00,0x00,0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0xE0,0x56,0x74,0xEC,0x02,0x78,
  0x31,0x1F,0x8D,0xB6,0x6B,0x78,0x62,0x4A,0x58,0x62,0xC7,0x90,0xB8,0xE4,0x90,0xB8,0xB8,0xC0,0xC8,0x70,
  0x43,0x48,0x84,0x5F,0x48,0x98,0x21,0xC8,0xE0,0xA3,0xF7,0xD5,0xEA,0xF4,0x3E,0x7A,0x45,0x51,0x2C,0x56,
  0x4B,0x63,0x73,0x93,0xD9,0x6A,0xA9,0x32,0xD7,0x5E,0x6B,0x30,0x56,0x36,0x54,0x17,0xD5,0x5D,0xBB,0x50,
  0x5D,0x5C,0x50,0x5D,0x7C,0xBA,0xF2,0xD2,0x59,0x63,0x61,0x73,0x4B,0x0B,0x3B,0x13,0x04,0x00,0xE0,0xEE,
  0x7C,0xB5,0xBA,0x41,0x71,0xE9,0x23,0x13,0xFB,0xDF,0xD1,0xAE,0x47,0xCF,0xA8,0xCE,0xFE,0x3A,0x43,0xEB,
  0x39,0xA1,0x33,0xF8,0x29,0x06,0x45,0x51,0xA2,0xFD,0xC3,0xBA,0x2A,0xED,0x6F,0xFA,0xBF,0x75,0x16,0xD3,
  0xF1,0xF2,0x73,0x7B,0x8A,0x4F,0x6C,0xBF,0x9C,0x7D,0xB0,0x34,0xB7,0xA9,0xD9,0xCA,0x4E,0x86,0x77,0xD0,
  0x04,0xCC,0xCF,0x74,0xD5,0x67,0xEB,0xE3,0x82,0x38,0x00,0x6A,0xCC,0xEB,0xFF,0xE8,0xBC,0xFE,0x3F,0xF2,
  0xC4,0x96,0x47,0xFD,0x63,0xBC,0xDD,0xCB,0xA8,0x56,0xA3,0xB9,0x33,0xBE,0xF7,0x8C,0x6E,0xA3,0xEE,0x4B,
  0xCE,0x0C,0xD4,0xFB,0x3B,0xA8,0xE5,0x46,0x73,0xED,0x17,0xE7,0xBF,0xF9,0x28,0x7F,0xDB,0xBE,0x92,0x1C,
  0xCE,0x40,0xD8,0x8B,0xA5,0xB8,0x96,0x11,0x00,0x20,0xD4,0x93,0xD0,0xEA,0xA6,0xA7,0x8C,0xFC,0x45,0x9F,
  0x87,0x3A,0x85,0xC6,0x3B,0xFA,0xB3,0x42,0x0D,0x41,0x8F,0xA5,0x8D,0x7B,0x2C,0x6D,0xDC,0xA9,0x8A,0x82,
  0xC5,0x87,0x57,0x7D,0x7E,0x6E,0x17,0xB3,0x43,0xF0,0x5C,0xDC,0x04,0x86,0x67,0x1B,0xD3,0x61,0xE0,0xA1,
  0x87,0xDF,0x7D,0xF3,0xEE,0x5F,0x38,0xA1,0xFA,0xDF,0xA8,0x7B,0x44,0xF2,0x3F,0x47,0xBE,0xB0,0xE7,0x81,
  0x77,0x06,0xC7,0xA5,0x73,0x14,0x40,0x00,0x00,0x4E,0x15,0xE2,0x1B,0xF8,0xDE,0xA8,0xDF,0x7D,0x3C,0xEE,
  0xA5,0xA4,0xE0,0x58,0x57,0xB5,0x21,0x35,0xA2,0xC3,0xC6,0x49,0xAF,0x2D,0x19,0x36,0xD7,0xCF,0xC7,0x97,
  0x23,0x02,0x02,0x00,0x70,0x4A,0xE5,0x0D,0x4F,0xDA,0x39,0xF5,0xCD,0xC9,0x9D,0x87,0xB9,0xBC,0x25,0x1A,
  0x45,0x33,0xAB,0xFB,0xF8,0xCD,0x93,0x5F,0x4F,0x08,0x8A,0xE6,0xB8,0x80,0x00,0x00,0x1C,0xAB,0x47,0x64,
  0xF2,0xC6,0x49,0x8B,0x3B,0x87,0x26,0xB8,0x4F,0x93,0x7A,0x47,0x77,0xDD,0x3C,0xE9,0xF5,0xC4,0xE0,0x18,
  0x8E,0x0E,0x08,0x00,0xC0,0x51,0x92,0x43,0xE2,0xD6,0x4E,0x58,0x14,0xE1,0x17,0xE2,0x6E,0x0D,0x4B,0x0C,
  0x8E,0xD9,0x30,0xF1,0xB5,0x48,0xBF,0x50,0x8E,0x11,0x08,0x00,0xC0,0xFE,0xFC,0x7C,0x7C,0xFF,0x3D,0xFA,
  0xF7,0x6E,0x5B,0x64,0x93,0x82,0x63,0x97,0x8D,0x78,0x5E,0xAB,0xD1,0x70,0xA4,0x40,0x00,0x00,0x76,0xF6,
  0xC2,0x80,0x47,0x7B,0x46,0x75,0x71,0xE7,0x16,0xDE,0x93,0xD8,0xEF,0x89,0xF4,0x49,0x1C,0x29,0x10,0x00,
  0x80,0x3D,0x25,0x87,0xC4,0x3D,0xD5,0xF3,0x7E,0xF7,0x6F,0xE7,0xBC,0xFE,0x8F,0xBA,0xE1,0x0C,0x15,0x40,
  0x00,0xC0,0x83,0xFD,0xB2,0xEF,0xC3,0xBE,0x5A,0x0F,0xF8,0xE9,0x62,0x98,0x21,0x88,0x41,0x00,0x08,0x00,
  0xE0,0xD6,0x5A,0xDA,0xFE,0xEB,0xD9,0x50,0x43,0xD0,0xB4,0x2E,0x77,0x7B,0xCA,0x17,0x9C,0x95,0x36,0x4E,
  0xA7,0xF5,0xE1,0x40,0xC3,0xCD,0xB1,0x14,0x04,0x9C,0x6D,0x77,0xD1,0x71,0x6B,0x4B,0x73,0x5B,0xB7,0x9A,
  0x90,0x3C,0xC4,0xCF,0x86,0x65,0xDD,0x5A,0xD5,0xD8,0xDC,0xB4,0xA1,0x60,0xCF,0x37,0x57,0x8E,0x1E,0x2A,
  0xCB,0x2F,0xAD,0xAF,0xA8,0x32,0xD7,0x2A,0x1A,0x4D,0x98,0x21,0x28,0x2E,0x20,0x72,0x40,0x6C,0xEA,0xDD,
  0xED,0xFB,0x8E,0xE9,0x70,0x87,0x8F,0x46,0x6D,0xC7,0xA8,0x5D,0x60,0xE4,0x90,0xB8,0x9E,0xBB,0xAE,0x1C,
  0xE1,0x70,0x83,0x00,0x00,0xFE,0xEB,0xCD,0x63,0xAB,0x05,0xB6,0x1A,0x91,0xD8,0x5F,0xFD,0x47,0x2F,0xCF,
  0x59,0xF7,0xE7,0xEC,0x15,0x65,0xA6,0xCA,0x9B,0xFE,0x7B,0x49,0x93,0xB9,0xA4,0xEE,0xDA,0x91,0xAB,0xA7,
  0xFF,0x71,0x72,0x6D,0x52,0x70,0xEC,0x1F,0x06,0xFD,0x74,0x8A,0xEA,0x9F,0x98,0xDD,0x93,0xD8,0x97,0x00,
  0x00,0x01,0x00,0xFC,0x57,0x7E,0xE5,0xA5,0x2D,0x17,0x0F,0x08,0x6C,0x98,0x19,0x97,0xA1,0xB2,0xE3,0xFF,
  0xD8,0xD6,0x97,0x37,0x5D,0xD8,0xDB,0xEA,0x5F,0x5E,0xAA,0x29,0x7D,0x7C,0xDB,0xC2,0x3D,0xC5,0x27,0x5E,
  0x1D,0xFA,0x33,0x35,0x9F,0xA8,0xB2,0xC1,0x80,0x13,0x70,0x0F,0x00,0x4E,0xF5,0xB7,0x63,0x6B,0x5A,0x14,
  0x91,0x1B,0x00,0xB1,0x01,0xE1,0x6A,0x3E,0x77,0xCE,0xCE,0xC5,0xB6,0x54,0xFF,0xEB,0x96,0x9D,0x5C,0xBB,
  0xE8,0xD0,0x0A,0x35,0x9F,0x98,0x12,0x96,0xC8,0xE1,0x06,0x23,0x00,0x38,0xCA,0x2B,0xD9,0x1F,0xBC,0x92,
  0xFD,0x81,0x93,0x3F,0x74,0x76,0xFA,0x44,0xE1,0xAE,0x71,0x99,0xA9,0xF2,0x93,0x33,0x3B,0x04,0x36,0xEC,
  0xA2,0x6E,0xD5,0x87,0xED,0x97,0xB3,0x3F,0x39,0xF3,0x65,0x5B,0xB7,0x7A,0xF5,0xD0,0x8A,0x07,0xBA,0x0C,
  0x17,0x5E,0x64,0x34,0xD4,0x10,0x14,0xE5,0x1F,0x5A,0x6E,0x32,0x72,0xA2,0x82,0x11,0x00,0xBC,0xE2,0x74,
  0xD1,0x68,0x9E,0x56,0xF1,0x24,0xFE,0x3F,0x4E,0xFC,0xC7,0x6C,0xB5,0x08,0x6C,0xA8,0xF2,0xB1,0xFA,0xD7,
  0x0E,0x7D,0x24,0xB0,0x55,0x53,0xB3,0x75,0xC9,0xD1,0x4F,0xD4,0x7C,0x6E,0xB8,0x21,0x98,0x73,0x06,0x04,
  0x00,0xBC,0xC4,0x84,0xE4,0xA1,0xC9,0x21,0x71,0x62,0xDB,0xD6,0x37,0x35,0xFC,0x33,0x67,0xBD,0xD8,0xB6,
  0x6A,0xDE,0xF0,0x55,0x69,0xAE,0x39,0x50,0x2A,0xF8,0xF6,0xAE,0x6D,0x97,0x0E,0xAA,0xD9,0x5D,0x8E,0x7B,
  0x31,0x19,0x60,0x17,0x4C,0x01,0xA1,0x0D,0x9E,0xED,0xFD,0x80,0xF0,0xB6,0x1F,0xE4,0x6D,0xA9,0x34,0xD7,
  0x38,0xBF,0xCD,0x07,0x4B,0x73,0x85,0x5F,0xDA,0x55,0x5C,0x57,0x7E,0xA5,0xF6,0xAA,0xF0,0x3A,0xCF,0x8E,
  0x5B,0x14,0x28,0x40,0xE7,0xD7,0x29,0x34,0x3E,0x21,0x28,0x2A,0xDA,0x3F,0x3C,0xC6,0x3F,0x3C,0x26,0x20,
  0x3C,0x26,0x20,0x3C,0xC6,0x3F,0x3C,0xDC,0x10,0x6C,0xD0,0xE9,0x0D,0x5A,0x5F,0x83,0x4E,0xEF,0xE7,0xE3,
  0x6B,0xF0,0xF1,0xF5,0xF5,0xD1,0x35,0x5A,0x9B,0x2C,0xCD,0x4D,0x66,0x6B,0xA3,0xD9,0x6A,0xB9,0x66,0x32,
  0x5E,0x35,0x55,0x95,0x37,0x18,0x8B,0xEB,0xCA,0xCF,0x54,0x15,0x9E,0xA9,0xBA,0x7C,0xBA,0xF2,0xB2,0x4B,
  0x8E,0x0B,0x08,0x00,0x78,0x92,0xCC,0xB8,0x8C,0x7E,0x31,0xDD,0xC4,0xB6,0xB5,0xB6,0x34,0xBF,0x7D,0xEC,
  0x33,0xE1,0x8F,0xAE,0x52,0x51,0xA1,0xCA,0x4D,0x55,0x6A,0xBE,0xF5,0x55,0x53,0x95,0x70,0x00,0x54,0x36,
  0xD8,0xA7,0xB0,0xC6,0xF8,0x87,0xF7,0x8A,0xEE,0xD2,0x35,0x2C,0xB1,0x4B,0x58,0xFB,0x2E,0xA1,0x09,0x5D,
  0xC2,0xDA,0xC7,0x05,0x46,0x6A,0x14,0x5B,0xD3,0xC5,0xE0,0xA3,0x37,0xF8,0xE8,0x83,0xF4,0xFE,0x8A,0xA2,
  0xC4,0x07,0x46,0x7D,0xFF,0x0F,0xCE,0x1B,0x8B,0xF6,0x95,0x9C,0xDC,0x5B,0x9C,0xB3,0xF5,0xD2,0xFE,0xD2,
  0xFA,0x4A,0x4E,0x75,0x02,0x00,0xB0,0x67,0xF7,0x7F,0xDD,0xF9,0x6F,0x2F,0xD6,0x94,0x08,0x6F,0xAE,0xA6,
  0x92,0x56,0x37,0xD6,0xA9,0xF9,0xD6,0x6A,0x36,0xAF,0x10,0xCD,0xAD,0x30,0x43,0x50,0x9F,0xE8,0x94,0x3E,
  0xD1,0x29,0x7D,0x63,0x52,0xFA,0x44,0xA7,0x38,0xFA,0x55,0x33,0x9D,0x42,0xE3,0x3B,0x85,0xC6,0xCF,0xE8,
  0x36,0xBA,0xB9,0xA5,0x65,0x6F,0xF1,0x89,0xCF,0xCE,0xED,0x5A,0x75,0x7A,0x47,0x9D,0xC5,0xC4,0x39,0x4F,
  0x00,0x00,0x8A,0xA2,0x28,0x29,0x61,0x89,0x63,0x3A,0x0C,0x14,0xDE,0x5C,0xEC,0xC7,0x5F,0xD7,0x95,0x9A,
  0xC4,0xBB,0xA5,0xA1,0xBE,0x41,0x6A,0x3E,0x3A,0xD4,0x20,0xB8,0x79,0x83,0xB5,0xB1,0xDA,0x2C,0x18,0x1E,
  0x9B,0x27,0xBD,0x9E,0x1A,0xD1,0xC1,0xF9,0x47,0x59,0xAB,0xD1,0x0C,0x89,0xEF,0x39,0x24,0xBE,0xE7,0xFC,
  0x81,0x8F,0xBF,0x97,0xBB,0xF1,0xED,0xE3,0x9F,0x31,0x20,0xF0,0x6E,0xDC,0x04,0x86,0x4D,0x9E,0xE9,0x3D,
  0xCD,0xF6,0x39,0x87,0x9B,0xEC,0x29,0x3E,0x71,0xA8,0x2C,0x5F,0xCD,0xA7,0x97,0xD4,0x5D,0x2B,0xAA,0x2B,
  0x17,0xDB,0x36,0x5A,0xDD,0x0F,0x08,0xA2,0xFD,0xC3,0xC4,0x36,0x3C,0x52,0x76,0x5A,0xE0,0x17,0x0F,0x6E,
  0x22,0xD4,0x10,0x34,0xB7,0xF7,0x83,0xD9,0xD3,0xDF,0x7D,0xBA,0xE7,0xFD,0x2C,0x6A,0x44,0x00,0x40,0x6A,
  0xB1,0x01,0xE1,0x0F,0x76,0x1D,0x21,0xBC,0xF9,0x1B,0x47,0x57,0xAB,0x6F,0xC3,0xBE,0x12,0xC1,0x27,0x79,
  0x06,0xC4,0xA4,0x0A,0xAF,0xED,0xD3,0x3E,0x28,0xE6,0x96,0x93,0xE6,0xB6,0xD8,0x5B,0x72,0xD2,0xD3,0x8F,
  0x7B,0xB0,0x6F,0xC0,0x1F,0x33,0x9F,0xDC,0x36,0x65,0x49,0xFB,0x20,0x5E,0x75,0x49,0x00,0x40,0x56,0x4F,
  0x66,0x4C,0x36,0xF8,0xE8,0xC5,0xB6,0x3D,0x5D,0x75,0x79,0xCB,0xC5,0xFD,0xEA,0xDB,0xF0,0xF5,0x95,0xA3,
  0xC2,0x9D,0xD9,0x81,0xED,0xBA,0x8B,0x6D,0x3B,0x3A,0x69,0x80,0xF3,0x1B,0xEC,0x6E,0xFA,0x44,0xA7,0xEC,
  0x9A,0xF6,0xB7,0x21,0xF1,0x3D,0xB9,0x10,0x08,0x00,0x48,0x27,0x50,0xEF,0xFF,0xE3,0xEE,0xF7,0x09,0x6F,
  0x2E,0xB6,0xF6,0xC3,0xF7,0xAD,0x3E,0xB3,0xB3,0x56,0xF4,0xB6,0xE4,0xAF,0xFB,0xCE,0x10,0xD8,0x4A,0xAF,
  0xD5,0xCD,0xED,0xF3,0xA0,0xD8,0x27,0x9E,0x37,0x16,0xED,0x2A,0x3C,0xEA,0x35,0xE7,0x40,0xA4,0x5F,0xE8,
  0xEA,0xF1,0x2F,0x0F,0x8E,0x4B,0xE7,0x72,0x20,0x00,0x20,0x97,0x99,0x69,0x63,0xC3,0x44,0x6F,0x84,0x96,
  0x99,0x2A,0x3F,0x3E,0xBD,0xDD,0x2E,0xCD,0xA8,0xB5,0x98,0x56,0xE6,0x6F,0x13,0xDB,0xF6,0x9E,0xC4,0x7E,
  0x0F,0xA5,0xB4,0x79,0x0A,0xEB,0x85,0x01,0x8F,0x76,0x08,0x6E,0x27,0xF6,0x89,0xFF,0x38,0xB9,0xD6,0x73,
  0x6F,0x00,0xDC,0x92,0xBF,0xCE,0xF0,0xC9,0xB8,0x97,0xBA,0x47,0x24,0x73,0x45,0x10,0x00,0x90,0x85,0x4E,
  0xEB,0xF3,0x74,0x86,0xF8,0xDA,0x0F,0xCB,0x4E,0xAE,0x15,0x5B,0xFB,0xE1,0x96,0xDE,0x3A,0xB6,0xC6,0xD4,
  0x64,0x16,0xDB,0xF6,0xCD,0xBB,0x9F,0x1B,0xD7,0x71,0xB0,0xED,0x7F,0x9F,0x95,0x31,0xE5,0xB9,0x3E,0xD3,
  0xC5,0x3E,0xAB,0xA8,0xAE,0xFC,0xC3,0xBC,0xCD,0xDE,0x77,0x32,0x04,0xFB,0x06,0xFC,0x6B,0xD4,0x6F,0xFD,
  0xED,0xF1,0x56,0x06,0x10,0x00,0xF0,0x00,0x53,0x3A,0x0F,0x4B,0x0C,0x16,0xBC,0x01,0x68,0x6A,0x32,0x2F,
  0x3F,0xB9,0xCE,0x8E,0x8D,0xB9,0x58,0x53,0xF2,0x87,0xFD,0xEF,0x8A,0x6D,0xEB,0xAB,0xD5,0x7D,0x34,0x76,
  0xC1,0x9F,0x87,0xFE,0x2C,0xC6,0xBF,0x95,0x87,0x82,0x92,0x82,0x63,0xFF,0x3D,0xFA,0xF7,0xAF,0x0C,0xC9,
  0x12,0x6E,0xE7,0x33,0x5F,0xFD,0xA5,0xD6,0x4B,0x1F,0xA2,0xEF,0x16,0x9E,0xF4,0xF2,0xE0,0x27,0xB8,0x2E,
  0xBC,0xA7,0x87,0xC7,0x2E,0xC0,0xED,0x6A,0x59,0xAF,0x69,0xC2,0xDB,0x3A,0x62,0xED,0x87,0x77,0x4E,0xFC,
  0xE7,0xDE,0x8E,0x99,0x77,0x26,0xF4,0x12,0xDB,0xFC,0x89,0xF4,0x89,0xB3,0xBA,0x8F,0x5F,0x5F,0xB0,0xFB,
  0x9B,0x2B,0xC7,0xB2,0xCB,0xF2,0xCA,0xEA,0x2B,0x8C,0xE6,0x3A,0x45,0x51,0x42,0x0D,0x81,0x71,0x81,0x51,
  0x03,0x62,0xD3,0x86,0xAB,0x7E,0x23,0xD8,0xBF,0x4E,0x6D,0xD8,0x71,0x39,0xDB,0x8B,0x4F,0x89,0x59,0xDD,
  0xC7,0x2F,0xCB,0x59,0x9B,0x57,0x71,0x91,0xAB,0x83,0x00,0x80,0x37,0xBB,0x2B,0xA1,0x4F,0xCF,0xA8,0x2E,
  0x62,0xDB,0x5A,0x5B,0x9A,0xDF,0x3E,0xFE,0x99,0xDD,0x9B,0xD4,0xA2,0xB4,0xFC,0x68,0xEB,0x8B,0x6B,0x27,
  0x2C,0xEA,0x25,0xDA,0x30,0x5F,0xAD,0xEE,0xFE,0xCE,0x77,0xDD,0xDF,0xF9,0x2E,0x47,0xEC,0xB1,0x0D,0x05,
  0x7B,0x7E,0xFD,0xED,0x5B,0xDE,0x7D,0x56,0xF8,0x68,0xB4,0x0B,0xEE,0xF8,0xC9,0xF4,0x4D,0xF3,0xB9,0x40,
  0x08,0x00,0x78,0xB3,0x67,0x7B,0x8B,0x77,0xFF,0xD7,0x17,0xEC,0xBE,0x50,0x5D,0xEC,0x88,0x56,0x19,0xCD,
  0xB5,0x93,0xD7,0xCD,0x5B,0x37,0xF1,0xCF,0xE9,0x91,0x9D,0xDC,0x6A,0x77,0x6D,0xBE,0xB8,0x6F,0xD6,0xB6,
  0x85,0x4D,0xCD,0x56,0x27,0x7C,0x56,0x63,0x73,0x53,0x6E,0xC5,0x85,0xBC,0x8A,0x8B,0x05,0xD5,0xC5,0x17,
  0xAA,0x8B,0x4A,0xEA,0x2B,0xAE,0x35,0x54,0x97,0x9B,0x8C,0xF5,0x4D,0x0D,0x8D,0x56,0x4B,0xA3,0xD5,0xA2,
  0x28,0x8A,0x5E,0xAB,0x0B,0xD4,0xFB,0x87,0x1A,0x02,0xDB,0x05,0x44,0x26,0x05,0xC7,0xA6,0x45,0x74,0x18,
  0x10,0x9B,0xD6,0x2F,0x26,0x55,0xF8,0x89,0xDE,0xEB,0xC6,0x76,0xB8,0xA3,0x5B,0x78,0x52,0x7E,0xE5,0x25,
  0xAE,0x11,0x02,0x00,0xDE,0xA9,0x7B,0x44,0xB2,0x9A,0xD7,0xF0,0xBE,0x69,0x8F,0x1F,0x7F,0xFD,0x90,0x4A,
  0x73,0xCD,0x98,0x2F,0x9E,0x5B,0x7C,0xE7,0x9C,0xE9,0x29,0x23,0xDD,0x61,0x5F,0x59,0x5B,0x9A,0x17,0x1F,
  0xFE,0x68,0x51,0xF6,0x87,0x02,0x2F,0xBB,0xB7,0x5D,0x7D,0x53,0xC3,0xEE,0xA2,0x13,0x5F,0x15,0x1E,0xD9,
  0x5D,0x7C,0xFC,0xD4,0xB5,0x82,0xC6,0xE6,0xA6,0x56,0x5A,0x65,0x6D,0x6C,0xB0,0x36,0x5E,0x6B,0x30,0x9E,
  0x37,0x16,0xED,0x29,0x3E,0xF1,0xDD,0x7F,0x0C,0xD2,0xFB,0xDF,0x97,0x9C,0xF9,0x64,0xC6,0xE4,0x3E,0xD1,
  0x29,0x6A,0x1A,0x33,0x2B,0x6D,0xFC,0x0B,0x7B,0x96,0x72,0x99,0x10,0x00,0xF0,0xD6,0xEE,0xBF,0xF8,0xD2,
  0x6F,0x7B,0x8B,0x4F,0x66,0x97,0xE5,0x39,0xB4,0x79,0x75,0x16,0x53,0xD6,0x97,0xAF,0xEE,0xB8,0x9C,0xFD,
  0xEA,0xD0,0x39,0xC2,0x4F,0xA9,0xDA,0xC5,0xA5,0x9A,0xD2,0xAC,0x2F,0x5F,0xBD,0x5E,0x61,0xED,0xCE,0xD4,
  0x64,0xDE,0x70,0x61,0xCF,0x17,0xE7,0xBE,0xD9,0x7E,0xE9,0x60,0x83,0xB5,0x51,0xE5,0xBF,0x56,0x6B,0x31,
  0xAD,0x3A,0xBD,0x63,0xD5,0xE9,0x1D,0x8F,0xA4,0x8E,0x5E,0x3C,0x74,0x8E,0x9F,0xE8,0x23,0x3D,0xD3,0xBB,
  0x8D,0xFC,0xBF,0x7D,0xCB,0x9C,0x33,0xDC,0x01,0x01,0x00,0xA7,0x8A,0x0F,0x8C,0x9A,0xDA,0x45,0x7C,0x96,
  0xFC,0x8D,0x63,0xAB,0x9D,0xD3,0xCE,0x4F,0xCF,0xEC,0xDC,0x7A,0xE9,0xE0,0x33,0xBD,0xA6,0x3D,0x95,0x31,
  0xD9,0xF9,0x6F,0x5F,0x29,0xAD,0xAF,0x7C,0xFD,0xF0,0x47,0xFF,0x3A,0xB5,0xA1,0xD5,0xCE,0xB8,0x98,0xB3,
  0xC6,0xC2,0x7F,0xE6,0xAC,0x5F,0x99,0xBF,0xCD,0x68,0xAE,0xB5,0xFB,0x3F,0xBE,0x22,0x6F,0xEB,0xC9,0xF2,
  0xF3,0x6B,0x27,0x2C,0x12,0x5B,0xED,0x2E,0xDC,0x10,0xDC,0x3F,0x26,0x6D,0x9F,0xE7,0x2F,0x77,0x21,0x39,
  0x1F,0xFD,0x5D,0x2E,0x7B,0x75,0xB5,0x4F,0xB0,0x2F,0x07,0xC0,0x3D,0xFD,0xA6,0xFF,0x23,0xC2,0x3F,0xFB,
  0x3C,0x53,0x55,0xF8,0xFC,0xB7,0x6F,0x3B,0xAD,0xA9,0x66,0x6B,0xE3,0xD7,0x57,0x8E,0xBE,0x9F,0xB7,0xA9,
  0xD6,0x62,0x8A,0x0F,0x8A,0x52,0xF9,0xF2,0x48,0x1B,0x1D,0xB9,0x7A,0xFA,0x2F,0x47,0x56,0x3D,0xBD,0xF3,
  0xB5,0x7D,0x25,0x39,0x8E,0x98,0xF6,0xC9,0x8C,0xCF,0x58,0x7C,0x78,0xD5,0x73,0xDF,0xBC,0x71,0xB0,0x34,
  0xD7,0xAC,0xBA,0xD7,0xFF,0xC3,0x01,0x56,0x71,0xA9,0xA6,0x74,0x52,0xE7,0x3B,0x85,0x37,0xF7,0x9A,0xE5,
  0x2E,0x5C,0xAE,0xB9,0xB6,0xD1,0x25,0x9F,0xAB,0x09,0x98,0x9F,0xE9,0xAA,0xEF,0xAC,0x8F,0x0B,0xE2,0xC0,
  0xBB,0xA1,0x60,0xDF,0x80,0x53,0x3F,0x5A,0x11,0xEC,0x1B,0x20,0xB6,0xF9,0xDC,0x5D,0x4B,0xFE,0x9D,0xBB,
  0xC9,0x55,0x8D,0x1F,0xDE,0xBE,0xEF,0x92,0xBB,0xE6,0x0A,0xFF,0x82,0xB7,0x55,0xD9,0x65,0x79,0xCF,0x7E,
  0xB5,0xE4,0x54,0x45,0x81,0xD7,0x1C,0xEE,0x4D,0x93,0x16,0x8B,0x85,0xFD,0xC1,0xD2,0xDC,0x51,0x9F,0xFF,
  0x9C,0xEB,0xC5,0x2E,0x2C,0xC5,0xB5,0x2E,0xF9,0x5C,0xA6,0x80,0x70,0xB3,0xC7,0xBB,0xDF,0x2B,0x5C,0xFD,
  0xAF,0x9A,0xAA,0x3E,0x3E,0xBD,0xC3,0x05,0xE7,0xB1,0xD6,0x67,0x54,0xD2,0xC0,0x47,0x53,0xC7,0x8C,0x48,
  0xEC,0xAF,0xFE,0x29,0x97,0xDB,0xE8,0x1F,0x93,0xBA,0x79,0xF2,0xE2,0x0D,0x05,0x7B,0xDE,0xCF,0xDB,0xBC,
  0xB7,0xD8,0x1B,0x26,0x40,0x3E,0x3E,0xB3,0x43,0x2C,0x00,0xD2,0x23,0x3B,0x69,0x35,0x1A,0xE1,0xD7,0x6D,
  0xC2,0x1D,0x10,0x00,0xF8,0xDF,0x61,0x99,0x56,0xF7,0x54,0xC6,0x64,0xE1,0xCD,0x97,0x9D,0x5C,0xDB,0x60,
  0x75,0xEA,0x60,0xD6,0x5F,0x67,0x78,0x2C,0x6D,0xFC,0x33,0xBD,0xA6,0x3A,0xFA,0xB5,0x59,0xD7,0x85,0xF8,
  0x06,0x3E,0xDC,0x6D,0xD4,0xC3,0xDD,0x46,0xE5,0x5C,0x2B,0x58,0x72,0xF4,0xE3,0x35,0x67,0xBF,0xF2,0xE8,
  0x22,0xB8,0xBB,0xE8,0xB8,0xF0,0x9E,0xEF,0x1C,0x9A,0x70,0xA6,0xAA,0x90,0xAB,0xC6,0x73,0xB1,0x14,0x04,
  0xFE,0xC7,0xB4,0xAE,0xC3,0xE3,0x44,0x57,0xC0,0x37,0x35,0x99,0x97,0xE7,0xAC,0x73,0x66,0x6B,0x1F,0x4A,
  0x19,0x71,0xE8,0xE1,0x77,0x5F,0x19,0x92,0xE5,0xB4,0xEA,0x7F,0xA3,0x1E,0x91,0xC9,0xCB,0x46,0xCC,0xFB,
  0x66,0xDA,0xDF,0x87,0x25,0xF4,0xF6,0xDC,0x23,0x5E,0x52,0x5F,0x21,0xBC,0x6D,0x6A,0x78,0x07,0x2E,0x19,
  0x02,0x00,0xDE,0xE3,0x99,0x5E,0x53,0x85,0xB7,0xFD,0x30,0x6F,0x4B,0x45,0x43,0xB5,0x73,0xDA,0xD9,0x2E,
  0x20,0x62,0xF5,0xBD,0x0B,0xDF,0xB9,0xE7,0x37,0xC2,0x2F,0x6C,0xB1,0x63,0x0C,0xAC,0x9D,0xB0,0x68,0xC9,
  0xB0,0xB9,0x01,0x3A,0x3F,0x4F,0x3C,0xE2,0x35,0x8D,0xF5,0xC2,0x4F,0x31,0xB9,0x24,0x77,0x41,0x00,0xC0,
  0x21,0x46,0x25,0x0D,0x10,0x5E,0xEF,0xB7,0xB9,0xA5,0xC5,0x11,0x6B,0x3F,0xDC,0xD2,0xC0,0xD8,0xB4,0xAF,
  0xA6,0xBD,0x35,0x52,0xC5,0xEF,0xD4,0xEC,0x6E,0x56,0xF7,0xF1,0xDB,0xA6,0x2C,0x71,0xDC,0xCD,0x67,0x87,
  0x6A,0x11,0x9D,0xC2,0x6A,0x17,0x10,0xC9,0x55,0xE3,0xD1,0xB8,0x07,0x80,0x1B,0xBB,0xFF,0xAA,0xD6,0x7E,
  0x28,0x70,0xCC,0xDA,0x0F,0x37,0x19,0x99,0xD8,0xFF,0xC3,0x31,0xF3,0xFD,0xDC,0x6F,0x51,0xE2,0x1E,0x91,
  0xC9,0x5B,0xA6,0xFC,0x65,0xF2,0xFA,0x79,0x4E,0x5B,0x28,0x2D,0xD8,0x37,0xA0,0x47,0x44,0x72,0xC7,0x90,
  0xB8,0xA4,0xE0,0xD8,0xC4,0xE0,0x98,0xD8,0x80,0x88,0x30,0x43,0x70,0x98,0x21,0x28,0xD4,0x37,0xC8,0xCF,
  0x47,0xAF,0xF7,0xD1,0xE9,0xB5,0x3A,0xBD,0xD6,0x81,0xD7,0x78,0xBB,0x40,0x02,0x80,0x00,0x80,0x57,0xE8,
  0x15,0xD5,0x45,0xCD,0x5C,0xF6,0x9B,0x4E,0xF9,0xF1,0xD7,0xE0,0xB8,0xF4,0x15,0x63,0x17,0x38,0xF4,0x39,
  0x1F,0x55,0x05,0x31,0x20,0x62,0xED,0x84,0x45,0x23,0x3F,0x9B,0x7B,0xA9,0xA6,0xD4,0x41,0x1F,0xD1,0x35,
  0xAC,0xFD,0xB0,0x84,0xDE,0x77,0xC6,0xF7,0xEA,0x13,0x93,0x92,0x14,0x1C,0xAB,0x51,0x34,0x2E,0xFC,0xBE,
  0xC1,0x4E,0xFF,0xF1,0x1D,0x08,0x00,0x38,0x84,0x9A,0xB5,0x1F,0xF6,0x95,0x9C,0x3C,0x58,0x9A,0xEB,0xE8,
  0x16,0x26,0x06,0xC7,0xAC,0x54,0x5D,0xFD,0xCB,0x4D,0xC6,0xED,0x97,0xB3,0xBF,0x2D,0x3A,0x76,0xAC,0xFC,
  0xEC,0x35,0x93,0xB1,0xC2,0x5C,0xA3,0x28,0x4A,0x84,0x21,0x38,0xD2,0x3F,0xB4,0x4F,0x74,0xCA,0xD0,0xF8,
  0x9E,0xA3,0x92,0x06,0x84,0x1B,0x82,0x85,0xFF,0xFD,0x18,0xFF,0xF0,0x4F,0xC6,0xBF,0x3C,0x7C,0xCD,0x1C,
  0xE1,0x77,0xD7,0xDC,0x52,0x4A,0x58,0xE2,0xB4,0xAE,0xC3,0xA7,0x75,0x19,0xDE,0x29,0x34,0xDE,0x7D,0xCE,
  0x19,0x3F,0x1D,0xBF,0xE5,0x24,0x00,0xE0,0xF9,0x92,0x82,0x63,0x27,0x77,0x1E,0x26,0xBC,0xF9,0x1B,0x47,
  0x1D,0xDE,0xFD,0xD7,0x6A,0x34,0xCB,0x46,0xCC,0x53,0x53,0x9A,0x4B,0xEA,0x2B,0x96,0x1C,0xF9,0xF8,0xBD,
  0xDC,0x4D,0x0D,0xDF,0x2B,0xCD,0x45,0x4D,0xE6,0xA2,0xBA,0xF2,0x13,0xE5,0xE7,0xDE,0xCF,0xDD,0x14,0xA8,
  0xF7,0xFF,0x49,0x8F,0xFB,0x7E,0xDE,0xFB,0x41,0xE1,0x1F,0x15,0xA7,0x86,0x27,0xFD,0x29,0x33,0xEB,0xE7,
  0x5F,0xFF,0xD5,0x2E,0x5F,0x7C,0x68,0x7C,0xCF,0xB9,0xBD,0x1F,0x1C,0xA5,0xE2,0xF5,0xF4,0x0E,0x0C,0x00,
  0x1F,0x02,0x80,0x00,0x80,0xE7,0x7B,0xBA,0xE7,0xFD,0xC2,0x6F,0x41,0x39,0x6B,0x2C,0xDC,0x74,0x61,0x9F,
  0xA3,0x5B,0x38,0x33,0x6D,0xDC,0xA0,0x76,0x3D,0x84,0x37,0xDF,0x5B,0x7C,0x72,0xE6,0xD6,0x97,0xAE,0x9A,
  0xAA,0x5A,0xFD,0xCB,0x3A,0x8B,0xE9,0x8D,0xA3,0x9F,0x7E,0x7E,0x6E,0xD7,0xCA,0x31,0x0B,0x32,0xA2,0x3A,
  0x8B,0x7D,0xDC,0x63,0xDD,0xC7,0xAD,0xCC,0xDF,0x7A,0x40,0xDD,0xA8,0x28,0x39,0x24,0xEE,0x4F,0x43,0x9E,
  0x1A,0xDB,0xE1,0x0E,0xB7,0x3D,0x6D,0x7C,0xDD,0x75,0x2E,0x0E,0xB6,0xF6,0xAB,0xD8,0x05,0x08,0x33,0x04,
  0xCD,0x4C,0x1B,0x2B,0xBC,0xF9,0xDF,0x8E,0xAD,0x71,0xF4,0x0B,0xD0,0xFD,0x7C,0x7C,0x7F,0x3B,0x60,0xA6,
  0xF0,0xE6,0x3B,0x2E,0x67,0x4F,0x5C,0xF7,0xBC,0x2D,0xD5,0xFF,0xBA,0xCB,0x35,0x65,0xA3,0xBF,0xF8,0xC5,
  0xB1,0xF2,0xB3,0x62,0x9F,0xA8,0x51,0x34,0x2F,0x0E,0x9E,0xAD,0xE6,0x2B,0xCF,0xE8,0x36,0x7A,0xEF,0x83,
  0xEF,0xB8,0x73,0xF5,0x57,0x14,0x45,0xA3,0xD1,0x70,0xF9,0x10,0x00,0xF0,0x6C,0x3F,0xED,0x31,0x41,0xF8,
  0x19,0xF6,0xAB,0xA6,0xAA,0x55,0xF9,0xDB,0x1D,0xDD,0xC2,0x19,0xDD,0x46,0xB5,0xFA,0x2E,0xDF,0xDB,0x94,
  0xF2,0xD9,0x3B,0x16,0x59,0xDA,0xFE,0xA8,0xBB,0xA9,0xC9,0x3C,0x73,0xCB,0x4B,0xC2,0x2B,0x71,0x0E,0x6A,
  0xD7,0x43,0x6C,0xC8,0xA2,0x51,0x34,0xAF,0x0C,0x79,0xEA,0xED,0xE1,0xBF,0xF4,0xE3,0xF5,0xEB,0x20,0x00,
  0xE0,0x50,0x06,0x1F,0xFD,0xEC,0xF4,0x49,0xC2,0x9B,0x2F,0x77,0xCA,0xDA,0x0F,0x33,0xD3,0xC6,0x09,0x6F,
  0xFB,0xDB,0x3D,0x4B,0x85,0x7F,0x9E,0x76,0xB1,0xA6,0xE4,0xB5,0xC3,0x1F,0xA9,0x68,0xB6,0xC8,0xB8,0xEA,
  0xCF,0x43,0x9F,0xCE,0x52,0xB1,0x1A,0x07,0x40,0x00,0xC0,0x56,0x0F,0xA5,0x8C,0x8C,0x0D,0x10,0xEC,0x5C,
  0x37,0x34,0x99,0x97,0xE7,0xAC,0x77,0x74,0x0B,0x3B,0x85,0xC6,0xF7,0x8E,0xEE,0x2A,0xB6,0xED,0x59,0x63,
  0xE1,0x86,0x0B,0x7B,0xD4,0x7C,0xFA,0x7B,0xB9,0x1B,0x6B,0x1A,0xEB,0xC5,0xB6,0x9D,0xD8,0xE9,0xCE,0xB6,
  0x3E,0x86,0x9F,0x95,0x31,0x65,0x76,0xFA,0x44,0x4E,0x4B,0x10,0x00,0x70,0x38,0x8D,0xA2,0x99,0xA3,0x66,
  0xED,0x87,0xFC,0xAD,0xD7,0x1A,0x8C,0x8E,0x6E,0xE4,0x3D,0xED,0xFB,0x09,0x6F,0xBB,0x22,0x6F,0xAB,0xCA,
  0x95,0xDA,0x6A,0x1A,0xEB,0xD7,0x17,0xEC,0x16,0xDB,0x36,0x48,0xEF,0xDF,0x3F,0x36,0xD5,0xF6,0xBF,0xCF,
  0x88,0xEA,0xFC,0xE2,0xE0,0x9F,0x72,0x5A,0x82,0x00,0x80,0x33,0x8C,0xED,0x38,0x28,0x25,0x4C,0xF0,0x8D,
  0x40,0x4E,0x5B,0xFB,0xE1,0x0E,0x15,0x0F,0xFF,0xD8,0xE5,0x4D,0x8D,0x7B,0x4B,0x72,0x54,0x34,0xBE,0xBB,
  0xED,0x7F,0xBC,0x68,0xC8,0x53,0xBE,0x5A,0x1E,0xCC,0x03,0x01,0x00,0xA7,0x98,0xDB,0x5B,0x7C,0xED,0x87,
  0x0D,0x17,0xF6,0x9C,0x37,0x16,0x39,0xA1,0x91,0x19,0x91,0x9D,0xC4,0x36,0x6C,0x6C,0x6E,0x3A,0x52,0x76,
  0x5A,0x7D,0x03,0x0E,0xA8,0x08,0x80,0x74,0x9B,0x1B,0x3F,0x3A,0x69,0x60,0x66,0x5C,0x06,0xE7,0x24,0x9C,
  0x89,0xEE,0x86,0xBC,0xFA,0xC7,0xA4,0x0E,0x6A,0x97,0x2E,0xBC,0xF9,0x9B,0x47,0x3F,0x75,0x4E,0x3B,0x3B,
  0x86,0x08,0xAE,0xB0,0x56,0x6E,0xAA,0xB2,0xCB,0xDB,0x7A,0x8B,0x55,0x2C,0x98,0xDC,0x31,0x24,0xCE,0xC6,
  0xBF,0xFC,0x69,0xFA,0x04,0xBB,0xEC,0xAE,0x53,0x15,0x05,0x5B,0x2F,0x1E,0xCC,0xAD,0xBC,0x70,0xBA,0xF2,
  0x72,0x49,0x7D,0x45,0x7D,0x53,0x43,0x9D,0xC5,0x74,0xFB,0xB7,0xB7,0x97,0xCE,0x5E,0x6F,0xE0,0x89,0x7E,
  0x02,0x00,0x52,0x51,0xB7,0xF6,0x43,0xCE,0x01,0xC7,0xAF,0xFD,0xA0,0x28,0x4A,0xA8,0x21,0x48,0xF8,0x69,
  0xC8,0xCA,0x86,0x1A,0xBB,0xB4,0xA1,0xDA,0x5C,0x67,0x6D,0x69,0x16,0xFB,0xA1,0x9C,0x8D,0xAB,0x55,0xB7,
  0x0B,0x88,0x18,0xA1,0x6E,0x71,0x53,0x6B,0x4B,0xF3,0xAA,0xFC,0xED,0xAF,0x1D,0x5E,0xE9,0x9C,0x25,0xF9,
  0x40,0x00,0xC0,0x83,0x25,0x87,0xC4,0xDD,0x97,0x3C,0x44,0xBC,0xFB,0xEF,0x94,0xA5,0xDF,0x14,0x45,0x09,
  0xD6,0x07,0x08,0x6F,0x5B,0x63,0xA9,0xB7,0x4B,0x1B,0x5A,0x94,0x96,0x3A,0x8B,0x29,0xC4,0x37,0xD0,0x71,
  0xED,0x1F,0x9E,0xD8,0x4F,0xF8,0x97,0xD8,0x8A,0xA2,0x5C,0x35,0x55,0xCD,0xD8,0xBC,0x40,0x6C,0x39,0x26,
  0x8D,0xA2,0xD1,0x69,0x7D,0xB8,0x22,0x08,0x00,0x48,0x64,0x4E,0xAF,0x69,0x5A,0xD1,0x9F,0x71,0x9E,0x33,
  0x5E,0xD9,0x74,0x61,0xAF,0x73,0xDA,0xA9,0x66,0x6A,0x22,0xC8,0x7E,0x6B,0x55,0x06,0x8A,0xFE,0x53,0x06,
  0xDB,0x96,0x4B,0x1B,0x16,0xDF,0x4B,0xB8,0x6D,0xF5,0x4D,0x0D,0xE3,0xFF,0xF3,0x4B,0xE1,0x57,0x33,0x46,
  0xF8,0x85,0xA8,0xC9,0x1E,0x78,0x34,0x0E,0xBC,0x8C,0x22,0xFD,0x42,0x1F,0xE9,0x36,0x4A,0x78,0xF3,0xBF,
  0x1D,0x5B,0xE3,0xB4,0xB7,0xE0,0x5A,0x54,0x4C,0xE2,0xAB,0x59,0x39,0xEE,0x46,0x21,0xBE,0x81,0xC2,0x25,
  0xD2,0x62,0xB5,0xD8,0xF2,0x67,0x3D,0xA3,0xBA,0x08,0x37,0xEF,0xC5,0xFD,0xEF,0xA9,0x79,0x31,0x6F,0x94,
  0x7F,0x28,0x57,0x04,0x01,0xE0,0x02,0x96,0xE2,0x5A,0x0E,0x80,0x4B,0xCC,0x4E,0x9F,0x28,0x3C,0xB1,0x5E,
  0x6E,0x32,0x7E,0x94,0xBF,0xCD,0x69,0x4D,0x55,0xB3,0xA8,0x72,0xA4,0x7F,0xA8,0x5D,0x26,0x37,0xDA,0x05,
  0x44,0x08,0x6F,0x5B,0xD7,0xD4,0x60,0xCB,0x9F,0x25,0x87,0xC6,0x89,0xFD,0xFB,0x0D,0xD6,0xC6,0x0F,0xF3,
  0x36,0xAB,0xF9,0x76,0xBC,0xD7,0xD7,0xE5,0x5C,0x58,0x09,0x19,0x01,0x48,0xC7,0x4F,0x67,0x98,0xAD,0xE2,
  0x81,0x93,0xE5,0x39,0xCE,0x58,0xFB,0xE1,0xBA,0x5A,0x8B,0x49,0xFC,0x9B,0xFA,0xF8,0xF6,0x52,0xD1,0xB3,
  0xBE,0x6E,0x60,0x5B,0x9E,0xE5,0xBF,0x39,0x00,0x2C,0xAD,0x07,0x40,0x84,0x5F,0x88,0xF0,0x5A,0x4C,0xFB,
  0x4B,0x72,0xD4,0xEC,0x22,0x45,0x51,0x46,0x24,0xF6,0xE3,0xA2,0x60,0x04,0x00,0x59,0xFC,0xA8,0xDB,0xE8,
  0x48,0x3F,0xC1,0x51,0x7F,0x43,0x93,0x79,0xD9,0xC9,0x75,0xCE,0x6C,0xAD,0xA9,0xC9,0x2C,0xBC,0x1C,0x9B,
  0xA2,0x28,0x76,0x79,0xB2,0x7E,0xB0,0x8A,0x87,0x65,0x8B,0xEB,0xCA,0x5B,0xFD,0x1B,0x35,0xF7,0x2A,0x8A,
  0x6A,0xCB,0x55,0x5D,0xFF,0x1A,0xCD,0x08,0x77,0x7A,0xB5,0x32,0x08,0x00,0x38,0xF2,0x78,0x6B,0x34,0x4F,
  0xF7,0xBC,0x5F,0x78,0xF3,0x15,0xF9,0xDB,0x9C,0xB0,0xF6,0xC3,0x4D,0x2E,0xD4,0x94,0x08,0x6F,0x3B,0xA3,
  0xDB,0x28,0x95,0x2F,0x4D,0x0C,0xD2,0xFB,0x4F,0xE8,0x24,0xFE,0xB8,0xD4,0xC5,0xEA,0xD6,0x1B,0x1F,0xA0,
  0xF7,0x13,0xFE,0xF7,0x55,0x8E,0xC6,0xA6,0x75,0x19,0x9E,0x10,0x14,0xCD,0x75,0x41,0x00,0x40,0x0A,0xF7,
  0x25,0x0F,0x11,0x7E,0xA7,0x60,0x73,0x4B,0xCB,0x5B,0xC7,0xD7,0x38,0xBF,0xCD,0xE7,0x8C,0x57,0x84,0xB7,
  0x4D,0x8B,0xE8,0x38,0xBA,0x83,0xAA,0x77,0x69,0x3D,0x96,0x36,0x5E,0xEC,0x01,0xD0,0x36,0x34,0x5E,0xC5,
  0x1D,0xF5,0x68,0xFF,0x30,0xE1,0x6D,0x7D,0x34,0xDA,0x5F,0xF7,0x7B,0x84,0x8B,0x82,0x00,0x80,0x2C,0xD4,
  0xFC,0xF8,0x6B,0xA3,0xB3,0xD6,0x7E,0xB8,0x89,0xCA,0xB7,0x0D,0xFF,0x31,0x33,0x2B,0xD8,0x57,0xF0,0xC7,
  0x04,0x89,0xC1,0x31,0xBF,0xEA,0xF7,0xB0,0xBA,0xC6,0xE7,0xB5,0xFA,0x37,0xF5,0x2A,0x6E,0x74,0xF7,0x8F,
  0x49,0x15,0x1E,0xE2,0xBC,0x30,0x60,0x66,0xD7,0xB0,0xF6,0x5C,0x14,0x04,0x00,0xA4,0x30,0x38,0x2E,0xBD,
  0x7F,0x4C,0xAA,0xF0,0xE6,0x4E,0xFB,0xF1,0xD7,0x4D,0xF6,0x16,0x9F,0x54,0xB3,0x79,0xE7,0xD0,0x84,0x77,
  0xEE,0xF9,0x8D,0xC0,0x73,0x9C,0x7E,0x3A,0xC3,0xBF,0x47,0xFD,0x9F,0x9A,0x67,0x49,0x9B,0x9A,0xAD,0xB6,
  0xA4,0x57,0xB9,0x49,0x7C,0x56,0xAD,0x5D,0x60,0x64,0x66,0xBC,0xC8,0x7D,0x8E,0xF1,0x1D,0x07,0xFF,0xB2,
  0xEF,0x74,0x2E,0x0A,0x02,0xC0,0x95,0x78,0x12,0xD4,0x99,0xE6,0xAA,0xE8,0xFE,0xEF,0x2F,0x39,0xB5,0xBF,
  0xE4,0x94,0x4B,0x9A,0x7D,0xA2,0xFC,0x5C,0x9B,0xDE,0xE6,0x78,0xCB,0x62,0xB7,0xFA,0xDE,0x85,0x6D,0x2A,
  0xE5,0xF1,0x81,0x51,0x9B,0x27,0x2D,0xEE,0x1B,0x93,0xA2,0xE6,0x73,0xF7,0x97,0xE4,0xD4,0xDB,0xF0,0x18,
  0x68,0x7D,0x53,0x83,0x9A,0x2F,0xF8,0xD2,0xA0,0xD9,0x6D,0x8D,0xB7,0xB1,0x1D,0xEE,0x58,0x3E,0x72,0x9E,
  0xCA,0xBB,0x23,0xF0,0x82,0x1A,0xC8,0x08,0x40,0x16,0x29,0x61,0x89,0x63,0x54,0xBC,0x60,0xD6,0x55,0xDD,
  0x7F,0x45,0x51,0xAC,0x2D,0xCD,0x6B,0xCE,0xEE,0x54,0xF9,0x8F,0x0C,0x6F,0xDF,0xF7,0xDB,0x07,0xFE,0xFE,
  0xE3,0xEE,0xF7,0xB6,0xFA,0xD3,0xE2,0x00,0x9D,0xDF,0xCF,0x7A,0x4E,0xFD,0x7A,0xDA,0xDB,0xC2,0x6F,0xA1,
  0xB9,0xEE,0x93,0x33,0x5F,0xDA,0xF8,0x97,0x79,0x15,0x17,0x85,0x3F,0xA5,0x6F,0x4C,0xCA,0x5B,0xC3,0x7F,
  0x65,0xE3,0xEF,0xBA,0x35,0x8A,0x26,0x2B,0x63,0xCA,0x8A,0xB1,0x0B,0x84,0x1F,0x3C,0x85,0x37,0x61,0x29,
  0x08,0x59,0xCC,0xE9,0x35,0x55,0xB8,0xC7,0x77,0xCE,0x78,0x65,0xA3,0xBA,0xF7,0x6A,0xA9,0xB4,0x32,0x7F,
  0x7B,0x56,0xC6,0x14,0x95,0xFF,0x48,0x42,0x50,0xF4,0xEB,0xC3,0x9E,0x9D,0x37,0xE0,0xD1,0x6D,0x17,0x0F,
  0x7E,0x5B,0x7C,0xFC,0x78,0xF9,0xD9,0x8A,0x86,0x9A,0x4A,0x73,0x8D,0xA2,0x28,0xE1,0x86,0xE0,0x48,0xBF,
  0x90,0xDE,0xD1,0x5D,0x87,0xC6,0xF7,0x1A,0x9D,0x34,0x20,0xC2,0x2F,0x44,0x7D,0x9B,0x1B,0xAC,0x8D,0x5F,
  0x9C,0xFF,0xC6,0xC6,0x3F,0xDE,0x57,0x92,0x73,0x67,0x82,0xF8,0x6A,0x10,0xD3,0x53,0x46,0x74,0x0C,0x89,
  0x7D,0xF6,0xAB,0x25,0xA7,0xAB,0x2E,0xDF,0xE6,0xCF,0x06,0xB5,0x4B,0x5F,0x98,0xF9,0x44,0xBF,0x98,0x6E,
  0x5C,0x0E,0x20,0x00,0x24,0x12,0xE3,0x1F,0xFE,0x50,0xCA,0x48,0xE1,0xCD,0xDF,0x72,0xE2,0xDA,0x0F,0xB7,
  0x74,0xBC,0xFC,0xEC,0xCE,0xC2,0xC3,0xC3,0xDB,0xF7,0xB5,0xCB,0xAE,0x78,0x24,0x75,0xF4,0x23,0xA9,0xA3,
  0x1D,0xDD,0xE6,0xF7,0x4E,0x6D,0xB0,0xFD,0x17,0x0C,0x3B,0x2E,0x67,0xFF,0xBA,0xDF,0x0C,0x35,0x1F,0x37,
  0xA8,0x5D,0xFA,0xFE,0xE9,0xCB,0xBE,0x2A,0x3C,0xB2,0xF1,0xC2,0xDE,0x43,0x65,0x79,0x97,0x6B,0xCA,0xAA,
  0x1B,0xEB,0x74,0x5A,0x5D,0x6C,0x40,0x78,0x5C,0x60,0xD4,0xDD,0x09,0x7D,0xC6,0x77,0x1C,0xDC,0x23,0x32,
  0x99,0x6B,0x01,0xEE,0x15,0x00,0x96,0xE2,0x5A,0x7D,0x5C,0x10,0x47,0xC2,0xA1,0x9E,0xCC,0x98,0x24,0xBC,
  0xAA,0xDA,0xB5,0x06,0xE3,0xCA,0xD3,0xDB,0x5D,0xFE,0x15,0xFE,0x74,0xF0,0x7D,0xBB,0x04,0x80,0x73,0x34,
  0x58,0x1B,0x97,0x1C,0xF9,0xC4,0xF6,0xBF,0xDF,0x5F,0x72,0xAA,0xB0,0xB6,0xAC,0x7D,0x50,0x8C,0x9A,0x0F,
  0xD5,0x28,0x9A,0xE1,0xED,0xFB,0x7A,0xD0,0x5E,0x82,0xCB,0x6F,0x82,0x72,0x0F,0xC0,0xFB,0x05,0xE8,0xFC,
  0x7E,0xD2,0x43,0x7C,0xED,0x87,0x65,0x27,0xD7,0x35,0xA8,0x78,0x4E,0xD1,0x5E,0x0E,0x94,0xE6,0xAE,0x3A,
  0xBD,0xC3,0x53,0xF6,0xF9,0xA2,0xEC,0x0F,0x4B,0xDA,0xF2,0x1A,0x99,0x16,0xA5,0xE5,0xDD,0x9C,0x0D,0x9C,
  0xAB,0x20,0x00,0x60,0x67,0x33,0xD3,0xC6,0x85,0x19,0x04,0xC7,0x58,0x0D,0xD6,0xC6,0xE5,0x39,0x6B,0xDD,
  0xE4,0x8B,0x3C,0xBF,0xFB,0xED,0x92,0xBA,0x6B,0xEE,0xBF,0xC3,0x0F,0x97,0x9D,0x7E,0xA3,0xED,0xAF,0x4B,
  0x5B,0x9E,0xB3,0xAE,0xBA,0xB1,0xCE,0xF9,0xAD,0x5D,0x7D,0x76,0xA7,0xD9,0xB6,0x25,0x4B,0x41,0x00,0xC0,
  0xC3,0xF8,0x68,0xB4,0x4F,0xF7,0x14,0xBF,0x7D,0xBA,0x32,0x7F,0x9B,0x9A,0xA7,0xD4,0xED,0xCB,0x68,0xAE,
  0x9D,0xB9,0xF5,0x25,0x67,0x2E,0x45,0x27,0xE0,0xAA,0xA9,0xEA,0xF1,0x6D,0x0B,0xAD,0x2D,0xCD,0x6D,0xDD,
  0xB0,0xBA,0xB1,0xEE,0xE5,0x03,0xEF,0x39,0xB9,0xB5,0x67,0xAA,0x0A,0xE7,0xEE,0xFA,0x2B,0x97,0x09,0x01,
  0xE0,0x4A,0xFC,0x1A,0xC0,0x71,0xA6,0x74,0xBE,0x2B,0x29,0x38,0x56,0x6C,0xDB,0xE6,0x96,0x96,0xB7,0x8E,
  0xAD,0x71,0xAB,0xAF,0x73,0xA0,0x34,0xF7,0x89,0x1D,0x8B,0x5C,0x7B,0x47,0xFA,0x36,0x4C,0x4D,0xE6,0xE9,
  0x9B,0xE6,0x5F,0x14,0x5D,0xBC,0x68,0x79,0xCE,0xBA,0x7D,0x2A,0x5E,0x40,0xDF,0x56,0x75,0x16,0xD3,0xCC,
  0xAD,0x2F,0xD5,0xA9,0x5B,0x4C,0x14,0x1E,0x5D,0xF7,0x18,0x01,0x78,0xB9,0x39,0xBD,0xA6,0x0A,0x6F,0xBB,
  0xE9,0xC2,0x5E,0x35,0xEB,0xF0,0x38,0xC8,0xDA,0xF3,0xDF,0x3E,0xBE,0x6D,0xA1,0x1B,0xCE,0x5A,0x54,0x9A,
  0x6B,0x26,0xAF,0x9F,0x77,0xA8,0x2C,0x5F,0xF8,0x5F,0x68,0x6E,0x69,0x99,0xB5,0x6D,0xA1,0xCA,0x5F,0xBD,
  0xD9,0xC8,0x6C,0xB5,0x3C,0xBC,0x79,0x41,0x6E,0xC5,0x05,0xAE,0x11,0x46,0x00,0xF0,0x4E,0xC3,0x12,0x7A,
  0xAB,0xF9,0x35,0x93,0x0B,0x7F,0xFC,0x75,0x7B,0xFF,0x39,0xFF,0xCD,0xE4,0xF5,0xF3,0xCA,0x4C,0x95,0xEE,
  0xD3,0xA4,0xF3,0xC6,0xA2,0xB1,0x5F,0x3C,0xA7,0xFE,0xC7,0xD2,0x25,0x75,0xD7,0x1E,0xD8,0xF8,0x7B,0x47,
  0xDF,0x0C,0x68,0x6A,0xB6,0xCE,0xDA,0xB6,0xF0,0xEB,0x2B,0x47,0xB9,0x46,0x08,0x00,0x78,0x2D,0x35,0x4B,
  0xBF,0x1D,0x28,0xCD,0x75,0xE6,0x74,0x44,0x5B,0xED,0x2D,0x3E,0x99,0xF9,0xC9,0x93,0x1B,0x9D,0xF5,0x6A,
  0xE2,0xDB,0x7B,0x3F,0x77,0xD3,0xD0,0x4F,0x9F,0xCA,0xAF,0xBC,0x64,0x97,0x7F,0xED,0xE8,0xD5,0x33,0x53,
  0x37,0xFC,0xAE,0xA2,0xA1,0xDA,0x41,0xAD,0x35,0x9A,0x6B,0x1F,0xD8,0xF8,0xFB,0x4D,0xEE,0xB1,0xEB,0xA4,
  0xE5,0x26,0xF3,0xDE,0x5A,0x76,0x87,0xB7,0xEA,0x1E,0x91,0x3C,0x52,0xC5,0xBB,0x3E,0xDE,0x6C,0xFB,0x73,
  0x2C,0x4E,0x56,0x6E,0x32,0xCE,0xD8,0xBC,0xE0,0xD1,0x2D,0x2F,0x9E,0x35,0x16,0xBA,0xAA,0x0D,0xC7,0xCA,
  0xCF,0x4E,0x5C,0xF7,0xFC,0xB3,0xBB,0x96,0xD4,0xDB,0xF6,0xEA,0x47,0x1B,0x1D,0x2C,0xCD,0x1D,0xF1,0xD9,
  0xB3,0x79,0x76,0x4A,0x94,0x1B,0x9D,0x33,0x5E,0x19,0xF9,0xF9,0xDC,0x9D,0x85,0x87,0xB9,0x40,0xC0,0x08,
  0xC0,0xBB,0xBB,0xFF,0xD3,0x84,0xB7,0x3D,0x6F,0x2C,0xDA,0xE0,0xD2,0xB5,0x1F,0x6C,0xB7,0xAE,0x60,0xF7,
  0xA0,0x8F,0x9F,0x78,0x76,0xD7,0x92,0x53,0x15,0x05,0xCE,0xFC,0xDC,0x43,0x65,0xF9,0x3F,0xD9,0xFE,0xA7,
  0xBB,0x57,0xCF,0x71,0xD0,0x44,0x4A,0x41,0x75,0xF1,0xDD,0xAB,0x7F,0xF6,0xF7,0xE3,0x9F,0xDB,0xEB,0x8E,
  0x77,0x53,0xB3,0xF5,0x8D,0xA3,0x9F,0x0E,0xFD,0xF4,0x29,0x35,0x6F,0x90,0x87,0x97,0x71,0xA3,0xA5,0x20,
  0xF8,0x49,0xB0,0x1D,0xC5,0x05,0x46,0x4D,0xED,0x72,0xB7,0xF0,0xE6,0x6F,0x1F,0xFF,0xCC,0x6D,0x9F,0xB4,
  0xB9,0x65,0x69,0x7B,0x3F,0x77,0xD3,0xFB,0xB9,0x9B,0x06,0xC7,0xA5,0x3F,0xD2,0x6D,0xF4,0xD8,0x0E,0x83,
  0xA2,0xFC,0x43,0x1D,0xF4,0x59,0x25,0xF5,0x15,0x1B,0x0A,0xF6,0x7C,0x90,0xB7,0xF9,0xE8,0xD5,0x33,0x8E,
  0xFE,0x5E,0x0D,0xD6,0xC6,0x17,0xF6,0x2C,0xFD,0x20,0x6F,0xCB,0x82,0x41,0x3F,0x1E,0x9D,0x34,0x50,0xF8,
  0xDF,0xB1,0xB6,0x34,0x6F,0x28,0xD8,0xB3,0xE8,0xD0,0x87,0x39,0xD7,0x0A,0xB8,0x34,0xDC,0xA4,0xD6,0xB9,
  0x49,0x4B,0x34,0x01,0xF3,0x33,0xDD,0x67,0xBF,0x10,0x00,0xB0,0xC3,0xA8,0x56,0xA3,0xB9,0xA3,0x5D,0x8F,
  0xE1,0xED,0xFB,0x0E,0x8C,0xED,0xDE,0x3F,0x36,0x55,0xCD,0x1B,0x77,0xBF,0x63,0x34,0xD7,0x66,0x97,0xE5,
  0x1D,0x28,0x39,0xF5,0x65,0xE1,0xE1,0xEC,0xD2,0xBC,0x16,0xC5,0x05,0xD1,0xD8,0x2D,0x3C,0xE9,0xF1,0xEE,
  0xE3,0x27,0x76,0xBA,0x33,0x3E,0x30,0xCA,0xF6,0xAD,0xF2,0x2A,0x2F,0x6D,0xBE,0xB0,0xEF,0xDD,0x53,0xEB,
  0x2F,0xD5,0x94,0x72,0x62,0x10,0x00,0x04,0x00,0xA4,0x0B,0x83,0xCE,0xA1,0x09,0x5D,0xC2,0x12,0xBB,0x84,
  0x26,0x24,0x87,0xC6,0xC7,0xFA,0x87,0x47,0xFA,0x87,0x46,0xFB,0x87,0x85,0xF8,0x06,0x1A,0x7C,0xF4,0xBE,
  0x3E,0x7A,0x5F,0xAD,0x5E,0x51,0x94,0x46,0x6B,0x63,0x63,0x73,0x93,0xD9,0x6A,0x31,0x36,0xD6,0x96,0x9B,
  0x8C,0xE5,0xA6,0xAA,0xD2,0xFA,0xCA,0xF3,0xC6,0xA2,0x73,0xC6,0xC2,0x33,0x55,0x85,0xE7,0x8D,0x45,0x2E,
  0x29,0xFA,0xB7,0x94,0x11,0xD5,0x79,0x60,0x6C,0x5A,0x9F,0xE8,0x94,0x4E,0xA1,0xF1,0x09,0x41,0xD1,0xE1,
  0x86,0x60,0x7F,0x9D,0x41,0xAB,0xD1,0xD6,0x59,0x4C,0x75,0x16,0x93,0xB1,0xB1,0xFE,0x42,0x75,0x51,0x7E,
  0xE5,0xA5,0xBC,0xCA,0x4B,0xDF,0x16,0x1D,0xBB,0x5C,0x53,0xC6,0x39,0x40,0x00,0x78,0x4C,0x00,0x90,0x01,
  0x00,0xA8,0xFE,0xCE,0xEB,0x21,0x71,0x3C,0x00,0x40,0xD2,0x21,0x32,0xBB,0x00,0x00,0x24,0xEC,0xFE,0xBB,
  0x63,0x00,0xF0,0x83,0x00,0x00,0x60,0x04,0x00,0x00,0x90,0x2C,0x00,0x18,0x04,0x00,0xF0,0x3E,0x6E,0x58,
  0xD9,0x18,0x01,0x00,0x00,0x23,0x00,0xA2,0x12,0x00,0x64,0xAA,0x69,0x8C,0x00,0x00,0x80,0x11,0x00,0x81,
  0x09,0x00,0x32,0x55,0x33,0x46,0x00,0x00,0xC0,0x08,0x80,0xD8,0x04,0x00,0x99,0xEA,0x18,0x23,0x00,0x00,
  0x90,0xB4,0x17,0xAB,0x65,0xF7,0x01,0x80,0x9C,0x3C,0x60,0x04,0x40,0x06,0x00,0xA0,0xFB,0x2F,0x69,0x00,
  0x00,0x00,0xE4,0x0D,0x00,0x06,0x01,0x00,0xE8,0xFE,0xCB,0x3B,0x02,0x20,0x03,0x00,0x50,0xAF,0x24,0x0D,
  0x00,0x00,0x80,0xBC,0x01,0xC0,0x20,0x00,0x00,0x95,0x4A,0xDE,0x11,0x00,0x19,0x00,0x80,0x1A,0x25,0x69,
  0x00,0x00,0x00,0xD5,0x5F,0xDE,0x00,0x60,0x10,0x00,0x00,0xF2,0x8E,0x00,0xC8,0x00,0x00,0xD4,0x25,0x49,
  0x03,0x80,0x0C,0x00,0x40,0x45,0x92,0x37,0x00,0xC8,0x00,0x00,0xD4,0x22,0x79,0x03,0x00,0x00,0xA8,0xFE,
  0xF2,0x06,0x00,0x83,0x00,0x00,0x90,0x77,0x04,0x40,0x06,0x00,0xA0,0xFE,0x48,0x1A,0x00,0x64,0x00,0x00,
  0x2A,0x8F,0xBC,0x01,0x40,0x06,0x00,0xA0,0xE6,0xC8,0x1B,0x00,0x64,0x00,0x00,0xAA,0x8D,0xBC,0x01,0x40,
  0x06,0x00,0xA0,0xCE,0xC8,0x1B,0x00,0x64,0x00,0x00,0x2A,0x8C,0xBC,0x01,0x40,0x06,0x00,0xA0,0xB6,0xC8,
  0x1B,0x00,0x64,0x00,0x00,0xAA,0x8A,0xBC,0x01,0x40,0x06,0x00,0xB0,0x63,0x31,0xF1,0xD6,0x7A,0xA2,0xE5,
  0xB0,0x01,0x80,0x9C,0x5D,0x49,0x2D,0xC7,0x0F,0x00,0xE4,0xAC,0x1E,0x5A,0x8E,0x22,0x00,0xC8,0x59,0x37,
  0xB4,0x1C,0x4B,0x00,0xB8,0xA9,0x5C,0x48,0x52,0x31,0xB4,0x1C,0x54,0x00,0x90,0xB3,0xB3,0xA8,0xE5,0xE8,
  0x02,0x80,0x9C,0xF5,0x41,0x27,0xED,0x31,0xD6,0xC7,0x05,0x71,0xBA,0x03,0x90,0xB9,0x6B,0xA8,0xE5,0x78,
  0x03,0xA0,0xFA,0xCB,0xF9,0xC5,0x75,0x1C,0x75,0x86,0x02,0x00,0xA5,0x5F,0x4E,0xBC,0x13,0x98,0xA1,0x00,
  0x20,0xE9,0x85,0xCF,0xB5,0x4F,0x00,0x70,0x2A,0x00,0x74,0xFB,0x24,0xA5,0x63,0x17,0xDC,0x74,0x4E,0x30,
  0x23,0x04,0x50,0xFA,0x09,0x00,0x62,0x00,0x00,0xA5,0x9F,0x00,0x20,0x06,0x00,0x50,0xFA,0x09,0x00,0x62,
  0x00,0x00,0x75,0x9F,0x00,0x90,0xEE,0x34,0x22,0x09,0x00,0x4A,0x3F,0x01,0xC0,0x80,0x00,0x00,0x75,0x9F,
  0x00,0x60,0x40,0x00,0x80,0xBA,0x4F,0x00,0x90,0x04,0x00,0xA8,0xFB,0x04,0x80,0xBC,0x27,0x22,0x61,0x00,
  0x50,0xF4,0x09,0x00,0x4E,0x50,0xF2,0x00,0xA0,0xE8,0x13,0x00,0x9C,0xBB,0xFF,0x8B,0x54,0x00,0xA8,0xF5,
  0x04,0x00,0x67,0x39,0x00,0xB8,0x0C,0x8B,0xC1,0x01,0x00,0x01,0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,
  0x00,0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x40,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
  0x01,0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,
  0x00,0x40,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x01,0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,0x80,
  0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x40,0x00,0x00,0x00,0x01,0x00,0x00,0x20,0x00,0x00,
  0x00,0x04,0x00,0x00,0x80,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x40,0x00,0x00,0x00,0x08,
  0x00,0x00,0x00,0x01,0x00,0x00,0x20,0x00,0x00,0x00,0x04,0x00,0x00,0x80,0x00,0x00,0x00,0x10,0x00,0x00,
  0x00,0x02,0x00,0x00,0x40,0x00,0x00,0x00,0x1C,0xE4,0xFF,0x01,0x7D,0x5F,0x08,0x75,0x7F,0xA4,0x0F,0xD7,
  0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
};


// =====================================================
// ДОПОМІЖНІ ФУНКЦІЇ
// =====================================================

// =====================================================
// V35: БЕЗПЕКА, ХМАРА, PUSH, ПОГОДА
// =====================================================
bool remoteTlsVerify = true;          // перевіряти сертифікат брокера (ISRG Root X1)
String webPassword = "";              // порожній = без пароля; логін завжди "admin"
String ntfyTopic = "";                // ntfy.sh topic для push-сповіщень
bool weatherEnabled = false;          // прогноз опадів Open-Meteo
float weatherLat = 48.53f;
float weatherLon = 25.04f;
const float WEATHER_SKIP_MM = 1.0f;   // пропуск автополиву, якщо за 6 год очікується >= 1 мм
const int MAX_WATER_MINUTES = 120;    // жорсткий ліміт тривалості зони
const float BOILER_MAX_SAFE_TEMP = 32.0f; // аварійне відключення ручного режиму котла

float weatherPrecip6h = -1.0f;
float weatherOutdoorTemp = -99.0f;
bool weatherOk = false;
unsigned long lastWeatherFetch = 0;
unsigned long lastWeatherOkMillis = 0;
bool otaStarted = false;
unsigned long mqttSubscribedAt = 0;
bool lightsRestored = false;

static const char ISRG_ROOT_X1[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

// Неблокуюче читання часу (getLocalTime() за замовчуванням чекає до 5 с,
// що без NTP «вішало» весь loop: веб, BLE, полив).
static bool localTimeNow(struct tm* t) {
  time_t now;
  time(&now);
  localtime_r(&now, t);
  return t->tm_year > (2016 - 1900);
}

// ---- Push-сповіщення через ntfy.sh (черга, щоб не блокувати callbacks)
String pushQueue[4];
uint8_t pushCount = 0;
unsigned long lastPushMillis = 0;

void pushNotify(const String& msg) {
  if (ntfyTopic.length() == 0) return;
  if (pushCount >= 4) {
    for (uint8_t i = 1; i < 4; i++) pushQueue[i - 1] = pushQueue[i];
    pushCount = 3;
  }
  pushQueue[pushCount++] = msg;
}

void processPushQueue() {
  if (pushCount == 0 || ntfyTopic.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED || wifiAPMode) return;
  if (lastPushMillis && millis() - lastPushMillis < 2500UL) return;
  lastPushMillis = millis();

  String msg = pushQueue[0];
  for (uint8_t i = 1; i < 4; i++) pushQueue[i - 1] = pushQueue[i];
  pushCount--;

  WiFiClientSecure c;
  c.setInsecure(); // ntfy: публічні сповіщення без секретів
  HTTPClient h;
  h.setConnectTimeout(3000);
  h.setTimeout(3000);
  if (!h.begin(c, "https://ntfy.sh/" + ntfyTopic)) return;
  h.addHeader("Tags", "seedling");
  int code = h.POST(msg);
  Serial.printf("PUSH %d\n", code);
  h.end();
}

void pushForEvent(const String& s, const String& e, const String& d) {
  if (ntfyTopic.length() == 0) return;
  String m;
  if (s == "RAIN" && e == "START") m = "🌧 Почався дощ. Автополив заблоковано.";
  else if (s == "RAIN" && e == "END") m = "☀️ Дощ закінчився.";
  else if (s == "WATER" && e == "ZONE_START") m = "💧 Полив: " + d;
  else if (s == "WATER" && e == "WEATHER_SKIP") m = "🌦 Полив пропущено за прогнозом: " + d;
  else if (s == "SYSTEM" && e == "BOOT") m = "🔌 Контролер запущено. " + d;
  else if (s == "BOILER" && e == "SENSOR_LOST") m = "⚠️ Немає даних датчика температури — котел вимкнено.";
  else if (s == "BOILER" && e == "SENSOR_OK") m = "✅ Датчик температури відновився.";
  else if (s == "BOILER" && e == "OVERHEAT") m = "🔥 Аварійне вимкнення котла: " + d;
  if (m.length()) pushNotify(m);
}

// ---- Прогноз погоди Open-Meteo (без ключа). Помилка = просто не пропускаємо полив.
void processWeather() {
  if (!weatherEnabled || WiFi.status() != WL_CONNECTED || wifiAPMode) return;
  if (!lastWeatherFetch) { if (millis() < 25000UL) return; }
  else if (millis() - lastWeatherFetch < 30UL * 60UL * 1000UL) return;
  lastWeatherFetch = millis();

  WiFiClientSecure c;
  c.setInsecure(); // публічні погодні дані; впливає лише на пропуск поливу
  HTTPClient h;
  h.setConnectTimeout(4000);
  h.setTimeout(4000);
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(weatherLat, 3) +
               "&longitude=" + String(weatherLon, 3) +
               "&current=temperature_2m&hourly=precipitation&forecast_hours=6&timezone=auto";
  if (!h.begin(c, url)) return;
  int code = h.GET();
  if (code != 200) { Serial.printf("WEATHER http %d\n", code); h.end(); return; }
  String body = h.getString();
  h.end();

  int cur = body.indexOf("\"current\":{");
  int hr = body.indexOf("\"hourly\":{");
  if (cur < 0 || hr < 0) return;
  int tk = body.indexOf("\"temperature_2m\":", cur);
  int pk = body.indexOf("\"precipitation\":[", hr);
  if (tk < 0 || pk < 0) return;

  float temp = body.substring(tk + 17, tk + 25).toFloat();
  int end = body.indexOf(']', pk);
  if (end < 0) return;
  String arr = body.substring(pk + 17, end);
  float sum = 0; int pos = 0;
  while (pos <= (int)arr.length()) {
    int comma = arr.indexOf(',', pos);
    String tok = comma < 0 ? arr.substring(pos) : arr.substring(pos, comma);
    tok.trim();
    if (tok.length() && tok != "null") sum += tok.toFloat();
    if (comma < 0) break;
    pos = comma + 1;
  }
  weatherOutdoorTemp = temp;
  weatherPrecip6h = sum;
  weatherOk = true;
  lastWeatherOkMillis = millis();
  Serial.printf("WEATHER: %.1f C, rain next 6h %.1f mm\n", temp, sum);
}

bool weatherSkipsWatering() {
  if (!weatherEnabled || !weatherOk) return false;
  if (millis() - lastWeatherOkMillis > 3UL * 60UL * 60UL * 1000UL) return false;
  return weatherPrecip6h >= WEATHER_SKIP_MM;
}

// ---- Відновлення світла після відключення живлення
void processLightRestore() {
  if (lightsRestored) return;
  struct tm t;
  if (!localTimeNow(&t)) return;
  lightsRestored = true;
  int nowMin = t.tm_hour * 60 + t.tm_min;
  for (int i = 0; i < LIGHT_COUNT; i++) {
    if (!lights[i].enabled) continue;
    if (!(lights[i].days & (1 << t.tm_wday))) continue;
    int on = lights[i].onHour * 60 + lights[i].onMinute;
    int off = lights[i].offHour * 60 + lights[i].offMinute;
    bool inside = on < off ? (nowMin >= on && nowMin < off) : (nowMin >= on || nowMin < off);
    if (inside) {
      setLight(i, true);
      logEvent("LIGHT", "RESTORE", "Канал " + String(i + 1));
    }
  }
}

// ---- Авторизація веб-інтерфейсу (HTTP Basic). Порожній пароль = відкрито.
bool webAuthOk() {
  if (webPassword.length() == 0) return true;
  if (server.authenticate("admin", webPassword.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Garden", "Потрібен пароль");
  return false;
}
#define AUTH(h) ([]() { if (!webAuthOk()) return; h(); })


uint64_t getEpoch() {
  time_t now;
  time(&now);
  return (uint64_t)now;
}

String twoDigits(int v) {
  if (v < 10) return "0" + String(v);
  return String(v);
}


// =====================================================
// ЖУРНАЛ / MICROSD
// =====================================================

String currentDateTimeText() {
  struct tm t;

  if (!localTimeNow(&t)) {
    return "NO_TIME";
  }

  char b[32];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &t);

  return String(b);
}

String csvEscape(const String& s) {
  String out = "\"";

  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];

    if (c == '\"') {
      out += "\"\"";
    } else {
      out += c;
    }
  }

  out += "\"";

  return out;
}

void logEvent(
  const String& source,
  const String& event,
  const String& details
) {
  // Lite: журнал на SD вимкнений, залишаємо лише короткий Serial-log.
  Serial.print("[LOG] ");
  Serial.print(source);
  Serial.print(" | ");
  Serial.println(event);
  pushForEvent(source, event, details);
}

String readLogFile() {
  return "Журнал на microSD вимкнено у V20 Lite.";
}

bool clearLogFile() { return false; }
bool sendLogByEmail() { return false; }

// =====================================================
// ДЖЕРЕЛО ТЕМПЕРАТУРИ ДЛЯ КОТЛА
// =====================================================

bool externalTemperatureValid() {
  if (externalRoomTemperature < -50.0 || externalRoomTemperature > 80.0) {
    return false;
  }

  return (millis() - lastExternalTempMillis) < EXTERNAL_TEMP_TIMEOUT;
}

float getEffectiveRoomTemperature() {
  float xt=xiaomiHeatingTemperature();
  if (xt > -50.0f) return xt;
  if (externalTemperatureValid()) return externalRoomTemperature;
  return roomTemperature;
}

String temperatureSourceText() {
  float xt=xiaomiHeatingTemperature();
  if (xt > -50.0f) {
    if (heatingXiaomiSource==4) return "XIAOMI_AVG";
    return "XIAOMI_" + String(heatingXiaomiSource+1) + "_" + xiaomiRoomNames[heatingXiaomiSource];
  }
  if (externalTemperatureValid()) return "MQTT";
  if (roomTemperature > -50.0 && roomTemperature < 80.0) return "DS18B20";
  return "NONE";
}

// =====================================================
// JSON / REST API HELPERS
// =====================================================

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];

    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }

  return out;
}

String buildStatusJson() {
  String j;
  j.reserve(2800);

  j += "{";

  j += "\"device\":\"";
  j += DEVICE_NAME;
  j += "\",";

  j += "\"ip\":\"";
  j += wifiAPMode
    ? WiFi.softAPIP().toString()
    : WiFi.localIP().toString();
  j += "\",";

  j += "\"wifi_ssid\":\"";
  j += jsonEscape(
    wifiAPMode
      ? String(WIFI_AP_SSID)
      : wifiSSID
  );
  j += "\",";

  j += "\"wifi_ap_mode\":";
  j += wifiAPMode ? "true" : "false";
  j += ",";

  j += "\"wifi_rssi\":";
  j += String(WiFi.RSSI());

  j += ",\"remote_enabled\":";
  j += remoteAccessEnabled ? "true" : "false";

  j += ",\"remote_connected\":";
  j += mqttClient.connected() ? "true" : "false";
  j += ",";

  j += "\"sd_ready\":";
  j += sdReady ? "true" : "false";
  j += ",";

  j += "\"rain\":";
  j += isRaining() ? "true" : "false";
  j += ",";

  j += "\"rain_blocked\":";
  j += rainBlocked() ? "true" : "false";
  j += ",";

  j += "\"firmware\":\"Garden_U_V35\",";
  j += "\"uptime_s\":"; j += String((unsigned long)(millis() / 1000UL)); j += ",";
  j += "\"free_heap\":"; j += String((unsigned long)ESP.getFreeHeap()); j += ",";
  { struct tm tt; j += "\"time_synced\":"; j += localTimeNow(&tt) ? "true," : "false,"; }
  j += "\"weather_ok\":"; j += weatherOk ? "true," : "false,";
  j += "\"outdoor_temperature\":"; if (weatherOk) j += String(weatherOutdoorTemp, 1); else j += "null"; j += ",";
  j += "\"rain_forecast_mm\":"; if (weatherOk) j += String(weatherPrecip6h, 1); else j += "null"; j += ",";
  j += "\"weather_skip\":"; j += weatherSkipsWatering() ? "true," : "false,";
  j += "\"boiler_gpio\":12,";
  j += "\"watering\":";
  j += watering ? "true" : "false";
  j += ",";

  j += "\"watering_zone\":";
  j += String(currentZone >= 0 ? currentZone + 1 : 0);
  j += ",";
  j += "\"watering_all\":";
  j += waterAllActive ? "true" : "false";
  j += ",";
  j += "\"watering_simultaneous\":";
  j += waterSimultaneousActive ? "true" : "false";
  j += ",";

  j += "\"lights\":[";
  for (int i = 0; i < LIGHT_COUNT; i++) {
    if (i) j += ",";
    j += getLightState(i) ? "true" : "false";
  }
  j += "],";

  float effectiveTemp = getEffectiveRoomTemperature();

  j += "\"temperature\":";
  if (effectiveTemp > -50.0) {
    j += String(effectiveTemp, 2);
  } else {
    j += "null";
  }
  j += ",";

  j += "\"temperature_source\":\"";
  j += temperatureSourceText();
  j += "\",";

  j += "\"ds18b20_temperature\":";
  if (roomTemperature > -50.0) {
    j += String(roomTemperature, 2);
  } else {
    j += "null";
  }
  j += ",";

  j += "\"external_temperature\":";
  if (externalTemperatureValid()) j += String(externalRoomTemperature, 2); else j += "null";
  j += ",";
  j += "\"xiaomi_online\":"; j += xiaomiTemperatureValid()?"true":"false"; j += ",";
  j += "\"xiaomi_temperature\":"; if (xiaomiTemperatureValid()) j += String(xiaomiTemperature,2); else j += "null"; j += ",";
  j += "\"xiaomi_humidity\":"; if (xiaomiSeen && xiaomiHumidity>=0.0) j += String(xiaomiHumidity,2); else j += "null"; j += ",";
  j += "\"xiaomi_battery\":"; if (xiaomiSeen) j += String(xiaomiBattery); else j += "null"; j += ",";
  j += "\"xiaomi_rssi\":"; if (xiaomiSeen) j += String(xiaomiRSSI); else j += "null"; j += ",";
  j += "\"xiaomi_switch\":"; if (xiaomiOpeningKnown) j += xiaomiOpeningState?"true":"false"; else j += "null"; j += ",";
  j += "\"xiaomi_button_event\":"; j += String(xiaomiButtonEvent); j += ",";

  j += "\"xiaomi_sensors\":[";
  for (uint8_t i=0;i<XIAOMI_SENSOR_COUNT;i++) {
    if(i) j += ",";
    j += "{";
    j += "\"enabled\":"; j += xiaomiSensorEnabled[i]?"true":"false"; j += ",";
    j += "\"name\":\"" + jsonEscape(xiaomiRoomNames[i]) + "\",";
    j += "\"mac\":\"" + jsonEscape(xiaomiMacs[i]) + "\",";
    j += "\"online\":"; j += xiaomiSlotValid(i)?"true":"false"; j += ",";
    j += "\"temperature\":"; if(xiaomiSlotValid(i)) j += String(xiaomiTemperatures[i],2); else j += "null"; j += ",";
    j += "\"humidity\":"; if(xiaomiSensorSeen[i]&&xiaomiHumidities[i]>=0) j += String(xiaomiHumidities[i],2); else j += "null"; j += ",";
    j += "\"battery\":"; if(xiaomiSensorSeen[i]) j += String(xiaomiBatteries[i]); else j += "null"; j += ",";
    j += "\"rssi\":"; if(xiaomiSensorSeen[i]) j += String(xiaomiRSSIs[i]); else j += "null";
    j += "}";
  }
  j += "],";
  j += "\"xiaomi_heating_source\":" + String(heatingXiaomiSource) + ",";

  j += "\"boiler_on\":";
  j += boilerState ? "true" : "false";
  j += ",";

  j += "\"boiler_mode\":";
  j += String((int)boilerMode);
  j += ",";

  j += "\"boiler_setpoint\":";
  j += String(boilerCurrentSetpoint, 2);
  j += ",";

  j += "\"vacation\":";
  j += vacationMode ? "true" : "false";
  j += ",";

  j += "\"vacation_temperature\":";
  j += String(vacationTemperature, 2);
  j += ",";

  j += "\"mqtt_connected\":";
  j += mqttClient.connected() ? "true" : "false";

  j += "}";

  return j;
}

void sendJson(int statusCode, const String& json) {
  if (webPassword.length() == 0) server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(statusCode, "application/json; charset=utf-8", json);
}


// =====================================================
// WIFI SETTINGS / FALLBACK ACCESS POINT
// =====================================================

void loadWiFiSettings() {
  prefs.begin("wifi", true);

  wifiSSID = prefs.getString(
    "ssid",
    DEFAULT_WIFI_SSID
  );

  wifiPassword = prefs.getString(
    "pass",
    DEFAULT_WIFI_PASSWORD
  );

  prefs.end();
}

void saveWiFiSettings(
  const String& ssid,
  const String& password
) {
  prefs.begin("wifi", false);

  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);

  prefs.end();

  wifiSSID = ssid;
  wifiPassword = password;
}

bool connectWiFi(unsigned long timeoutMs) {
  if (wifiSSID.length() == 0) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPassword.c_str()
  );

  Serial.print("WiFi: ");
  Serial.print(wifiSSID);
  Serial.print(" ");

  unsigned long start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < timeoutMs
  ) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiAPMode = false;

    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());

    return true;
  }

  Serial.println("WiFi connection failed");
  return false;
}

void startWiFiSetupAP() {
  WiFi.disconnect(true);
  delay(250);

  WiFi.mode(WIFI_AP);

  bool ok = WiFi.softAP(
    WIFI_AP_SSID,
    WIFI_AP_PASSWORD
  );

  wifiAPMode = ok;

  if (ok) {
    Serial.println("WiFi setup AP started");
    Serial.print("SSID: ");
    Serial.println(WIFI_AP_SSID);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("WiFi setup AP ERROR");
  }
}

void handleWiFiSettings() {
  String page;
  page.reserve(5000);

  page += R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#26733b">
<title>Wi-Fi</title>
<style>
body{
  font-family:-apple-system,BlinkMacSystemFont,Arial,sans-serif;
  background:#eef4ee;
  margin:0;
  padding:14px;
}
.card{
  max-width:650px;
  margin:auto;
  background:var(--card);
  padding:20px;
  border-radius:18px;
  box-shadow:0 3px 12px rgba(0,0,0,.14);
}
h1{text-align:center}
label{display:block;margin-top:15px;font-weight:600}
input{
  width:100%;
  box-sizing:border-box;
  padding:12px;
  font-size:17px;
  border:1px solid #bbb;
  border-radius:10px;
  margin-top:6px;
}
button{
  width:100%;
  padding:14px;
  margin-top:18px;
  font-size:17px;
  border:0;
  border-radius:11px;
  background:#328e4c;
  color:white;
}
.back{background:#555}
.small{color:#666;line-height:1.45}
</style>
</head>
<body>
<div class="card">

<h1>⚙️ Налаштування Wi-Fi</h1>

<p class="small">
Після збереження ESP32 перезавантажиться і спробує
підключитися до нової Wi-Fi мережі.
</p>

<form action="/wifiSave" method="POST">

<label>Назва Wi-Fi (SSID)</label>
<input
type="text"
name="ssid"
maxlength="32"
value="
)rawliteral";

  page += wifiSSID;

  page += R"rawliteral(
"
required>

<label>Пароль Wi-Fi</label>
<input
type="password"
name="password"
maxlength="64"
placeholder="Введіть новий пароль">

<p class="small">
Якщо пароль не змінюється — залиште поле порожнім.
</p>

<button type="submit">💾 Зберегти Wi-Fi</button>

</form>

<a href="/"><button class="back">← Назад</button></a>

<hr>

<p class="small">
Якщо ESP32 не зможе підключитися до роутера, вона створить
власну мережу:<br><br>
<b>ESP32-S3-Garden-Setup</b><br>
Пароль: <b>garden1234</b><br><br>
Після підключення відкрийте:
<b>http://192.168.4.1</b>
</p>

</div>
</body>
</html>
)rawliteral";

  server.send(
    200,
    "text/html; charset=utf-8",
    page
  );
}

void handleWiFiSave() {
  if (!server.hasArg("ssid")) {
    server.send(
      400,
      "text/plain; charset=utf-8",
      "SSID не вказаний"
    );
    return;
  }

  String newSSID = server.arg("ssid");
  newSSID.trim();

  if (newSSID.length() == 0) {
    server.send(
      400,
      "text/plain; charset=utf-8",
      "SSID порожній"
    );
    return;
  }

  String newPassword = wifiPassword;

  if (server.hasArg("password")) {
    String p = server.arg("password");

    if (p.length() > 0) {
      newPassword = p;
    }
  }

  saveWiFiSettings(
    newSSID,
    newPassword
  );

  logEvent(
    "SYSTEM",
    "WIFI_SETTINGS_CHANGED",
    newSSID
  );

  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wi-Fi saved</title>
</head>
<body style="font-family:Arial;padding:20px">
<h2>✅ Wi-Fi збережено</h2>
<p>ESP32 перезавантажується...</p>
<p>Якщо нова мережа недоступна, підключіться до
<b>ESP32-S3-Garden-Setup</b> і відкрийте <b>192.168.4.1</b>.</p>
</body>
</html>
)rawliteral";

  server.send(
    200,
    "text/html; charset=utf-8",
    page
  );

  delay(1200);
  ESP.restart();
}


// =====================================================
// ВІДДАЛЕНИЙ ДОСТУП
// =====================================================

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);

  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];

    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }

  return out;
}

void loadRemoteSettings() {
  prefs.begin("remote", true);

  remoteAccessEnabled = prefs.getBool("enabled", false);
  remoteTlsEnabled = prefs.getBool("tls", true);
  remoteMqttHost = prefs.getString("host", "");
  remoteMqttPort = prefs.getUShort("port", remoteTlsEnabled ? 8883 : 1883);
  remoteMqttUser = prefs.getString("user", "");
  remoteMqttPassword = prefs.getString("pass", "");
  remoteMqttBase = prefs.getString("base", "garden/controller");
  remoteTlsVerify = prefs.getBool("tlsv", true);
  webPassword = prefs.getString("wpass", "");
  ntfyTopic = prefs.getString("ntfy", "");
  weatherEnabled = prefs.getBool("wx", false);
  weatherLat = prefs.getFloat("lat", 48.53f);
  weatherLon = prefs.getFloat("lon", 25.04f);

  prefs.end();

  remoteMqttHost.trim();
  remoteMqttUser.trim();
  remoteMqttBase.trim();

  if (remoteMqttBase.length() == 0) {
    remoteMqttBase = "garden/controller";
  }
}

void saveRemoteSettings() {
  prefs.begin("remote", false);

  prefs.putBool("enabled", remoteAccessEnabled);
  prefs.putBool("tls", remoteTlsEnabled);
  prefs.putString("host", remoteMqttHost);
  prefs.putUShort("port", remoteMqttPort);
  prefs.putString("user", remoteMqttUser);
  prefs.putString("pass", remoteMqttPassword);
  prefs.putString("base", remoteMqttBase);
  prefs.putBool("tlsv", remoteTlsVerify);
  prefs.putString("wpass", webPassword);
  prefs.putString("ntfy", ntfyTopic);
  prefs.putBool("wx", weatherEnabled);
  prefs.putFloat("lat", weatherLat);
  prefs.putFloat("lon", weatherLon);

  prefs.end();
}

void configureMqttTransport() {
  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }

  if (remoteTlsEnabled) {
    mqttSecureClient.stop();
    if (remoteTlsVerify) mqttSecureClient.setCACert(ISRG_ROOT_X1);
    else mqttSecureClient.setInsecure();
    mqttClient.setClient(mqttSecureClient);
  } else {
    mqttPlainClient.stop();
    mqttClient.setClient(mqttPlainClient);
  }

  if (remoteMqttHost.length() > 0) {
    mqttClient.setServer(remoteMqttHost.c_str(), remoteMqttPort);
  }

  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(4096);
}

void handleRemoteSettings() {
  String page;
  page.reserve(10000);

  page += R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#087b34">
<title>Віддалений доступ</title>
<style>
:root{
  --bg:#edf3ee;
  --card:#fff;
  --text:#14231a;
  --muted:#68776e;
  --line:#dce7df;
  --green:#087b34;
}
@media(prefers-color-scheme:dark){
  :root{
    --bg:#07110c;
    --card:#101b15;
    --text:#f3faf5;
    --muted:#9aae9f;
    --line:#26372d;
    --green:#18b85b;
  }
}
*{box-sizing:border-box}
body{
  margin:0;
  padding:14px;
  background:var(--bg);
  color:var(--text);
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;
}
.card{
  max-width:720px;
  margin:auto;
  background:var(--card);
  border:1px solid var(--line);
  border-radius:22px;
  padding:20px;
  box-shadow:0 10px 30px rgba(0,0,0,.12);
}
h1{margin-top:0;text-align:center}
.status{
  padding:13px;
  border-radius:14px;
  background:rgba(8,123,52,.10);
  margin:12px 0 18px;
  line-height:1.5;
}
label{
  display:block;
  margin-top:15px;
  font-weight:700;
}
input[type=text],
input[type=password],
input[type=number]{
  width:100%;
  margin-top:6px;
  padding:12px;
  border-radius:11px;
  border:1px solid var(--line);
  background:var(--bg);
  color:var(--text);
  font-size:16px;
}
.check{
  display:flex;
  align-items:center;
  gap:10px;
  padding:12px 0 2px;
}
.check input{width:21px;height:21px}
button{
  width:100%;
  margin-top:16px;
  padding:14px;
  border:0;
  border-radius:13px;
  background:linear-gradient(135deg,#087b34,#0ca84a);
  color:#fff;
  font-size:16px;
  font-weight:800;
}
.back{background:#4f5c55}
.small{
  color:var(--muted);
  font-size:14px;
  line-height:1.5;
}
.warn{
  color:#c98700;
  font-weight:700;
}
</style>
</head>
<body>
<div class="card">
<h1>🌐 Віддалений доступ</h1>

<div class="status">
<b>Стан:</b> )rawliteral";

  if (!remoteAccessEnabled) {
    page += "⚪ вимкнено";
  } else if (mqttClient.connected()) {
    page += "🟢 підключено до MQTT";
  } else {
    page += "🟠 увімкнено, очікується з'єднання";
  }

  page += R"rawliteral(
</div>

<p class="small">
ESP32 сама встановлює вихідне з'єднання з MQTT-брокером.
Тому <b>відкривати порт на домашньому роутері не потрібно</b>.
Через цей канал можна віддалено керувати поливом,
освітленням і котлом.
</p>

<form action="/remoteSave" method="POST">

<div class="check">
<input type="checkbox" id="enabled" name="enabled" value="1" )rawliteral";

  if (remoteAccessEnabled) page += "checked";

  page += R"rawliteral(>
<label for="enabled" style="margin:0">Увімкнути віддалений доступ</label>
</div>

<div class="check">
<input type="checkbox" id="tls" name="tls" value="1" )rawliteral";

  if (remoteTlsEnabled) page += "checked";

  page += R"rawliteral(>
<label for="tls" style="margin:0">TLS-шифрування</label>
</div>

<label>Адреса MQTT-брокера</label>
<input type="text" name="host" maxlength="120"
placeholder="broker.example.com"
value=")rawliteral";

  page += htmlEscape(remoteMqttHost);

  page += R"rawliteral(">

<label>Порт</label>
<input type="number" name="port" min="1" max="65535"
value=")rawliteral";

  page += String(remoteMqttPort);

  page += R"rawliteral(">

<label>Користувач</label>
<input type="text" name="user" maxlength="80"
value=")rawliteral";

  page += htmlEscape(remoteMqttUser);

  page += R"rawliteral(">

<label>Пароль</label>
<input type="password" name="password" maxlength="120"
placeholder="Залиште порожнім, щоб не змінювати">

<label>Базовий MQTT topic</label>
<input type="text" name="base" maxlength="100"
value=")rawliteral";

  page += htmlEscape(remoteMqttBase);

  page += R"rawliteral(">

<p class="small">
Для TLS типовий порт — <b>8883</b>, без TLS — <b>1883</b>.
Після збереження ESP32 перезавантажиться.
</p>

<div class="check">
<input type="checkbox" id="tlsv" name="tlsv" value="1" )rawliteral";

  if (remoteTlsVerify) page += "checked";

  page += R"rawliteral(>
<label for="tlsv" style="margin:0">Перевіряти сертифікат брокера (ISRG Root X1: HiveMQ Cloud та інші на Let's Encrypt)</label>
</div>

<hr>
<label>🔐 Пароль веб-інтерфейсу (логін: admin). Порожнє поле — не змінювати</label>
<input type="password" name="webpass" maxlength="40" placeholder="Мінімум 6 символів">
<div class="check">
<input type="checkbox" id="webpassclear" name="webpassclear" value="1">
<label for="webpassclear" style="margin:0">Зняти пароль (відкритий доступ у локальній мережі)</label>
</div>
<p class="small">З паролем також вмикається оновлення прошивки по Wi-Fi (OTA) з цим же паролем.</p>

<hr>
<label>🔔 ntfy.sh topic для push-сповіщень (порожньо — вимкнено)</label>
<input type="text" name="ntfy" maxlength="60" placeholder="garden-xxxxxxxx" value=")rawliteral";

  page += htmlEscape(ntfyTopic);

  page += R"rawliteral(">

<hr>
<div class="check">
<input type="checkbox" id="wx" name="wx" value="1" )rawliteral";

  if (weatherEnabled) page += "checked";

  page += R"rawliteral(>
<label for="wx" style="margin:0">🌦 Пропускати автополив, якщо за 6 годин очікується дощ (Open-Meteo)</label>
</div>
<label>Широта / довгота</label>
<input type="text" name="lat" maxlength="10" value=")rawliteral";

  page += String(weatherLat, 3);

  page += R"rawliteral(">
<input type="text" name="lon" maxlength="10" value=")rawliteral";

  page += String(weatherLon, 3);

  page += R"rawliteral(">

<button type="submit">💾 Зберегти віддалений доступ</button>
</form>

<a href="/"><button class="back">← Назад у контролер</button></a>
</div>
</body>
</html>
)rawliteral";

  server.sendHeader("Cache-Control", "no-store");

  server.send(
    200,
    "text/html; charset=utf-8",
    page
  );
}

void handleRemoteSave() {
  remoteAccessEnabled = server.hasArg("enabled");
  remoteTlsEnabled = server.hasArg("tls");
  remoteTlsVerify = server.hasArg("tlsv");

  if (server.hasArg("webpassclear")) {
    webPassword = "";
  } else if (server.hasArg("webpass")) {
    String wp = server.arg("webpass");
    if (wp.length() >= 6) webPassword = wp;
  }
  if (server.hasArg("ntfy")) {
    ntfyTopic = server.arg("ntfy");
    ntfyTopic.trim();
  }
  weatherEnabled = server.hasArg("wx");
  if (server.hasArg("lat")) {
    float la = server.arg("lat").toFloat();
    if (la >= -90.0f && la <= 90.0f && la != 0.0f) weatherLat = la;
  }
  if (server.hasArg("lon")) {
    float lo = server.arg("lon").toFloat();
    if (lo >= -180.0f && lo <= 180.0f && lo != 0.0f) weatherLon = lo;
  }

  if (server.hasArg("host")) {
    remoteMqttHost = server.arg("host");
    remoteMqttHost.trim();
  }

  if (server.hasArg("port")) {
    int p = server.arg("port").toInt();

    if (p >= 1 && p <= 65535) {
      remoteMqttPort = (uint16_t)p;
    } else {
      remoteMqttPort = remoteTlsEnabled ? 8883 : 1883;
    }
  }

  if (server.hasArg("user")) {
    remoteMqttUser = server.arg("user");
    remoteMqttUser.trim();
  }

  if (server.hasArg("password")) {
    String p = server.arg("password");

    if (p.length() > 0) {
      remoteMqttPassword = p;
    }
  }

  if (server.hasArg("base")) {
    remoteMqttBase = server.arg("base");
    remoteMqttBase.trim();

    while (remoteMqttBase.endsWith("/")) {
      remoteMqttBase.remove(remoteMqttBase.length() - 1);
    }

    if (remoteMqttBase.length() == 0) {
      remoteMqttBase = "garden/controller";
    }
  }

  saveRemoteSettings();

  logEvent(
    "SYSTEM",
    "REMOTE_ACCESS_SETTINGS_CHANGED",
    remoteAccessEnabled ? "ENABLED" : "DISABLED"
  );

  server.send(
    200,
    "text/html; charset=utf-8",
    R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Збережено</title>
</head>
<body style="font-family:Arial;padding:24px">
<h2>✅ Налаштування збережено</h2>
<p>ESP32 перезавантажується...</p>
</body>
</html>
)rawliteral"
  );

  delay(1200);
  ESP.restart();
}

// =====================================================
// MQTT / HOME ASSISTANT
// =====================================================

bool mqttEnabled() {
  return remoteAccessEnabled && remoteMqttHost.length() > 0;
}

void mqttPublish(const String& topic, const String& payload, bool retained = true) {
  if (!mqttClient.connected()) return;
  mqttClient.publish(topic.c_str(), payload.c_str(), retained);
}

void publishMQTTState(bool retained) {
  if (!mqttClient.connected()) return;

  String base = remoteMqttBase;

  mqttPublish(base + "/availability", "online", true);
  mqttPublish(base + "/state", buildStatusJson(), retained);

  mqttPublish(base + "/rain", isRaining() ? "ON" : "OFF", retained);
  mqttPublish(base + "/rain_blocked", rainBlocked() ? "ON" : "OFF", retained);

  mqttPublish(base + "/water/current_zone",
              String(currentZone >= 0 ? currentZone + 1 : 0),
              retained);

  for (int i = 0; i < LIGHT_COUNT; i++) {
    mqttPublish(
      base + "/light/" + String(i + 1) + "/state",
      getLightState(i) ? "ON" : "OFF",
      retained
    );
  }

  float effectiveTemp = getEffectiveRoomTemperature();

  if (effectiveTemp > -50.0) {
    mqttPublish(
      base + "/boiler/temperature",
      String(effectiveTemp, 2),
      retained
    );
  }

  mqttPublish(
    base + "/boiler/temperature_source",
    temperatureSourceText(),
    retained
  );

  mqttPublish(
    base + "/boiler/state",
    boilerState ? "ON" : "OFF",
    retained
  );

  mqttPublish(
    base + "/boiler/mode",
    String((int)boilerMode),
    retained
  );

  mqttPublish(
    base + "/boiler/setpoint",
    String(boilerCurrentSetpoint, 2),
    retained
  );

  mqttPublish(
    base + "/vacation/state",
    vacationMode ? "ON" : "OFF",
    retained
  );
}

void publishHomeAssistantDiscovery() {
  if (!mqttClient.connected()) return;
  if (!HA_DISCOVERY_ENABLED) return;

  String dev =
    "\"device\":{\"identifiers\":[\"esp32_garden\"],"
    "\"name\":\"ESP32 Garden\","
    "\"manufacturer\":\"DIY\","
    "\"model\":\"ESP32-S3 Garden Controller\"}";

  String base = remoteMqttBase;

  // Rain sensor
  {
    String p =
      "{\"name\":\"Garden Rain\","
      "\"unique_id\":\"esp32_garden_rain\","
      "\"state_topic\":\"" + base + "/rain\","
      "\"payload_on\":\"ON\","
      "\"payload_off\":\"OFF\","
      "\"device_class\":\"moisture\"," +
      dev + "}";

    mqttPublish(
      "homeassistant/binary_sensor/esp32_garden/rain/config",
      p,
      true
    );
  }

  // Temperature sensor
  {
    String p =
      "{\"name\":\"Garden Room Temperature\","
      "\"unique_id\":\"esp32_garden_temperature\","
      "\"state_topic\":\"" + base + "/boiler/temperature\","
      "\"unit_of_measurement\":\"°C\","
      "\"device_class\":\"temperature\","
      "\"state_class\":\"measurement\"," +
      dev + "}";

    mqttPublish(
      "homeassistant/sensor/esp32_garden/temperature/config",
      p,
      true
    );
  }

  // 3 light switches
  for (int i = 0; i < LIGHT_COUNT; i++) {
    int n = i + 1;

    String p =
      "{\"name\":\"Garden Light " + String(n) + "\","
      "\"unique_id\":\"esp32_garden_light_" + String(n) + "\","
      "\"state_topic\":\"" + base + "/light/" + String(n) + "/state\","
      "\"command_topic\":\"" + base + "/light/" + String(n) + "/set\","
      "\"payload_on\":\"ON\","
      "\"payload_off\":\"OFF\"," +
      dev + "}";

    mqttPublish(
      "homeassistant/switch/esp32_garden/light_" + String(n) + "/config",
      p,
      true
    );
  }

  mqttPublish(base + "/light/4/state", "OFF", true);
  // Remove obsolete retained Home Assistant Light 4 discovery.
  mqttPublish("homeassistant/switch/esp32_garden/light_4/config", "", true);

  // Boiler relay/state switch
  {
    String p =
      "{\"name\":\"Garden Boiler\","
      "\"unique_id\":\"esp32_garden_boiler\","
      "\"state_topic\":\"" + base + "/boiler/state\","
      "\"command_topic\":\"" + base + "/boiler/set\","
      "\"payload_on\":\"ON\","
      "\"payload_off\":\"OFF\"," +
      dev + "}";

    mqttPublish(
      "homeassistant/switch/esp32_garden/boiler/config",
      p,
      true
    );
  }

  // Water zone buttons
  for (int i = 0; i < 4; i++) {
    int n = i + 1;

    String p =
      "{\"name\":\"Start Water Zone " + String(n) + "\","
      "\"unique_id\":\"esp32_garden_water_" + String(n) + "\","
      "\"command_topic\":\"" + base + "/water/" + String(n) + "/set\","
      "\"payload_press\":\"START\"," +
      dev + "}";

    mqttPublish(
      "homeassistant/button/esp32_garden/water_" + String(n) + "/config",
      p,
      true
    );
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Захист: retained-команди (наприклад "START"), що приходять одразу після
  // підписки, ігноруємо — інакше після кожного перепідключення вони б виконувались знову.
  if (mqttSubscribedAt && millis() - mqttSubscribedAt < 1500UL) return;
  String t = String(topic);

  String p;
  p.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    p += (char)payload[i];
  }

  p.trim();
  p.toUpperCase();

  String base = remoteMqttBase;

  // Light commands
  for (int i = 0; i < LIGHT_COUNT; i++) {
    String expected =
      base + "/light/" + String(i + 1) + "/set";

    if (t == expected) {
      if (p == "ON") {
        setLight(i, true);
        logEvent("LIGHT", "MQTT_ON", "Канал " + String(i + 1));
      } else if (p == "OFF") {
        setLight(i, false);
        logEvent("LIGHT", "MQTT_OFF", "Канал " + String(i + 1));
      }

      publishMQTTState();
      return;
    }
  }

  // Water commands
  for (int i = 0; i < 4; i++) {
    String expected =
      base + "/water/" + String(i + 1) + "/set";

    if (t == expected && p == "START") {
      if (!rainBlocked()) {
        stopWatering();
        waterQueue[0] = i;
        queueCount = 1;
        queuePosition = 0;
        startNextWaterZone();
      }

      publishMQTTState();
      return;
    }
  }

  if (t == base + "/water/stop/set") {
    stopWatering();
    publishMQTTState();
    return;
  }

  if (t == base + "/boiler/set") {
    if (p == "ON") {
      boilerMode = BOILER_MANUAL;
    } else if (p == "OFF") {
      boilerMode = BOILER_OFF;
      setBoiler(false);
    }

    publishMQTTState();
    return;
  }

  if (t == base + "/boiler/mode/set") {
    if (p == "OFF" || p == "0") {
      boilerMode = BOILER_OFF;
      setBoiler(false);
    } else if (p == "MANUAL" || p == "1") {
      boilerMode = BOILER_MANUAL;
    } else if (p == "AUTO" || p == "2") {
      boilerMode = BOILER_AUTO;
    }

    publishMQTTState();
    return;
  }

  if (t == base + "/boiler/setpoint/set") {
    float requested = p.toFloat();

    if (requested >= 5.0 && requested <= 30.0) {
      // Round to the nearest 0.5 °C
      requested = roundf(requested * 2.0f) / 2.0f;

      heatingSetpoint = requested;
      heatingSetpointOverride = true;
      boilerMode = BOILER_AUTO;
      boilerCurrentSetpoint = heatingSetpoint;
      queueXiaomiSetpointDisplay(boilerCurrentSetpoint);

      saveSettings();

      logEvent(
        "HEATING",
        "MQTT_SETPOINT",
        String(heatingSetpoint, 1) + " C"
      );

      publishMQTTState();
    }

    return;
  }

  if (t == base + "/boiler/setpoint/reset") {
    heatingSetpointOverride = false;
    updateBoilerSetpointBySchedule();
    queueXiaomiSetpointDisplay(boilerCurrentSetpoint);
    saveSettings();

    logEvent(
      "HEATING",
      "MQTT_SETPOINT_SCHEDULE",
      "Повернення до розкладу"
    );

    publishMQTTState();
    return;
  }

  if (t == base + "/boiler/external_temperature/set") {
    float ext = p.toFloat();

    if (ext > -50.0 && ext < 80.0) {
      externalRoomTemperature = ext;
      lastExternalTempMillis = millis();

      logEvent(
        "BOILER",
        "EXTERNAL_TEMP",
        "MQTT " + String(ext, 2) + " C"
      );

      publishMQTTState();
    }

    return;
  }

  if (t == base + "/vacation/set") {
    vacationMode = (p == "ON" || p == "1");

    if (vacationMode) {
      boilerMode = BOILER_AUTO;
    }

    saveSettings();
    updateBoilerSetpointBySchedule();
    publishMQTTState();
    return;
  }
}

void ensureMQTT() {
  if (!mqttEnabled()) return;

  if (WiFi.status() != WL_CONNECTED) return;

  // Для перевірки сертифіката потрібен справжній час (NTP).
  if (remoteTlsEnabled && remoteTlsVerify && !mqttClient.connected()) {
    struct tm tt;
    if (!localTimeNow(&tt)) return;
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  if (
    millis() - lastMQTTReconnectAttempt <
    MQTT_RECONNECT_INTERVAL
  ) {
    return;
  }

  lastMQTTReconnectAttempt = millis();

  String clientId =
    "esp32-garden-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok;

  if (remoteMqttUser.length() > 0) {
    ok = mqttClient.connect(
      clientId.c_str(),
      remoteMqttUser.c_str(),
      remoteMqttPassword.c_str(),
      (remoteMqttBase + "/availability").c_str(),
      0,
      true,
      "offline"
    );
  } else {
    ok = mqttClient.connect(
      clientId.c_str(),
      (remoteMqttBase + "/availability").c_str(),
      0,
      true,
      "offline"
    );
  }

  if (!ok) {
    Serial.print("MQTT connect failed, state=");
    Serial.println(mqttClient.state());
    return;
  }

  Serial.println("MQTT connected");

  String base = remoteMqttBase;

  // Subscribe once to every topic below the controller base.
  // mqttCallback() still filters and handles only known command topics.
  // V35: лише командні топіки (раніше base/# — прилітали власні публікації).
  bool subOK = mqttClient.subscribe((base + "/+/set").c_str());
  subOK &= mqttClient.subscribe((base + "/+/+/set").c_str());
  subOK &= mqttClient.subscribe((base + "/boiler/setpoint/reset").c_str());
  mqttSubscribedAt = millis();
  Serial.println(subOK ? "MQTT SUB OK" : "MQTT SUB FAILED");

  publishHomeAssistantDiscovery();
  publishMQTTState(true);
}

void ensureWiFi() {
  if (wifiAPMode) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (
    millis() - lastWiFiReconnectAttempt <
    WIFI_RECONNECT_INTERVAL
  ) {
    return;
  }

  lastWiFiReconnectAttempt = millis();

  Serial.println("WiFi reconnect...");
  WiFi.disconnect();

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPassword.c_str()
  );
}

// =====================================================
// REST API
// =====================================================

void handleApiStatus() {
  sendJson(200, buildStatusJson());
}

void handleApiWater() {
  String action = server.hasArg("action")
    ? server.arg("action")
    : "";

  action.toLowerCase();

  if (action == "stop") {
    stopWatering();
    publishMQTTState();
    sendJson(200, "{\"ok\":true}");
    return;
  }

  if (action == "all") {
    if (rainBlocked()) {
      sendJson(409, "{\"ok\":false,\"error\":\"rain_blocked\"}");
      return;
    }

    stopWatering();
    waterAllActive = true;

    queueCount = 4;
    queuePosition = 0;

    for (int i = 0; i < 4; i++) {
      waterQueue[i] = i;
    }

    startNextWaterZone();
    publishMQTTState();

    sendJson(200, "{\"ok\":true}");
    return;
  }

  if (action == "alltogether") {
    if (rainBlocked()) {
      sendJson(409, "{\"ok\":false,\"error\":\"rain_blocked\"}");
      return;
    }

    stopWatering();

    waterAllActive = true;
    waterSimultaneousActive = true;
    waterSimStartMillis = millis();
    currentZone = -1;
    queueCount = 0;
    queuePosition = 0;

    for (int i = 0; i < 4; i++) {
      digitalWrite(WATER_PINS[i], RELAY_ON);
      waterSimZoneActive[i] = true;
      rememberWaterStart(i);
    }

    watering = true;

    Serial.println("WATER ALL ON");
    logEvent("WATER", "ALL_SIMULTANEOUS_START", "Зони 1-4 увімкнені одночасно");
    publishMQTTState();

    sendJson(200, "{\"ok\":true}");
    return;
  }

  if (action == "start" && server.hasArg("zone")) {
    int zone = server.arg("zone").toInt();

    if (zone < 1 || zone > 4) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad_zone\"}");
      return;
    }

    if (rainBlocked()) {
      sendJson(409, "{\"ok\":false,\"error\":\"rain_blocked\"}");
      return;
    }

    stopWatering();
    waterAllActive = false;

    waterQueue[0] = zone - 1;
    queueCount = 1;
    queuePosition = 0;

    startNextWaterZone();
    publishMQTTState();

    sendJson(200, "{\"ok\":true}");
    return;
  }

  sendJson(400, "{\"ok\":false,\"error\":\"bad_request\"}");
}

void handleApiLight() {
  if (!server.hasArg("channel") || !server.hasArg("state")) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad_request\"}");
    return;
  }

  int ch = server.arg("channel").toInt();
  String state = server.arg("state");

  state.toLowerCase();

  if (ch < 1 || ch > 4) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad_channel\"}");
    return;
  }

  if (state == "on" || state == "1") {
    setLight(ch - 1, true);
  } else if (state == "off" || state == "0") {
    setLight(ch - 1, false);
  } else {
    sendJson(400, "{\"ok\":false,\"error\":\"bad_state\"}");
    return;
  }

  logEvent("LIGHT", "API", "Канал " + String(ch) + " -> " + state);
  publishMQTTState();

  sendJson(200, "{\"ok\":true}");
}

void handleApiBoiler() {
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    mode.toLowerCase();

    if (mode == "off" || mode == "0") {
      boilerMode = BOILER_OFF;
      setBoiler(false);
    } else if (mode == "manual" || mode == "on" || mode == "1") {
      boilerMode = BOILER_MANUAL;
    } else if (mode == "auto" || mode == "2") {
      boilerMode = BOILER_AUTO;
    } else {
      sendJson(400, "{\"ok\":false,\"error\":\"bad_mode\"}");
      return;
    }
  }

  publishMQTTState();
  sendJson(200, buildStatusJson());
}


void handleApiExternalTemperature() {
  if (!server.hasArg("value")) {
    sendJson(400, "{\"ok\":false,\"error\":\"missing_value\"}");
    return;
  }

  float t = server.arg("value").toFloat();

  if (t <= -50.0 || t >= 80.0) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad_temperature\"}");
    return;
  }

  externalRoomTemperature = t;
  lastExternalTempMillis = millis();

  logEvent(
    "BOILER",
    "EXTERNAL_TEMP_API",
    String(t, 2) + " C"
  );

  publishMQTTState();

  sendJson(200, buildStatusJson());
}

void handleApiVacation() {
  if (!server.hasArg("state")) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad_request\"}");
    return;
  }

  String state = server.arg("state");
  state.toLowerCase();

  vacationMode =
    (state == "on" || state == "1" || state == "true");

  if (vacationMode) {
    boilerMode = BOILER_AUTO;
  }

  saveSettings();
  updateBoilerSetpointBySchedule();
  publishMQTTState();

  sendJson(200, buildStatusJson());
}

void handleApiLogClear() {
  bool ok = clearLogFile();

  sendJson(
    ok ? 200 : 500,
    ok ? "{\"ok\":true}" : "{\"ok\":false}"
  );
}

void handleApiLogEmail() {
  bool ok = sendLogByEmail();

  if (ok) {
    logEvent("SYSTEM", "LOG_EMAIL_SENT", smtpRecipient);
  }

  sendJson(
    ok ? 200 : 500,
    ok ? "{\"ok\":true}" : "{\"ok\":false}"
  );
}

// =====================================================
// WEB APP MANIFEST
// =====================================================

void handleManifest() {
  String manifest = R"json(
{
  "name":"Староміська 78а",
  "short_name":"78а",
  "start_url":"/",
  "scope":"/",
  "display":"standalone",
  "orientation":"portrait-primary",
  "background_color":"#07110c",
  "theme_color":"#087b34",
  "description":"Полив, освітлення та опалення",
  "icons":[
    {
      "src":"/pwa-icon-180.png",
      "sizes":"180x180",
      "type":"image/png",
      "purpose":"any"
    },
    {
      "src":"/pwa-icon-512.png",
      "sizes":"512x512",
      "type":"image/png",
      "purpose":"any maskable"
    }
  ]
}
)json";

  server.sendHeader("Cache-Control", "no-cache");
  server.send(
    200,
    "application/manifest+json; charset=utf-8",
    manifest
  );
}

void handlePwaIcon180() {
  server.send_P(
    200,
    "image/png",
    (PGM_P)PWA_ICON_180,
    sizeof(PWA_ICON_180)
  );
}

void handlePwaIcon512() {
  server.send_P(
    200,
    "image/png",
    (PGM_P)PWA_ICON_512,
    sizeof(PWA_ICON_512)
  );
}

void handleServiceWorker() {
  String sw = R"js(
const CACHE='staromiska78a-v1';
const CORE=['/','/manifest.json','/pwa-icon-180.png','/pwa-icon-512.png'];

self.addEventListener('install',event=>{
  event.waitUntil(
    caches.open(CACHE)
      .then(cache=>cache.addAll(CORE))
      .catch(()=>{})
  );
  self.skipWaiting();
});

self.addEventListener('activate',event=>{
  event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch',event=>{
  if(event.request.method!=='GET') return;

  event.respondWith(
    fetch(event.request)
      .then(response=>{
        const copy=response.clone();
        caches.open(CACHE).then(cache=>cache.put(event.request,copy)).catch(()=>{});
        return response;
      })
      .catch(()=>caches.match(event.request))
  );
});
)js";

  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Service-Worker-Allowed", "/");
  server.send(
    200,
    "application/javascript; charset=utf-8",
    sw
  );
}

void handlePwaInstall() {
  String page;
  page.reserve(10000);

  page += R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#087b34">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Староміська 78а">
<link rel="manifest" href="/manifest.json">
<link rel="apple-touch-icon" href="/pwa-icon-180.png">
<link rel="apple-touch-icon" href="/pwa-icon-180.png">
<title>Встановити — Староміська 78а</title>
<style>
:root{
  --bg:#edf3ee;
  --card:#fff;
  --text:#14231a;
  --muted:#6d7b72;
  --line:#dce7df;
  --green:#087b34;
}
@media(prefers-color-scheme:dark){
  :root{
    --bg:#07110c;
    --card:#101b15;
    --text:#f4faf5;
    --muted:#9aae9f;
    --line:#26372d;
    --green:#18b85b;
  }
}
*{box-sizing:border-box}
body{
  margin:0;
  padding:18px;
  background:var(--bg);
  color:var(--text);
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;
}
.wrap{max-width:650px;margin:auto}
.hero{
  text-align:center;
  background:linear-gradient(145deg,#087b34,#0c9a46);
  color:#fff;
  padding:28px 18px;
  border-radius:26px;
  box-shadow:0 12px 30px rgba(0,0,0,.18)
}
.icon{
  width:108px;
  height:108px;
  border-radius:25px;
  box-shadow:0 8px 22px rgba(0,0,0,.25);
  margin-bottom:12px
}
h1{margin:8px 0 3px}
.card{
  margin-top:14px;
  background:var(--card);
  border:1px solid var(--line);
  border-radius:22px;
  padding:18px;
}
.step{
  display:flex;
  gap:12px;
  align-items:flex-start;
  padding:11px 0;
  border-bottom:1px solid var(--line)
}
.step:last-child{border-bottom:0}
.num{
  width:32px;height:32px;border-radius:50%;
  display:grid;place-items:center;flex:0 0 32px;
  color:#fff;background:var(--green);font-weight:900
}
.big{
  display:block;
  width:100%;
  margin-top:14px;
  padding:15px;
  text-align:center;
  text-decoration:none;
  color:#fff;
  background:linear-gradient(135deg,#087b34,#0ca84a);
  border-radius:14px;
  font-weight:800
}
.install{
  display:none;
  width:100%;
  border:0;
  margin-top:14px;
  padding:15px;
  color:#fff;
  background:linear-gradient(135deg,#236bc4,#3898ed);
  border-radius:14px;
  font-size:16px;
  font-weight:800
}
.small{color:var(--muted);font-size:13px;line-height:1.45}
.badge{
  display:inline-block;
  padding:6px 10px;
  border-radius:99px;
  background:rgba(255,255,255,.16);
  font-size:13px;
  font-weight:800
}
</style>
</head>
<body>
<div class="wrap">
  <div class="hero">
    <img class="icon" src="/pwa-icon-180.png" alt="78а">
    <div class="badge">PWA • зберігається в ESP32-S3</div>
    <h1>Староміська 78а</h1>
    <div>Додаток керування будинком і садом</div>
  </div>

  <div class="card">
    <h2>📱 Встановлення на iPhone</h2>

    <div class="step">
      <div class="num">1</div>
      <div>Відкрийте цю сторінку саме у <b>Safari</b>.</div>
    </div>

    <div class="step">
      <div class="num">2</div>
      <div>Натисніть кнопку <b>Поділитися</b> ⬆️ в Safari.</div>
    </div>

    <div class="step">
      <div class="num">3</div>
      <div>Оберіть <b>«На початковий екран»</b>.</div>
    </div>

    <div class="step">
      <div class="num">4</div>
      <div>Натисніть <b>«Додати»</b>. З'явиться окрема іконка <b>78а</b>.</div>
    </div>

    <button id="installBtn" class="install">📲 Встановити додаток</button>

    <a class="big" href="/">Відкрити «Староміська 78а»</a>

    <p class="small">
      Веб-додаток та його іконки зберігаються безпосередньо
      у Flash ESP32-S3. Для локального керування iPhone має
      бути в мережі, де доступна ESP32.
    </p>
  </div>
</div>

<script>
let deferredPrompt=null;

window.addEventListener('beforeinstallprompt',e=>{
  e.preventDefault();
  deferredPrompt=e;

  const b=document.getElementById('installBtn');
  b.style.display='block';

  b.onclick=async()=>{
    if(!deferredPrompt) return;
    deferredPrompt.prompt();
    await deferredPrompt.userChoice;
    deferredPrompt=null;
    b.style.display='none';
  };
});

if('serviceWorker' in navigator){
  navigator.serviceWorker
    .register('/sw.js')
    .catch(()=>{});
}
</script>
</body>
</html>
)rawliteral";

  server.sendHeader(
    "Cache-Control",
    "no-store, no-cache, must-revalidate, max-age=0"
  );

  server.send(
    200,
    "text/html; charset=utf-8",
    page
  );
}

// =====================================================
// ПОЛИВ — РЕЛЕ
// =====================================================

void allWaterOff() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(WATER_PINS[i], RELAY_OFF);
  }
}

void waterValveOn(int zone) {
  allWaterOff();
  delay(300);

  if (zone >= 0 && zone < 4) {
    digitalWrite(WATER_PINS[zone], RELAY_ON);
  }
}

// =====================================================
// ОСВІТЛЕННЯ
// =====================================================

void setLight(int channel, bool state) {
  if (channel < 0 || channel >= LIGHT_COUNT) return;

  digitalWrite(
    LIGHT_PINS[channel],
    state ? RELAY_ON : RELAY_OFF
  );
}

bool getLightState(int channel) {
  if (channel < 0 || channel >= LIGHT_COUNT) return false;
  return digitalRead(LIGHT_PINS[channel]) == RELAY_ON;
}


// =====================================================
// ТЕСТ 8 РЕЛЕ
// =====================================================

void allRelayTestOff() {
  for (int i = 0; i < 4; i++) digitalWrite(WATER_PINS[i], RELAY_OFF);
  for (int i = 0; i < LIGHT_COUNT; i++) digitalWrite(LIGHT_PINS[i], RELAY_OFF);

  for (int i = 0; i < 7; i++) {
    relayTestState[i] = false;
  }
}

void setRelayTestChannel(int relayIndex, bool state) {
  if (relayIndex < 0 || relayIndex > 6) {
    return;
  }

  if (relayIndex < 4) {
    digitalWrite(
      WATER_PINS[relayIndex],
      state ? RELAY_ON : RELAY_OFF
    );
  } else {
    int lightIndex = relayIndex - 4;

    digitalWrite(
      LIGHT_PINS[lightIndex],
      state ? RELAY_ON : RELAY_OFF
    );
  }

  relayTestState[relayIndex] = state;

  logEvent(
    "RELAY_TEST",
    state ? "ON" : "OFF",
    "Relay " + String(relayIndex + 1)
  );
}

// =====================================================
// ДОЩ
// =====================================================

bool isRaining() {
  return digitalRead(RAIN_PIN) == RAIN_ACTIVE_LEVEL;
}

bool rainBlocked() {
  if (!rainProtectionEnabled) return false;

  if (isRaining()) return true;

  uint64_t now = getEpoch();

  // Якщо час ще не синхронізований — не використовуємо rainBlockUntil
  if (now < 100000) return false;

  if (now < rainBlockUntil) return true;

  return false;
}

// =====================================================
// ПОЛИВ — ЛОГІКА
// =====================================================

bool waterIntervalAllowsAuto(int zone) {
  if (zone < 0 || zone > 3) return false;
  if (!zones[zone].intervalEnabled) return true;
  if (lastWaterEpoch[zone] == 0) return true;

  uint64_t now = getEpoch();
  if (now < 100000) return true; // якщо час ще не синхронізовано — не блокуємо

  uint64_t intervalSec =
    ((uint64_t)zones[zone].intervalHours * 3600ULL) +
    ((uint64_t)zones[zone].intervalMinutes * 60ULL);

  if (intervalSec == 0) return true;
  return (now - lastWaterEpoch[zone]) >= intervalSec;
}

void rememberWaterStart(int zone) {
  if (zone < 0 || zone > 3) return;
  uint64_t now = getEpoch();
  if (now < 100000) return;
  lastWaterEpoch[zone] = now;

  prefs.begin("garden", false);
  String key = "w" + String(zone) + "last";
  prefs.putULong64(key.c_str(), now);
  prefs.end();
}

void stopWatering() {
  allWaterOff();

  watering = false;
  waterAllActive = false;
  waterSimultaneousActive = false;
  currentZone = -1;

  for (int i = 0; i < 4; i++) {
    waterSimZoneActive[i] = false;
  }

  queueCount = 0;
  queuePosition = 0;

  Serial.println("WATER STOP");
  logEvent("WATER", "STOP", "Полив зупинено");
  publishMQTTState();
}

void startWaterZone(int zone) {
  if (zone < 0 || zone > 3) return;

  if (rainBlocked()) {
    Serial.println("Water blocked by rain");
    stopWatering();
    return;
  }

  waterSimultaneousActive = false;
  for (int i = 0; i < 4; i++) waterSimZoneActive[i] = false;

  currentZone = zone;
  allWaterOff();
  digitalWrite(WATER_PINS[zone], RELAY_ON);

  zoneStartMillis = millis();
  zoneDurationMillis = (unsigned long)constrain((int)zones[zone].duration, 1, MAX_WATER_MINUTES) * 60000UL;
  watering = true;
  rememberWaterStart(zone);

  Serial.print("WATER ZONE ");
  Serial.print(zone + 1);
  Serial.print(" START, duration ");
  Serial.print(zones[zone].duration);
  Serial.println(" min");

  logEvent("WATER", "ZONE_START", "Зона " + String(zone + 1) + ", " + String(zones[zone].duration) + " хв");
  publishMQTTState();
}

void startNextWaterZone() {
  if (queuePosition >= queueCount) {
    Serial.println("WATER QUEUE DONE");
    stopWatering();
    return;
  }

  int zone = waterQueue[queuePosition++];
  Serial.print("WATER QUEUE -> ZONE ");
  Serial.println(zone + 1);
  startWaterZone(zone);
}

void processWatering() {
  if (relayTestMode) return;
  if (!watering) return;

  if (rainProtectionEnabled && isRaining()) {
    Serial.println("WATER STOP: RAIN");
    stopWatering();
    return;
  }

  // Режим: усі 4 зони одночасно.
  // Кожна зона вимикається після СВОЄЇ заданої тривалості.
  if (waterSimultaneousActive) {
    bool anyActive = false;
    unsigned long elapsed = millis() - waterSimStartMillis;

    for (int i = 0; i < 4; i++) {
      if (!waterSimZoneActive[i]) continue;

      unsigned long durationMs =
        (unsigned long)constrain((int)zones[i].duration, 1, MAX_WATER_MINUTES) * 60000UL;

      if (elapsed >= durationMs) {
        digitalWrite(WATER_PINS[i], RELAY_OFF);
        waterSimZoneActive[i] = false;

        Serial.print("W ZONE ");
        Serial.print(i + 1);
        Serial.println(" DONE");
        logEvent("WATER", "ZONE_DONE", "Одночасний полив — зона " + String(i + 1));
      } else {
        anyActive = true;
      }
    }

    // Повторна перевірка після можливих вимкнень.
    if (!anyActive) {
      for (int i = 0; i < 4; i++) {
        if (waterSimZoneActive[i]) {
          anyActive = true;
          break;
        }
      }
    }

    if (!anyActive) {
      Serial.println("WATER ALL DONE");
      stopWatering();
    }

    return;
  }

  // Звичайний режим: зони йдуть по черзі.
  if ((millis() - zoneStartMillis) >= zoneDurationMillis) {
    int finishedZone = currentZone;
    allWaterOff();
    watering = false;
    currentZone = -1;

    Serial.print("WATER ZONE ");
    Serial.print(finishedZone + 1);
    Serial.println(" DONE");

    // Без блокуючого delay: одразу запускаємо наступну зону черги.
    startNextWaterZone();
  }
}

void processRainSensor() {
  bool rain = isRaining();

  // Початок дощу
  if (rain && !previousRainState) {
    Serial.println("RAIN START");
    logEvent("RAIN", "START", "Дощ почався");

    if (rainProtectionEnabled) {
      stopWatering();
    }
  }

  // Кінець дощу
  if (!rain && previousRainState) {
    Serial.println("RAIN END");
    logEvent("RAIN", "END", "Дощ закінчився");

    uint64_t now = getEpoch();

    if (now > 100000) {
      rainBlockUntil =
        now + ((uint64_t)rainDelayHours * 3600ULL);

      prefs.begin("garden", false);
      prefs.putULong64("rainUntil", rainBlockUntil);
      prefs.end();
    }
  }

  previousRainState = rain;
}

// =====================================================
// КОТЕЛ
// =====================================================

void setBoiler(bool state) {
  if (boilerState == state) return;

  boilerState = state;

  digitalWrite(
    BOILER_PIN,
    state ? RELAY_ON : RELAY_OFF
  );

  Serial.print("BOILER: ");
  Serial.println(state ? "ON" : "OFF");
  logEvent(
    "BOILER",
    state ? "ON" : "OFF",
    "Температура " + String(getEffectiveRoomTemperature(), 1) +
    " C (" + temperatureSourceText() + "), уставка " +
    String(boilerCurrentSetpoint, 1) + " C"
  );
  publishMQTTState();
}

void readTemperature() {
  if (!extraTempSensorEnabled) {
    roomTemperature = -127.0;
    return;
  }

  if ((millis() - lastTempRead) < TEMP_READ_INTERVAL) return;

  lastTempRead = millis();

  tempSensors.requestTemperatures();

  float t = tempSensors.getTempCByIndex(0);

  if (t > -50.0 && t < 80.0) {
    roomTemperature = t;
  } else {
    roomTemperature = -127.0;
    Serial.println("DS18B20 ERROR");
  }
}

void updateBoilerSetpointBySchedule() {
  if (vacationMode) {
    boilerCurrentSetpoint = vacationTemperature;
    return;
  }

  if (heatingSetpointOverride) {
    boilerCurrentSetpoint = heatingSetpoint;
    return;
  }

  struct tm t;

  if (!localTimeNow(&t)) return;

  int nowMinutes =
    t.tm_hour * 60 + t.tm_min;

  int dayMinutes =
    boilerDayHour * 60 + boilerDayMinute;

  int nightMinutes =
    boilerNightHour * 60 + boilerNightMinute;

  // Працює і якщо інтервал переходить через північ
  if (dayMinutes < nightMinutes) {
    if (nowMinutes >= dayMinutes && nowMinutes < nightMinutes) {
      boilerCurrentSetpoint = boilerDayTemp;
    } else {
      boilerCurrentSetpoint = boilerNightTemp;
    }
  } else {
    if (nowMinutes >= dayMinutes || nowMinutes < nightMinutes) {
      boilerCurrentSetpoint = boilerDayTemp;
    } else {
      boilerCurrentSetpoint = boilerNightTemp;
    }
  }
}

void processBoiler() {
  if (relayTestMode) { setBoiler(false); return; }
  if (boilerMode == BOILER_OFF) {
    setBoiler(false);
    return;
  }

  if (boilerMode == BOILER_MANUAL) {
    static bool overheat = false;
    float tm = getEffectiveRoomTemperature();
    if (tm > -50.0f) {
      if (tm >= BOILER_MAX_SAFE_TEMP) {
        if (!overheat) {
          overheat = true;
          logEvent("BOILER", "OVERHEAT", String(tm, 1) + " C");
        }
      } else if (tm < BOILER_MAX_SAFE_TEMP - 1.0f) {
        overheat = false;
      }
    }
    setBoiler(!overheat);
    return;
  }

  if (boilerMode == BOILER_AUTO) {
    updateBoilerSetpointBySchedule();

    float effectiveTemp = getEffectiveRoomTemperature();

    // Безпечна поведінка: якщо немає ні бездротового датчика,
    // ні справного DS18B20 — котел вимикається.
    static unsigned long sensorLostSince = 0;
    static bool sensorLostNotified = false;
    if (effectiveTemp < -50.0) {
      if (!sensorLostSince) sensorLostSince = millis();
      if (!sensorLostNotified && millis() - sensorLostSince > 600000UL) {
        sensorLostNotified = true;
        logEvent("BOILER", "SENSOR_LOST", "");
      }
      setBoiler(false);
      return;
    }
    sensorLostSince = 0;
    if (sensorLostNotified) {
      sensorLostNotified = false;
      logEvent("BOILER", "SENSOR_OK", "");
    }

    float lower =
      boilerCurrentSetpoint - boilerHysteresis / 2.0;

    float upper =
      boilerCurrentSetpoint + boilerHysteresis / 2.0;

    if (effectiveTemp <= lower) {
      setBoiler(true);
    }

    if (effectiveTemp >= upper) {
      setBoiler(false);
    }
  }
}

void processVacationEnd() {
  if (!vacationMode) return;
  if (!vacationEndEnabled) return;
  if (vacationEndEpoch == 0) return;

  uint64_t now = getEpoch();

  if (now < 100000) return;

  if (now >= vacationEndEpoch) {
    vacationMode = false;
    vacationEndEnabled = false;
    vacationEndEpoch = 0;

    saveSettings();

    updateBoilerSetpointBySchedule();

    Serial.println("VACATION MODE AUTO OFF");
    logEvent("BOILER", "VACATION_AUTO_OFF", "Автоматичне завершення відпустки");
  }
}

// =====================================================
// ЗБЕРЕЖЕННЯ
// =====================================================

void saveSettings() {
  prefs.begin("garden", false);

  // Полив
  for (int i = 0; i < 4; i++) {
    String p = "w" + String(i);

    prefs.putBool((p + "en").c_str(), zones[i].enabled);
    prefs.putUChar((p + "h").c_str(), zones[i].hour);
    prefs.putUChar((p + "m").c_str(), zones[i].minute);
    prefs.putUShort((p + "dur").c_str(), zones[i].duration);
    prefs.putUChar((p + "days").c_str(), zones[i].days);
    prefs.putBool((p + "intEn").c_str(), zones[i].intervalEnabled);
    prefs.putUShort((p + "intH").c_str(), zones[i].intervalHours);
    prefs.putUChar((p + "intM").c_str(), zones[i].intervalMinutes);
    prefs.putULong64((p + "last").c_str(), lastWaterEpoch[i]);
  }

  // Освітлення
  for (int i = 0; i < LIGHT_COUNT; i++) {
    String p = "l" + String(i);

    prefs.putBool((p + "en").c_str(), lights[i].enabled);
    prefs.putUChar((p + "onh").c_str(), lights[i].onHour);
    prefs.putUChar((p + "onm").c_str(), lights[i].onMinute);
    prefs.putUChar((p + "offh").c_str(), lights[i].offHour);
    prefs.putUChar((p + "offm").c_str(), lights[i].offMinute);
    prefs.putUChar((p + "days").c_str(), lights[i].days);
  }

  // Дощ
  prefs.putBool("rainEn", rainProtectionEnabled);
  prefs.putUShort("rainDelay", rainDelayHours);
  prefs.putULong64("rainUntil", rainBlockUntil);

  // Котел
  prefs.putUChar("bDayH", boilerDayHour);
  prefs.putUChar("bDayM", boilerDayMinute);
  prefs.putFloat("bDayTemp", boilerDayTemp);

  prefs.putUChar("bNightH", boilerNightHour);
  prefs.putUChar("bNightM", boilerNightMinute);
  prefs.putFloat("bNightTemp", boilerNightTemp);

  prefs.putFloat("bHist", boilerHysteresis);

  prefs.putBool("heatOv", heatingSetpointOverride);
  prefs.putFloat("heatTemp", heatingSetpoint);

  // V28: Xiaomi кімнатні датчики x4
  prefs.putUChar("xHeatSrc", heatingXiaomiSource);
  for (uint8_t i=0;i<XIAOMI_SENSOR_COUNT;i++) {
    String p="x"+String(i);
    prefs.putBool((p+"en").c_str(), xiaomiSensorEnabled[i]);
    prefs.putString((p+"mac").c_str(), xiaomiMacs[i]);
    prefs.putString((p+"name").c_str(), xiaomiRoomNames[i]);
  }

  // Додатковий датчик температури DS18B20
  prefs.putBool("ds18En", extraTempSensorEnabled);

  // Відпустка
  prefs.putBool("vacation", vacationMode);
  prefs.putFloat("vacTemp", vacationTemperature);

  prefs.putBool("vacEndEn", vacationEndEnabled);
  prefs.putULong64("vacEnd", vacationEndEpoch);

  prefs.end();
}

void loadSettings() {
  prefs.begin("garden", true);

  // Полив
  for (int i = 0; i < 4; i++) {
    String p = "w" + String(i);

    zones[i].enabled =
      prefs.getBool((p + "en").c_str(), true);

    zones[i].hour =
      prefs.getUChar((p + "h").c_str(), 6);

    zones[i].minute =
      prefs.getUChar((p + "m").c_str(), i * 15);

    zones[i].duration =
      prefs.getUShort((p + "dur").c_str(), 10);

    // Пн/Ср/Пт
    zones[i].days =
      prefs.getUChar((p + "days").c_str(), 0b00101010);

    zones[i].intervalEnabled =
      prefs.getBool((p + "intEn").c_str(), false);

    zones[i].intervalHours =
      prefs.getUShort((p + "intH").c_str(), 24);

    zones[i].intervalMinutes =
      prefs.getUChar((p + "intM").c_str(), 0);

    lastWaterEpoch[i] =
      prefs.getULong64((p + "last").c_str(), 0);
  }

  // Освітлення
  for (int i = 0; i < LIGHT_COUNT; i++) {
    String p = "l" + String(i);

    lights[i].enabled =
      prefs.getBool((p + "en").c_str(), false);

    lights[i].onHour =
      prefs.getUChar((p + "onh").c_str(), 19);

    lights[i].onMinute =
      prefs.getUChar((p + "onm").c_str(), 0);

    lights[i].offHour =
      prefs.getUChar((p + "offh").c_str(), 23);

    lights[i].offMinute =
      prefs.getUChar((p + "offm").c_str(), 0);

    lights[i].days =
      prefs.getUChar((p + "days").c_str(), 0x7F);
  }

  // Дощ
  rainProtectionEnabled =
    prefs.getBool("rainEn", true);

  rainDelayHours =
    prefs.getUShort("rainDelay", 12);

  rainBlockUntil =
    prefs.getULong64("rainUntil", 0);

  personalMemo = prefs.getString("memo", "");

  // Котел
  boilerDayHour =
    prefs.getUChar("bDayH", 6);

  boilerDayMinute =
    prefs.getUChar("bDayM", 30);

  boilerDayTemp =
    prefs.getFloat("bDayTemp", 21.5);

  boilerNightHour =
    prefs.getUChar("bNightH", 23);

  boilerNightMinute =
    prefs.getUChar("bNightM", 0);

  boilerNightTemp =
    prefs.getFloat("bNightTemp", 19.0);

  boilerHysteresis =
    0.5f; // V34.3: fixed hysteresis 0.5 C (ignores old NVS value)

  heatingSetpointOverride =
    prefs.getBool("heatOv", false);

  heatingSetpoint =
    prefs.getFloat("heatTemp", 21.5);

  if (heatingSetpoint < 5.0 || heatingSetpoint > 30.0) {
    heatingSetpoint = 21.5;
  }
  // V32 migration: старі V27-V31 могли помилково записати 5.0 C через хибні Switch-події.
  // Один раз відкидаємо саме цей підозрілий override і повертаємось до розкладу.
  if (heatingSetpointOverride && heatingSetpoint <= 5.01f) {
    heatingSetpointOverride = false;
    heatingSetpoint = 21.5f;
    Serial.println("V33: stale 5.0 C override cleared");
  }

  // V28: Xiaomi кімнатні датчики x4
  heatingXiaomiSource = prefs.getUChar("xHeatSrc", 0);
  if (heatingXiaomiSource > 4) heatingXiaomiSource = 0;
  for (uint8_t i=0;i<XIAOMI_SENSOR_COUNT;i++) {
    String p="x"+String(i);
    xiaomiSensorEnabled[i]=prefs.getBool((p+"en").c_str(), i==0);
    xiaomiMacs[i]=prefs.getString((p+"mac").c_str(), i==0 ? "A4:C1:38:F9:68:BF" : "");
    xiaomiRoomNames[i]=prefs.getString((p+"name").c_str(), i==0 ? "Перший поверх" : ("Кімната "+String(i+1)));
  }

  // Додатковий датчик температури DS18B20
  extraTempSensorEnabled = prefs.getBool("ds18En", false);

  // Відпустка
  vacationMode =
    prefs.getBool("vacation", false);

  vacationTemperature =
    prefs.getFloat("vacTemp", 15.0);

  vacationEndEnabled =
    prefs.getBool("vacEndEn", false);

  vacationEndEpoch =
    prefs.getULong64("vacEnd", 0);

  prefs.end();

  // Безпечний старт
  boilerMode = BOILER_OFF;
}

// =====================================================
// РОЗКЛАД ПОЛИВУ
// =====================================================

void checkWaterSchedule() {
  if (relayTestMode) return;
  if (rainBlocked()) return;

  struct tm t;
  if (!localTimeNow(&t)) return;

  long dayKey =
    (t.tm_year + 1900) * 1000L + t.tm_yday;

  for (int i = 0; i < 4; i++) {
    if (!zones[i].enabled) continue;

    if (!(zones[i].days & (1 << t.tm_wday))) continue;

    if (
      t.tm_hour == zones[i].hour &&
      t.tm_min == zones[i].minute
    ) {
      if (lastWaterRunKey[i] == dayKey) continue;

      if (!waterIntervalAllowsAuto(i)) {
        Serial.print("AUTO WATER SKIP ZONE ");
        Serial.print(i + 1);
        Serial.println(": interval not elapsed");
        continue;
      }

      if (weatherSkipsWatering()) {
        lastWaterRunKey[i] = dayKey;
        logEvent("WATER", "WEATHER_SKIP",
                 "зона " + String(i + 1) + ", очікується " + String(weatherPrecip6h, 1) + " мм");
        continue;
      }

      lastWaterRunKey[i] = dayKey;

      if (!watering) {
        queueCount = 1;
        queuePosition = 0;
        waterQueue[0] = i;

        startNextWaterZone();
      } else {
        bool found = false;

        for (int q = queuePosition; q < queueCount; q++) {
          if (waterQueue[q] == i) {
            found = true;
            break;
          }
        }

        if (!found && queueCount < 4) {
          waterQueue[queueCount] = i;
          queueCount++;
        }
      }
    }
  }
}

// =====================================================
// РОЗКЛАД ОСВІТЛЕННЯ
// =====================================================

void checkLightSchedule() {
  if (relayTestMode) return;

  struct tm t;
  if (!localTimeNow(&t)) return;

  long dayKey =
    (t.tm_year + 1900) * 1000L + t.tm_yday;

  for (int i = 0; i < LIGHT_COUNT; i++) {
    if (!lights[i].enabled) continue;

    if (!(lights[i].days & (1 << t.tm_wday))) continue;

    // ON
    if (
      t.tm_hour == lights[i].onHour &&
      t.tm_min == lights[i].onMinute
    ) {
      if (lastLightOnKey[i] != dayKey) {
        lastLightOnKey[i] = dayKey;

        // Датчик дощу тут НЕ використовується
        setLight(i, true);

        Serial.print("LIGHT ON ");
        Serial.println(i + 1);
        logEvent("LIGHT", "ON", "Канал " + String(i + 1));
      }
    }

    // OFF
    if (
      t.tm_hour == lights[i].offHour &&
      t.tm_min == lights[i].offMinute
    ) {
      if (lastLightOffKey[i] != dayKey) {
        lastLightOffKey[i] = dayKey;

        setLight(i, false);

        Serial.print("LIGHT OFF ");
        Serial.println(i + 1);
        logEvent("LIGHT", "OFF", "Канал " + String(i + 1));
      }
    }
  }
}

// =====================================================
// HTML HELPERS
// =====================================================

String dayCheckbox(
  const String& prefix,
  int day,
  const String& label,
  uint8_t days
) {
  String s;

  s += "<label class='day'>";
  s += "<input type='checkbox' name='";
  s += prefix;
  s += "d";
  s += String(day);
  s += "' ";

  if (days & (1 << day)) {
    s += "checked ";
  }

  s += ">";
  s += label;
  s += "</label>";

  return s;
}

// =====================================================
// WEB PAGE
// =====================================================

String createPage() {
  struct tm t;

  String timeText = "--:--:--";
  String dateText = "--.--.----";

  if (localTimeNow(&t)) {
    char tb[20];
    char db[20];

    strftime(tb, sizeof(tb), "%H:%M:%S", &t);
    strftime(db, sizeof(db), "%d.%m.%Y", &t);

    timeText = tb;
    dateText = db;
  }

  String html;
  html.reserve(30000);

  html += R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#26733b">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="default">
<meta name="apple-mobile-web-app-title" content="Староміська 78а">
<link rel="manifest" href="/manifest.json">
<title>Староміська 78а</title>

<style>
html{color-scheme:light dark}
:root{--bg:#edf3ee;--surface:#fff;--surface2:#f5f8f6;--text:#13251a;--muted:#718078;--line:#dce7df;--green:#087b34;--shadow:0 10px 30px rgba(20,55,33,.12);--glass:rgba(255,255,255,.72)}
html[data-theme="dark"]{--bg:#07110c;--surface:#101b15;--surface2:#15241c;--text:#f3faf5;--muted:#9aae9f;--line:#26372d;--green:#17c45e;--shadow:0 12px 34px rgba(0,0,0,.42);--glass:rgba(10,20,14,.72)}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;transition:.28s}button,input,select{font:inherit}
.topbar{position:sticky;top:0;z-index:1000;height:62px;display:flex;align-items:center;gap:10px;padding:0 14px;background:linear-gradient(135deg,#056629,#07913d);color:#fff;box-shadow:0 3px 16px rgba(0,0,0,.22)}
.brandLeaf{width:34px;height:34px;border-radius:12px;display:grid;place-items:center;background:rgba(255,255,255,.16);font-size:22px}.topTitle{font-size:20px;font-weight:800}.topClock{margin-left:auto;font-weight:700;font-size:14px}.menuBtn{width:42px;height:42px;margin:0;padding:0;border:0;border-radius:12px;color:#fff;background:rgba(255,255,255,.12);font-size:24px}
.overlay{display:none;position:fixed;inset:0;z-index:1500;background:rgba(0,0,0,.48);backdrop-filter:blur(3px)}.overlay.show{display:block}.drawer{position:fixed;z-index:2000;top:0;left:-110%;height:100vh;width:min(88vw,365px);overflow:auto;background:linear-gradient(180deg,#071725,#0b1d2d 58%,#0c2018);color:#fff;box-shadow:10px 0 35px rgba(0,0,0,.45);transition:left .32s cubic-bezier(.2,.85,.25,1);padding-bottom:25px}.drawer.open{left:0}
.drawerHero{min-height:170px;padding:24px 18px;background:radial-gradient(circle at 80% 30%,rgba(42,222,110,.28),transparent 24%),linear-gradient(145deg,#073c27,#0c7440)}.drawerLogo{font-size:34px;animation:leafPulse 2.4s ease-in-out infinite}.drawerTitle{font-size:25px;font-weight:900;margin-top:8px}.drawerSub{font-size:13px;opacity:.75;margin-top:4px}.navWrap{padding:12px}.navHome,.navGroup{background:rgba(255,255,255,.045);border:1px solid rgba(255,255,255,.07);border-radius:16px;margin-bottom:9px;overflow:hidden}.navHome{display:flex;align-items:center;gap:12px;padding:14px;color:#fff;text-decoration:none;font-weight:750}.navGroup summary{list-style:none;display:grid;grid-template-columns:36px 1fr 22px;align-items:center;padding:14px;cursor:pointer;font-weight:750}.navGroup summary::-webkit-details-marker{display:none}.navIcon{font-size:22px;text-align:center}.navChev{font-size:22px;opacity:.65;transition:.2s}.navGroup[open] .navChev{transform:rotate(90deg)}.navSubs{padding:0 10px 10px}.navSubs a{display:block;text-decoration:none;color:#d9e6de;padding:10px 12px 10px 46px;border-radius:11px;font-size:14px}.themeMini{display:flex;align-items:center;justify-content:space-between;margin:8px 12px;padding:12px 14px;border-radius:14px;background:rgba(255,255,255,.06)}
.themeSwitch{position:relative;width:54px;height:30px}.themeSwitch input{opacity:0;width:0;height:0}.themeSlider{position:absolute;inset:0;background:#516158;border-radius:999px;transition:.25s}.themeSlider:before{content:"☀️";position:absolute;width:24px;height:24px;left:3px;top:3px;display:grid;place-items:center;border-radius:50%;background:#fff;font-size:13px;transition:.25s}.themeSwitch input:checked+.themeSlider{background:#197348}.themeSwitch input:checked+.themeSlider:before{content:"🌙";transform:translateX(24px);background:#122018}
.container{max-width:760px;margin:auto;padding:12px 12px 34px}.screen{display:none}.screen.active{display:block;animation:screenIn .28s ease}.card{background:var(--surface);padding:18px;margin-bottom:14px;border-radius:22px;box-shadow:var(--shadow);border:1px solid var(--line)}h1{margin:0}h2{margin:2px 0 15px;color:var(--green)}.status{text-align:center;font-size:17px;font-weight:700;padding:6px}.small{text-align:center;color:var(--muted)}.warning{color:#ff4e61;font-weight:800;text-align:center}.mode{padding:9px;text-align:center;font-weight:800}.zone{border-top:1px solid var(--line);padding-top:13px;margin-top:13px}.days{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}.day{background:var(--surface2);padding:7px 9px;border-radius:8px}hr{border:0;border-top:1px solid var(--line);margin:18px 0}input[type=number],input[type=datetime-local],input[type=text],input[type=password]{background:var(--surface2);color:var(--text);font-size:16px;padding:8px 9px;border:1px solid var(--line);border-radius:9px}input[type=number]{width:82px}button{width:100%;padding:14px;margin:5px 0;font-size:16px;border:0;border-radius:13px;color:#fff;background:linear-gradient(135deg,#079b43,#13b957)}.stop{background:#cf3e4b}.off{background:#4b5550}.lightOn{background:linear-gradient(135deg,#079b43,#13b957)}.waterActive{background:linear-gradient(135deg,#168fd3,#27b8f2)!important}.blue{background:#2879b8}
.waterZone{border:1px solid var(--line);background:var(--surface2);border-radius:18px;padding:14px;margin:14px 0}.waterZone h3{margin:0 0 12px;font-size:22px}.waterToggle{display:flex;align-items:center;gap:9px;font-weight:800;margin-bottom:14px}.waterToggle input{width:22px;height:22px;flex:0 0 auto}.waterBlockTitle{font-weight:900;margin:10px 0 8px}.waterGrid2{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.waterField{min-width:0}.waterField label{display:block;font-size:13px;color:var(--muted);font-weight:800;margin:0 0 6px}.waterField input[type=number]{width:100%;min-width:0;height:44px;text-align:center;font-size:18px;font-weight:800}.waterDuration{display:grid;grid-template-columns:minmax(0,1fr) 96px 28px;gap:9px;align-items:center;margin:14px 0}.waterDuration span:first-child{font-weight:800}.waterDuration input[type=number]{width:100%;height:44px;text-align:center;font-size:18px;font-weight:800}.waterIntervalToggle{display:flex;align-items:flex-start;gap:9px;font-weight:800;line-height:1.3;margin:14px 0 10px}.waterIntervalToggle input{width:22px;height:22px;flex:0 0 auto;margin-top:1px}.waterNote{font-size:12px;line-height:1.4;text-align:left;color:var(--muted);margin:9px 0 12px}.waterZone .days{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin-top:10px}.waterZone .day{display:flex;align-items:center;justify-content:center;gap:4px;padding:9px 4px;text-align:center;white-space:nowrap}.waterZone .day input{width:18px;height:18px}@media(max-width:430px){.container{padding-left:8px;padding-right:8px}.card{padding:14px}.waterZone{padding:12px}.waterZone h3{font-size:20px}.waterGrid2{gap:8px}.waterDuration{grid-template-columns:minmax(0,1fr) 82px 24px}.waterZone .day{font-size:14px;padding:8px 2px}}
.homeCard{padding:0;overflow:hidden}.hero{min-height:190px;position:relative;overflow:hidden;background:linear-gradient(180deg,rgba(7,17,12,.04),rgba(7,17,12,.25)),linear-gradient(150deg,#87c78b 0,#d9ead3 45%,#7db678 100%)}html[data-theme="dark"] .hero{background:linear-gradient(180deg,rgba(0,0,0,.08),rgba(0,0,0,.38)),linear-gradient(150deg,#16301f 0,#153b2b 45%,#0a2118 100%)}.heroSun{position:absolute;width:52px;height:52px;border-radius:50%;right:28px;top:25px;background:#ffd657;box-shadow:0 0 35px rgba(255,214,87,.55);animation:sunGlow 2.5s ease-in-out infinite}.heroHill1,.heroHill2{position:absolute;left:-10%;width:120%;border-radius:50% 50% 0 0}.heroHill1{height:100px;bottom:-25px;background:#559d58}.heroHill2{height:75px;bottom:-34px;background:#2d793e}.heroTree{position:absolute;left:34px;bottom:42px;width:16px;height:78px;background:#60462f;border-radius:6px}.heroTree:before,.heroTree:after{content:"";position:absolute;border-radius:50%;background:#23914d}.heroTree:before{width:72px;height:72px;left:-28px;top:-42px}.heroTree:after{width:54px;height:54px;left:-45px;top:-25px}.sprinkler{position:absolute;right:78px;bottom:32px;width:10px;height:35px;background:#65746b;border-radius:5px}.sprinkler:before,.sprinkler:after{content:"";position:absolute;width:70px;height:38px;bottom:24px;border:3px solid #58baff;border-color:#58baff transparent transparent transparent;border-radius:50%;animation:spray 1.6s ease-in-out infinite}.sprinkler:before{right:2px}.sprinkler:after{left:2px;transform:scaleX(-1)}.heroLabel{position:absolute;left:18px;top:18px;z-index:5;padding:9px 12px;border-radius:14px;background:var(--glass);backdrop-filter:blur(8px);font-weight:800}
.heroMeta{position:absolute;z-index:6;left:112px;right:12px;top:83px;display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:6px}
.heroMetric{min-width:0;padding:7px 5px;border-radius:12px;background:rgba(255,255,255,.72);backdrop-filter:blur(7px);text-align:center;font-size:11px;color:#405247}
.heroMetric b{display:block;margin-top:2px;font-size:14px;color:#14251a;white-space:nowrap}
.heroDate{display:block;font-size:11px;font-weight:700;opacity:.72;margin-top:2px}
.hero.time-dawn{background:linear-gradient(180deg,#f6b36b 0,#f6d6a0 48%,#83b879 100%)}
.hero.time-day{background:linear-gradient(180deg,#78c8f0 0,#d9eff6 50%,#79b779 100%)}
.hero.time-evening{background:linear-gradient(180deg,#e77d63 0,#f3b071 48%,#587b66 100%)}
.hero.time-night{background:linear-gradient(180deg,#08152d 0,#172a49 55%,#173b2a 100%)}
.hero.time-night .heroSun{background:#eef4ff;box-shadow:0 0 28px rgba(225,236,255,.75)}
.hero.time-dawn .heroSun,.hero.time-evening .heroSun{background:#ffb24a;box-shadow:0 0 35px rgba(255,147,64,.75)}
.hero.season-spring .heroTree:before,.hero.season-spring .heroTree:after{background:#55b85d}
.hero.season-summer .heroTree:before,.hero.season-summer .heroTree:after{background:#178d43}
.hero.season-autumn .heroTree:before,.hero.season-autumn .heroTree:after{background:#d8862b}
.hero.season-winter .heroTree:before,.hero.season-winter .heroTree:after{background:#eef3f5;box-shadow:0 3px 0 rgba(198,215,224,.9)}
.hero.season-winter .heroHill1{background:#dce8ec}.hero.season-winter .heroHill2{background:#b9cdd4}
.hero.season-autumn .heroHill1{background:#8e9a55}.hero.season-autumn .heroHill2{background:#647840}
@media(max-width:430px){.heroMeta{left:105px;right:8px;gap:4px}.heroMetric{font-size:10px;padding:6px 3px}.heroMetric b{font-size:13px}}
.switchRow{display:flex;align-items:center;justify-content:space-between;gap:14px;padding:14px 0}.switchRow input{width:24px;height:24px;accent-color:#2f80ed}
.sensorGrid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin:12px 0}.sensorCard{padding:14px;border:1px solid var(--line);border-radius:16px;background:var(--surface2)}.sensorTemp{font-size:30px;font-weight:900}.sensorMeta{font-size:12px;color:var(--muted);margin-top:4px}.sensorCard input{box-sizing:border-box;width:100%;margin:5px 0;padding:10px;border-radius:10px;border:1px solid var(--line)}@media(max-width:650px){.sensorGrid{grid-template-columns:1fr}}.dashboard{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;padding:14px}.dashTile{min-width:0;width:auto;box-sizing:border-box;min-height:138px;padding:16px;border-radius:20px;background:var(--surface);border:1px solid var(--line);box-shadow:0 7px 20px rgba(0,0,0,.08);cursor:pointer}.tileIcon{width:58px;height:58px;border-radius:18px;display:grid;place-items:center;font-size:32px;margin-bottom:9px}.tileTitle{font-size:18px;font-weight:900}.tileSub{font-size:12px;color:var(--muted);margin-top:4px}.waterTile .tileIcon{background:rgba(24,151,255,.13);animation:waterFloat 2.2s ease-in-out infinite}.lightTile .tileIcon{background:rgba(255,176,0,.14);animation:lampGlow 1.8s ease-in-out infinite}.boilerTile .tileIcon{background:rgba(255,91,101,.13);animation:flamePulse 1.1s ease-in-out infinite}.logTile .tileIcon{background:rgba(154,103,255,.13)}.settingsTile .tileIcon{background:rgba(110,160,255,.13);animation:gearSoft 7s linear infinite}.infoTile .tileIcon{background:rgba(25,210,100,.13)}.quickStatus{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:0 14px 14px;padding:12px;border-radius:18px;background:var(--surface2);border:1px solid var(--line)}.qItem{text-align:center;font-size:12px;color:var(--muted)}.qItem b{display:block;color:var(--text);font-size:13px;margin-top:3px}.screenHeader{display:flex;align-items:center;gap:10px;margin-bottom:12px}.backBtn{width:42px;height:42px;border-radius:13px;margin:0;padding:0;background:var(--surface);color:var(--text);border:1px solid var(--line)}.screenHeaderTitle{font-size:24px;font-weight:900}.screenHeaderSub{font-size:12px;color:var(--muted)}.sectionAnchor{scroll-margin-top:76px}
.thermostatBox{margin:14px 0 16px;padding:18px 14px;border-radius:20px;background:linear-gradient(145deg,var(--surface2),var(--surface));border:1px solid var(--line);text-align:center}
.thermostatLabel{color:var(--muted);font-size:13px;font-weight:750;margin-bottom:8px}
.thermostatValue{font-size:42px;line-height:1;font-weight:900;letter-spacing:-1px;color:var(--text);margin-bottom:14px}
.thermostatValue small{font-size:20px;color:var(--muted);font-weight:800}
.tempControls{display:grid;grid-template-columns:1fr 1fr;gap:12px;max-width:360px;margin:0 auto}
.tempBtn{display:flex;align-items:center;justify-content:center;min-height:58px;margin:0;border-radius:18px;font-size:31px;font-weight:900;text-decoration:none;color:white;box-shadow:0 7px 18px rgba(0,0,0,.12)}
.tempMinus{background:linear-gradient(135deg,#2478b8,#349bea)}
.tempPlus{background:linear-gradient(135deg,#e45a4f,#ff795d)}
.scheduleBtn{display:inline-block;margin-top:13px;padding:9px 13px;border-radius:12px;background:var(--surface2);border:1px solid var(--line);color:var(--text);text-decoration:none;font-size:13px;font-weight:800}
.overrideNote{margin-top:9px;color:var(--muted);font-size:12px}
@keyframes screenIn{from{opacity:0;transform:translateY(7px)}to{opacity:1;transform:none}}@keyframes leafPulse{0%,100%{transform:scale(1)}50%{transform:scale(1.08)}}@keyframes sunGlow{0%,100%{box-shadow:0 0 28px rgba(255,214,87,.42)}50%{box-shadow:0 0 48px rgba(255,214,87,.8)}}@keyframes spray{0%,100%{opacity:.45;transform:translateY(1px)}50%{opacity:1;transform:translateY(-3px)}}@keyframes waterFloat{0%,100%{transform:translateY(0)}50%{transform:translateY(-5px)}}@keyframes lampGlow{0%,100%{filter:drop-shadow(0 0 2px #ffc72c)}50%{filter:drop-shadow(0 0 13px #ffc72c)}}@keyframes flamePulse{0%,100%{transform:scale(1)}50%{transform:scale(1.08,.95)}}@keyframes gearSoft{from{transform:rotate(0)}to{transform:rotate(360deg)}}@media(min-width:760px){.dashboard{grid-template-columns:repeat(3,1fr)}}
</style>
</head>

<body>
<div class="topbar"><div class="brandLeaf">🍃</div><div class="topTitle">Староміська 78а</div><div id="topClock" class="topClock">--:--</div><button class="menuBtn" onclick="toggleMenu()">☰</button></div>
<div id="overlay" class="overlay" onclick="closeMenu()"></div>
<nav id="drawer" class="drawer"><div class="drawerHero"><div class="drawerLogo">🍃</div><div class="drawerTitle">Староміська 78а</div></div><div class="navWrap">
<a class="navHome" href="#" onclick="showSection('home');return false;"><span class="navIcon">🏠</span><span>Головна</span></a>
<details class="navGroup"><summary><span class="navIcon">💧</span><span>Полив</span><span class="navChev">›</span></summary><div class="navSubs"><a href="#" onclick="showSection('water','water-manual');return false;">Ручний полив</a><a href="#" onclick="showSection('water','water-schedule');return false;">Розклад</a><a href="#" onclick="showSection('water','rain');return false;">Датчик дощу</a></div></details>
<details class="navGroup"><summary><span class="navIcon">💡</span><span>Освітлення</span><span class="navChev">›</span></summary><div class="navSubs"><a href="#" onclick="showSection('lights','lights-manual');return false;">Ручне керування</a><a href="#" onclick="showSection('lights','lights-schedule');return false;">Розклад</a></div></details>
<details class="navGroup"><summary><span class="navIcon">🔥</span><span>Опалення</span><span class="navChev">›</span></summary><div class="navSubs"><a href="#" onclick="showSection('boiler','boiler-control');return false;">Стан / режими</a><a href="#" onclick="showSection('heatmanual','heating-manual');return false;">🔥 Ручний ON</a><a href="#" onclick="showSection('boiler','boiler-settings');return false;">День / ніч</a><a href="#" onclick="showSection('boiler','boiler-settings');return false;">Відпустка</a><a href="#" onclick="showSection('boiler','xiaomi-rooms');return false;">🌡️ Кімнатні датчики</a></div></details>
<details class="navGroup"><summary><span class="navIcon">📊</span><span>Журнал</span><span class="navChev">›</span></summary><div class="navSubs"><a href="#" onclick="showSection('journal','journal');return false;">Події / графіки</a></div></details>
<details class="navGroup"><summary><span class="navIcon">⚙️</span><span>Налаштування</span><span class="navChev">›</span></summary><div class="navSubs"><a href="/wifi">Wi‑Fi</a><a href="/remote">🌐 Віддалений доступ</a><a href="/pwa">📱 Встановити PWA</a><a href="#" onclick="showSection('settings','integration');return false;">MQTT / Home Assistant</a><a href="#" onclick="showSection('settings','extra-temp');return false;">🌡️ Додатковий датчик</a><a href="/relayTest">Тест 7 реле</a><a href="#" onclick="showSection('settings','gpio');return false;">GPIO</a></div></details>
<a class="navHome" href="#" onclick="showSection('memo','memo');return false;"><span class="navIcon">📝</span><span>Памʼятка</span></a>
<a class="navHome" href="#" onclick="showSection('authors','authors');return false;"><span class="navIcon">👨‍💻</span><span>Автори</span></a>
</div><div class="themeMini"><span>🌗 Темна тема</span><label class="themeSwitch"><input id="themeToggle" type="checkbox" onchange="toggleTheme(this.checked)"><span class="themeSlider"></span></label></div></nav>
<div class="container">

<div class="card sectionAnchor screen section-home active homeCard" id="home"><div class="hero" id="gardenHero"><div class="heroLabel">🌿 Мій сад<span class="heroDate" id="heroDate">--.--.----</span></div><div class="heroSun"></div><div class="heroTree"></div><div class="sprinkler"></div><div class="heroHill1"></div><div class="heroHill2"></div><div class="heroMeta"><div class="heroMetric">🕒 Час<b id="heroClock">--:--</b></div><div class="heroMetric">🌡️ Надворі<b id="outTemp">--.- °C</b></div><div class="heroMetric">🏠 В хаті<b id="inTemp">--.- °C</b></div></div></div><div class="dashboard">
<div class="dashTile waterTile" onclick="showSection('water')"><div class="tileIcon">💧</div><div class="tileTitle">Полив</div><div class="tileSub">Авто / Ручний</div></div>
<div class="dashTile lightTile" onclick="showSection('lights')"><div class="tileIcon">💡</div><div class="tileTitle">Освітлення</div><div class="tileSub">Сад / Двір</div></div>
<div class="dashTile boilerTile" onclick="showSection('boiler')"><div class="tileIcon">🔥</div><div class="tileTitle">Опалення</div><div class="tileSub">Температура / Авто</div></div>
<div class="dashTile logTile" onclick="showSection('journal')"><div class="tileIcon">📊</div><div class="tileTitle">Журнал</div><div class="tileSub">Події / Графіки</div></div>
<div class="dashTile settingsTile" onclick="showSection('settings')"><div class="tileIcon">⚙️</div><div class="tileTitle">Налаштування</div><div class="tileSub">Wi‑Fi / Remote / MQTT / Тема</div></div>
<div class="dashTile infoTile" onclick="showSection('settings','integration')"><div class="tileIcon">ⓘ</div><div class="tileTitle">Інформація</div><div class="tileSub">IP / Час / API</div></div></div>
<div class="quickStatus"><div class="qItem">📶 Wi‑Fi<b>Підключено</b></div><div class="qItem">🌐 IP<b id="ipMini">локальна</b></div><div class="qItem">🕒 Час<b id="clockMini">--:--:--</b></div></div>
)rawliteral";

  html += "<div class='status'>" + dateText + " &nbsp; " + timeText + "</div>";
  html += "<div class='small'>Firmware: Garden_U_V34.5 | Build: " __DATE__ " " __TIME__ " | Котел: GPIO12</div>";

  if (currentZone >= 0) {
    html += "<div class='status'>💧 Полив: зона ";
    html += String(currentZone + 1);
    html += "</div>";
  } else {
    html += "<div class='status'>💧 Полив вимкнений</div>";
  }

  if (isRaining()) {
    html += "<div class='status'>🌧️ Дощ</div>";
  } else if (rainBlocked()) {
    html += "<div class='status'>⏳ Пауза після дощу</div>";
  } else {
    html += "<div class='status'>☀️ Полив дозволений</div>";
  }

  html += "</div>";

  // ===================================================
  // РУЧНИЙ ПОЛИВ
  // ===================================================

  html += R"rawliteral(
<div class="card sectionAnchor screen section-water" id="water-manual"><div class="screenHeader"><button class="backBtn" onclick="showSection('home')">‹</button><div><div class="screenHeaderTitle">💧 Полив</div><div class="screenHeaderSub">4 зони • ручне та автоматичне</div></div></div>
<h2>💧 Ручний полив</h2>
)rawliteral";

  for (int i = 0; i < 4; i++) {
    html += "<button id='waterBtn" + String(i + 1) + "' onclick='waterZone(" + String(i + 1) + ")'";
    if (waterAllActive || currentZone == i) html += " class='waterActive'";
    html += ">Зона " + String(i + 1) + " — " + String(zones[i].duration) + " хв</button>";
  }

  html += R"rawliteral(
<button id="waterAllBtn" onclick="waterAll()">▶ Полити всі зони по черзі</button>
<button id="waterAllTogetherBtn" class="blue" onclick="waterAllTogether()">⚡ Полити всі 4 зони одночасно</button>
<button class="stop" onclick="waterStop()">■ STOP</button>
</div>
)rawliteral";

  // ===================================================
  // ОСВІТЛЕННЯ
  // ===================================================

  html += R"rawliteral(
<div class="card sectionAnchor screen section-lights" id="lights-manual"><div class="screenHeader"><button class="backBtn" onclick="showSection('home')">‹</button><div><div class="screenHeaderTitle">💡 Освітлення</div><div class="screenHeaderSub">3 канали • сад та двір</div></div></div>
<h2>💡 Освітлення</h2>
)rawliteral";

  for (int i = 0; i < LIGHT_COUNT; i++) {
    html += "<h3>Світло " + String(i + 1) + " — ";

    if (getLightState(i)) {
      html += "🟢 ON</h3>";
      html += "<button id='lightBtn" + String(i + 1) + "' class='off' onclick='setLightAjax(" + String(i + 1) + ",0)'>Вимкнути</button>";
    } else {
      html += "⚫ OFF</h3>";
      html += "<button id='lightBtn" + String(i + 1) + "' class='lightOn' onclick='setLightAjax(" + String(i + 1) + ",1)'>Увімкнути</button>";
    }
  }

  html += "</div>";

  // ===================================================
  // КОТЕЛ
  // ===================================================

  updateBoilerSetpointBySchedule();

  html += R"rawliteral(
<div class="card sectionAnchor screen section-boiler" id="boiler-control"><div class="screenHeader"><button class="backBtn" onclick="showSection('home')">‹</button><div><div class="screenHeaderTitle">🔥 Опалення</div><div class="screenHeaderSub">температура • день/ніч • відпустка</div></div></div>
<h2>🔥 Опалення</h2>
)rawliteral";

  html += "<div class='status'>Температура кімнати: ";

  float effectiveTemp = getEffectiveRoomTemperature();

  if (effectiveTemp > -50.0) {
    html += String(effectiveTemp, 1) + " °C";
    html += " <span style='font-size:13px;color:#666'>(";
    html += temperatureSourceText();
    html += ")</span>";
  } else {
    html += "❗ Немає датчика температури";
  }

  html += "</div>";

  html += "<div class='status'>Задана температура: ";
  html += String(boilerCurrentSetpoint, 1);
  html += " °C</div>";

  html += "<div class='status'>Опалення: ";
  html += boilerState ? "🔥 ПРАЦЮЄ" : "⚫ ВИМКНЕНО";
  html += "</div>";

  html += "<div class='mode'>Режим: ";

  if (boilerMode == BOILER_OFF) html += "OFF";
  else if (boilerMode == BOILER_MANUAL) html += "РУЧНИЙ ON";
  else html += "AUTO";

  html += "</div>";

  if (vacationMode) {
    html += "<div class='status'>🏖️ ВІДПУСТКА — ";
    html += String(vacationTemperature, 1);
    html += " °C</div>";

    if (vacationEndEnabled && vacationEndEpoch > 0) {
      time_t raw = (time_t)vacationEndEpoch;
      struct tm vt;
      localtime_r(&raw, &vt);

      char vb[40];
      strftime(vb, sizeof(vb), "%d.%m.%Y %H:%M", &vt);

      html += "<div class='small'>Автовимкнення: ";
      html += vb;
      html += "</div>";
    }
  }

  html += R"rawliteral(
<button class="stop" onclick="setBoilerMode('off')">OFF</button>
<button onclick="showSection('heatmanual','heating-manual')">🔥 Ручний ON</button>
<button class="blue" onclick="setBoilerMode('auto')">🌡 AUTO</button>
)rawliteral";

  if (!vacationMode) {
    html += R"rawliteral(
<a href="/vacation?state=1"><button class="lightOn">🏖️ Увімкнути ВІДПУСТКУ</button></a>
)rawliteral";
  } else {
    html += R"rawliteral(
<a href="/vacation?state=0"><button>🏠 Повернути звичайний режим</button></a>
)rawliteral";
  }

  html += "</div>";

  // ===================================================
  // ОПАЛЕННЯ — РУЧНИЙ ON
  // ===================================================

  html += R"rawliteral(
<div class="card sectionAnchor screen section-boiler" id="xiaomi-rooms">
<h2>🌡️ Кімнатні датчики Xiaomi</h2>
<p class="small">До 4 датчиків MJWSD05MMC / BTHome. Введіть назву кімнати та MAC. Перший датчик уже заповнений.</p>
<div id="xiaomiRoomCards">
<div class="sensorGrid">
<div class="sensorCard" id="xs0"></div><div class="sensorCard" id="xs1"></div><div class="sensorCard" id="xs2"></div><div class="sensorCard" id="xs3"></div>
</div>
<h3>Датчик для керування опаленням</h3>
<select id="xiaomiHeatSource"><option value="0">Датчик 1</option><option value="1">Датчик 2</option><option value="2">Датчик 3</option><option value="3">Датчик 4</option><option value="4">Середня температура доступних</option></select>
<button type="button" class="blue" onclick="saveXiaomiRooms()">💾 Зберегти датчики</button><div id="xiaomiRoomsMsg" class="small"></div>
</div>
</div>

<div class="card sectionAnchor screen section-heatmanual" id="heating-manual">
<div class="screenHeader">
  <button class="backBtn" onclick="showSection('boiler','boiler-control')">‹</button>
  <div>
    <div class="screenHeaderTitle">🔥 Ручний ON</div>
    <div class="screenHeaderSub">ручна температура • крок 0,5 °C</div>
  </div>
</div>
)rawliteral";

  html += "<div class='status'>Температура кімнати: ";

  float manualEffectiveTemp = getEffectiveRoomTemperature();

  if (manualEffectiveTemp > -50.0) {
    html += String(manualEffectiveTemp, 1) + " °C";
    html += " <span style='font-size:13px;color:var(--muted)'>(";
    html += temperatureSourceText();
    html += ")</span>";
  } else {
    html += "❗ Немає датчика температури";
  }

  html += "</div>";

  html += "<div class='thermostatBox'>";
  html += "<div class='thermostatLabel'>Задана температура</div>";
  html += "<div class='thermostatValue' id='heatSetpointValue'>";
  html += String(boilerCurrentSetpoint, 1);
  html += "<small> °C</small></div>";

  html += "<div class='tempControls'>";
  html += "<a class='tempBtn tempMinus' href='#' onclick='heatTempAjax(-5);return false;'>−</a>";
  html += "<a class='tempBtn tempPlus' href='#' onclick='heatTempAjax(5);return false;'>+</a>";
  html += "</div>";

  if (!vacationMode && heatingSetpointOverride) {
    html += "<div class='overrideNote'>Ручна уставка активна • крок 0,5 °C</div>";
    html += "<a class='scheduleBtn' href='#' onclick='heatTempResetAjax();return false;'>🕒 За розкладом</a>";
  } else if (vacationMode) {
    html += "<div class='overrideNote'>Температура режиму Відпустка • крок 0,5 °C</div>";
  } else {
    html += "<div class='overrideNote'>Натисніть + або − для ручної уставки</div>";
  }

  html += "</div>";

  html += "<div class='status'>Опалення: ";
  html += boilerState ? "🔥 ПРАЦЮЄ" : "⚫ ВИМКНЕНО";
  html += "</div>";

  html += "<a href='/boiler?mode=0'><button class='stop'>OFF</button></a>";
  html += "<a href='/boiler?mode=2'><button class='blue'>🌡 AUTO</button></a>";
  html += "</div>";

  // ===================================================
  // ФОРМА НАЛАШТУВАНЬ
  // ===================================================

  html += R"rawliteral(
<form action="/settings">

<div class="card sectionAnchor screen section-water" id="water-schedule">
<h2>💧 Розклад поливу</h2>
)rawliteral";

  for (int i = 0; i < 4; i++) {
    String p = "w" + String(i);

    html += "<div class='zone waterZone'>";
    html += "<h3>Зона " + String(i + 1) + "</h3>";

    html += "<label class='waterToggle'><input type='checkbox' name='" + p + "en' ";
    if (zones[i].enabled) html += "checked ";
    html += "> Автоматично</label>";

    html += "<div class='waterBlockTitle'>Час старту</div>";
    html += "<div class='waterGrid2'>";
    html += "<div class='waterField'><label>Години (0–23)</label><input type='number' min='0' max='23' name='" + p + "h' value='" + String(zones[i].hour) + "'></div>";
    html += "<div class='waterField'><label>Хвилини (0–59)</label><input type='number' min='0' max='59' name='" + p + "m' value='" + String(zones[i].minute) + "'></div>";
    html += "</div>";

    html += "<div class='waterDuration'><span>Тривалість поливу</span><input type='number' min='1' max='240' name='" + p + "dur' value='" + String(zones[i].duration) + "'><span>хв</span></div>";

    html += "<label class='waterIntervalToggle'><input type='checkbox' name='" + p + "intEn' ";
    if (zones[i].intervalEnabled) html += "checked ";
    html += "><span>Використовувати мінімальний інтервал між автоматичними поливами</span></label>";
    html += "<div class='waterGrid2'>";
    html += "<div class='waterField'><label>Інтервал — години</label><input type='number' min='0' max='720' name='" + p + "intH' value='" + String(zones[i].intervalHours) + "'></div>";
    html += "<div class='waterField'><label>Інтервал — хвилини</label><input type='number' min='0' max='59' name='" + p + "intM' value='" + String(zones[i].intervalMinutes) + "'></div>";
    html += "</div>";
    html += "<div class='waterNote'>Якщо прапорець вимкнений — інтервал не використовується. Ручний полив завжди доступний.</div>";

    html += "<div class='days'>";
    html += dayCheckbox(p, 1, "Пн", zones[i].days);
    html += dayCheckbox(p, 2, "Вт", zones[i].days);
    html += dayCheckbox(p, 3, "Ср", zones[i].days);
    html += dayCheckbox(p, 4, "Чт", zones[i].days);
    html += dayCheckbox(p, 5, "Пт", zones[i].days);
    html += dayCheckbox(p, 6, "Сб", zones[i].days);
    html += dayCheckbox(p, 0, "Нд", zones[i].days);
    html += "</div></div>";
  }

  html += R"rawliteral(
<button type="button" class="blue" onclick="saveWaterSettings()">💾 Зберегти налаштування поливу</button>
<div id="waterSaveMsg" class="small"></div>
</div>

<div class="card sectionAnchor screen section-water" id="rain">
<h2>🌧️ Датчик дощу</h2>

<label>
<input type="checkbox" id="rainEn" name="rainEn"
)rawliteral";

  if (rainProtectionEnabled) html += " checked ";

  html += R"rawliteral(
>
Блокувати полив під час дощу
</label>

<br><br>
Пауза після дощу:
<input type="number" id="rainDelay" name="rainDelay" min="0" max="168" value="
)rawliteral";

  html += String(rainDelayHours);

  html += R"rawliteral(
"> годин

<p class="small">
Датчик дощу впливає тільки на полив.
Освітлення та котел працюють незалежно.
</p>
<button type="button" onclick="saveRainSettings()">💾 Зберегти налаштування датчика дощу</button>
<div id="rainSaveMsg" class="small"></div>

</div>

<div class="card sectionAnchor screen section-lights" id="lights-schedule">
<h2>💡 Розклад освітлення</h2>
)rawliteral";

  for (int i = 0; i < LIGHT_COUNT; i++) {
    String p = "l" + String(i);

    html += "<div class='zone'>";
    html += "<h3>Світло " + String(i + 1) + "</h3>";

    html += "<label><input type='checkbox' name='" + p + "en' ";
    if (lights[i].enabled) html += "checked ";
    html += "> Автоматично</label>";

    html += "<br><br><b>Увімкнення</b><br>Години: ";
    html += "<input type='number' min='0' max='23' name='" + p + "onh' value='" + String(lights[i].onHour) + "'> ";
    html += "Хвилини: <input type='number' min='0' max='59' name='" + p + "onm' value='" + String(lights[i].onMinute) + "'>";

    html += "<br><br><b>Вимкнення</b><br>Години: ";
    html += "<input type='number' min='0' max='23' name='" + p + "offh' value='" + String(lights[i].offHour) + "'> ";
    html += "Хвилини: <input type='number' min='0' max='59' name='" + p + "offm' value='" + String(lights[i].offMinute) + "'>";

    html += "<div class='days'>";
    html += dayCheckbox(p, 1, "Пн", lights[i].days);
    html += dayCheckbox(p, 2, "Вт", lights[i].days);
    html += dayCheckbox(p, 3, "Ср", lights[i].days);
    html += dayCheckbox(p, 4, "Чт", lights[i].days);
    html += dayCheckbox(p, 5, "Пт", lights[i].days);
    html += dayCheckbox(p, 6, "Сб", lights[i].days);
    html += dayCheckbox(p, 0, "Нд", lights[i].days);
    html += "</div></div>";
  }

  html += R"rawliteral(
</div>

<div class="card sectionAnchor screen section-boiler" id="boiler-settings">
<h2>🔥 Налаштування котла</h2>

<h3>☀️ День</h3>

Температура:
<input type="number" step="0.1" min="5" max="30" name="dayTemp" value="
)rawliteral";

  html += String(boilerDayTemp, 1);

  html += R"rawliteral(
"> °C

<br><br>
Початок — години:
<input type="number" min="0" max="23" name="dayH" value="
)rawliteral";

  html += String(boilerDayHour);

  html += R"rawliteral(
"> &nbsp; хвилини:
<input type="number" min="0" max="59" name="dayM" value="
)rawliteral";

  html += String(boilerDayMinute);

  html += R"rawliteral(
">

<hr>

<h3>🌙 Ніч</h3>

Температура:
<input type="number" step="0.1" min="5" max="30" name="nightTemp" value="
)rawliteral";

  html += String(boilerNightTemp, 1);

  html += R"rawliteral(
"> °C

<br><br>
Початок — години:
<input type="number" min="0" max="23" name="nightH" value="
)rawliteral";

  html += String(boilerNightHour);

  html += R"rawliteral(
"> &nbsp; хвилини:
<input type="number" min="0" max="59" name="nightM" value="
)rawliteral";

  html += String(boilerNightMinute);

  html += R"rawliteral(
">

<hr>

Гістерезис:
<input type="number" step="0.1" min="0.2" max="5" name="hist" value="
)rawliteral";

  html += String(boilerHysteresis, 1);

  html += R"rawliteral(
"> °C

<hr>

<h3>🏖️ Відпустка</h3>

Температура:
<input type="number" step="0.1" min="5" max="25" name="vacTemp" value="
)rawliteral";

  html += String(vacationTemperature, 1);

  html += R"rawliteral(
"> °C

<br><br>

<label>
<input type="checkbox" name="vacEndEnabled"
)rawliteral";

  if (vacationEndEnabled) html += " checked ";

  html += R"rawliteral(
>
Автоматично завершити режим
</label>

<br><br>
Дата і час повернення:
<br><br>

<input type="datetime-local" name="vacEndDate"
)rawliteral";

  if (vacationEndEpoch > 0) {
    time_t raw = (time_t)vacationEndEpoch;
    struct tm vt;

    localtime_r(&raw, &vt);

    char vb[30];
    strftime(vb, sizeof(vb), "%Y-%m-%dT%H:%M", &vt);

    html += " value='";
    html += vb;
    html += "'";
  }

  html += R"rawliteral(
>

<br><br>

<button type="submit">💾 Зберегти всі налаштування</button>

</div>

</form>

<div class="card sectionAnchor screen section-memo" id="memo"><div class="screenHeader"><button class="backBtn" onclick="showSection('home')">‹</button><div><div class="screenHeaderTitle">📝 Памʼятка</div><div class="screenHeaderSub">особисті нотатки</div></div></div>
<h2>📝 Памʼятка</h2><p class="small">Телефони майстрів, номери рахунків та інші нотатки. Не зберігайте тут паролі, PIN або CVV.</p>
<textarea id="memoText" maxlength="3000" style="width:100%;min-height:260px;box-sizing:border-box;background:var(--surface2);color:var(--text);font-size:16px;padding:12px;border:1px solid var(--line);border-radius:12px;resize:vertical">)rawliteral";
  html += htmlEscape(personalMemo);
  html += R"rawliteral(</textarea><button type="button" onclick="saveMemo()">💾 Зберегти</button><div id="memoSaveMsg" class="small"></div></div>
<div class="card sectionAnchor screen section-authors" id="authors"><div class="screenHeader"><button class="backBtn" onclick="showSection('home')">‹</button><div><div class="screenHeaderTitle">👨‍💻 Автори</div><div class="screenHeaderSub">проєкт Староміська 78а</div></div></div><h2>👨‍💻 Автори</h2><div class="status">Автор ідеї — <b>Остап Федорак</b></div><div class="status">IT виконавець — <b>Ші Остапович</b></div></div>

<div class="card sectionAnchor screen section-journal" id="journal"><div class="screenHeader"><button class="backBtn" onclick="showSection('home')">‹</button><div><div class="screenHeaderTitle">📊 Журнал</div><div class="screenHeaderSub">події та статистика</div></div></div>
<h2>📋 Журнал</h2>

<div class="status">
microSD:
)rawliteral";

  if (!SD_ENABLED) {
    html += "⚪ вимкнена";
  } else {
    html += sdReady ? "🟢 готова" : "🔴 недоступна";
  }

  html += R"rawliteral(
</div>

<p class="small">
microSD і журнал тимчасово вимкнені у прошивці.
</p>
</div>

<div class="card sectionAnchor screen section-settings" id="relay-test"><div class="screenHeader"><button class="backBtn" onclick="showSection('home')">‹</button><div><div class="screenHeaderTitle">⚙️ Налаштування</div><div class="screenHeaderSub">Wi‑Fi • MQTT • реле • GPIO</div></div></div>
<h2>🧪 Тест релейного блока</h2>
<p class="small">
Ручна перевірка 7 каналів реле (котел виключено). У тестовому режимі автоматика поливу та освітлення призупиняється.
</p>
<a href="/relayTest"><button class="blue">🧪 Відкрити тест 7 реле</button></a>
</div>

<div class="card sectionAnchor screen section-settings" id="extra-temp">
<h2>🌡️ Додатковий датчик температури</h2>
<p class="small">
DS18B20 на GPIO 14 використовується лише як резервний датчик.
Основним кімнатним датчиком залишається Xiaomi BLE/BTHome.
</p>
<label class="switchRow"><span><b>DS18B20</b><br><span class="small">Опитувати додатковий датчик</span></span><input id="ds18Enable" type="checkbox" )rawliteral";
  if (extraTempSensorEnabled) html += "checked";
  html += R"rawliteral(></label>
<button type="button" class="blue" onclick="saveExtraTempSensor()">💾 Зберегти</button>
<div id="ds18SaveMsg" class="small" style="margin-top:8px"></div>
</div>

<div class="card sectionAnchor screen section-settings" id="gpio">
<h2>🔌 ESP32-S3 N16R8 — GPIO</h2>

<p class="small">
Полив: GPIO 4, 5, 6, 7<br>
Датчик дощу: GPIO 8<br>
Освітлення: GPIO 9, 10, 11<br>
Котел: GPIO 12<br>
DS18B20: GPIO 14<br>
microSD: SCK 39 / MISO 40 / MOSI 41 / CS 42
</p>

<p class="small">
GPIO 19/20 залишені вільними для native USB.
GPIO 0, 3, 45, 46 не використовуються.
</p>
</div>

<div class="card sectionAnchor screen section-settings" id="wifi-settings">
<h2>⚙️ Налаштування Wi-Fi</h2>

<div class="status">Мережа: )rawliteral";

  if (wifiAPMode) {
    html += WIFI_AP_SSID;
    html += " (SETUP AP)";
  } else {
    html += wifiSSID;
  }

  html += R"rawliteral(</div>

<a href="/wifi">
<button class="blue">⚙️ Змінити Wi-Fi</button>
</a>
</div>

<div class="card sectionAnchor screen section-settings" id="integration">
<h2>🌐 Доступ та інтеграція</h2>

<div class="status">Локальний IP: )rawliteral";

  html += WiFi.localIP().toString();

  html += R"rawliteral(</div>

<div class="status">MQTT: )rawliteral";

  if (!mqttEnabled()) {
    html += "⚪ не налаштований";
  } else if (mqttClient.connected()) {
    html += "🟢 підключений";
  } else {
    html += "🔴 відключений";
  }

  html += R"rawliteral(</div>

<p class="small">
Для віддаленого доступу використовуйте Home Assistant + MQTT.
Через Home Assistant Cloud система доступна з iPhone, Android і Windows
без відкривання порту ESP32 в інтернет.
</p>

<p class="small">
REST API для власного застосунку: <b>/api/status</b><br>
Бездротова температура: <b>/api/temperature?value=21.5</b><br><br>
ESP32-S3 має Bluetooth 5.0. У цій стабільній версії бездротова
температура приймається через Xiaomi BLE/BTHome або MQTT/Home Assistant.
DS18B20 можна окремо увімкнути як резервний датчик у меню Налаштування.
</p>
</div>

<div class="card screen section-settings">
<p class="warning">
УВАГА: котел підключати тільки через гальванічно ізольоване реле
до входу кімнатного термостата / сухого контакту, якщо це дозволено виробником котла.
</p>
</div>

</div>

<script>
function toggleMenu(){document.getElementById('drawer').classList.toggle('open');document.getElementById('overlay').classList.toggle('show')}
function closeMenu(){document.getElementById('drawer').classList.remove('open');document.getElementById('overlay').classList.remove('show')}
function showSection(name,anchor){sessionStorage.setItem('gardenScreen',name);if(anchor)sessionStorage.setItem('gardenAnchor',anchor);else sessionStorage.removeItem('gardenAnchor');document.querySelectorAll('.screen').forEach(e=>e.classList.remove('active'));document.querySelectorAll('.section-'+name).forEach(e=>e.classList.add('active'));closeMenu();window.scrollTo({top:0,behavior:'smooth'});if(anchor)setTimeout(()=>{const e=document.getElementById(anchor);if(e)e.scrollIntoView({behavior:'smooth',block:'start'})},120)}
function applyTheme(t){document.documentElement.setAttribute('data-theme',t);const x=document.getElementById('themeToggle');if(x)x.checked=(t==='dark')}
function toggleTheme(d){const t=d?'dark':'light';localStorage.setItem('gardenTheme',t);applyTheme(t)}
(function(){const s=localStorage.getItem('gardenTheme');applyTheme((s==='dark'||s==='light')?s:((window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)').matches)?'dark':'light'))})();
function updateClock(){const d=new Date(),tz='Europe/Kyiv';const hm=d.toLocaleTimeString('uk-UA',{timeZone:tz,hour:'2-digit',minute:'2-digit'}),hms=d.toLocaleTimeString('uk-UA',{timeZone:tz});const a=document.getElementById('topClock');if(a)a.textContent=hm;const b=document.getElementById('clockMini');if(b)b.textContent=hms;const c=document.getElementById('heroClock');if(c)c.textContent=hm;const dt=document.getElementById('heroDate');if(dt)dt.textContent=d.toLocaleDateString('uk-UA',{timeZone:tz,day:'2-digit',month:'2-digit',year:'numeric'});updateHeroScene(d)}function updateHeroScene(d){const h=document.getElementById('gardenHero');if(!h)return;const tz='Europe/Kyiv';const m=Number(new Intl.DateTimeFormat('en-GB',{timeZone:tz,month:'numeric'}).format(d));const hr=Number(new Intl.DateTimeFormat('en-GB',{timeZone:tz,hour:'2-digit',hour12:false}).format(d));const s=(m==12||m<=2)?'winter':(m<=5?'spring':(m<=8?'summer':'autumn'));const p=(hr>=5&&hr<8)?'dawn':(hr>=8&&hr<18?'day':(hr>=18&&hr<21?'evening':'night'));h.className='hero season-'+s+' time-'+p}setInterval(updateClock,1000);updateClock();const ip=document.getElementById('ipMini');if(ip)ip.textContent=location.hostname||'локальна';
const qs=new URLSearchParams(location.search);const sc=qs.get('screen')||sessionStorage.getItem('gardenScreen');const sa=sessionStorage.getItem('gardenAnchor');if(sc==='water')setTimeout(()=>showSection('water',sa||'water-manual'),0);else if(sc==='lights')setTimeout(()=>showSection('lights',sa||'lights-manual'),0);else if(sc==='boiler')setTimeout(()=>showSection('boiler',sa||'boiler-control'),0);else if(sc==='heatmanual')setTimeout(()=>showSection('heatmanual',sa||'heating-manual'),0);else if(sc==='journal')setTimeout(()=>showSection('journal',sa||'journal'),0);else if(sc==='settings')setTimeout(()=>showSection('settings',sa||'relay-test'),0);else if(sc==='memo')setTimeout(()=>showSection('memo','memo'),0);else if(sc==='authors')setTimeout(()=>showSection('authors','authors'),0);
document.querySelectorAll('.navGroup').forEach(c=>c.addEventListener('toggle',()=>{if(c.open)document.querySelectorAll('.navGroup').forEach(o=>{if(o!==c)o.open=false})}));
async function api(url){try{const r=await fetch(url,{cache:'no-store'});return r.ok}catch(e){return false}}
function paintWater(all,zone){for(let i=1;i<=4;i++){const b=document.getElementById('waterBtn'+i);if(b)b.classList.toggle('waterActive',!!all||zone===i)}}
let gardenStateBusy=false;let lastXiaomiUiRender=0;async function refreshGardenState(){if(gardenStateBusy)return;gardenStateBusy=true;try{const r=await fetch('/api/status?_='+Date.now(),{cache:'no-store'});if(!r.ok)return;const d=await r.json();if(Date.now()-lastXiaomiUiRender>10000){renderXiaomiSensors(d);lastXiaomiUiRender=Date.now();}paintWater(d.watering_all,d.watering_zone||0);const t=document.getElementById('inTemp');if(t)t.textContent=(d.temperature==null?'--.-':Number(d.temperature).toFixed(1))+' °C';const sp=document.getElementById('heatSetpointValue');if(sp&&d.boiler_setpoint!=null&&!heatBusy){heatShown=Number(d.boiler_setpoint);sp.innerHTML=heatShown.toFixed(1)+"<small> °C</small>";}}catch(e){}finally{gardenStateBusy=false;}}
async function waterZone(z){paintWater(false,z);await api('/api/water?action=start&zone='+z);refreshGardenState()}
async function waterAll(){paintWater(true,0);await api('/api/water?action=all');refreshGardenState()}
async function waterAllTogether(){paintWater(true,0);await api('/api/water?action=alltogether');refreshGardenState()}
async function waterStop(){paintWater(false,0);await api('/api/water?action=stop');refreshGardenState()}
async function setBoilerMode(mode){await api('/api/boiler?mode='+encodeURIComponent(mode))}
let heatBusy=false,heatQueue=[],heatSending=false,heatShown=null;
function heatRender(n){const v=document.getElementById('heatSetpointValue');if(v&&Number.isFinite(n))v.innerHTML=n.toFixed(1)+"<small> °C</small>";}
async function heatFlush(){if(heatSending)return;heatSending=true;heatBusy=true;while(heatQueue.length){const delta=heatQueue.shift();try{const r=await fetch('/heatTemp?delta='+delta+'&ajax=1',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const d=await r.json();if(!heatQueue.length&&d.boiler_setpoint!=null){heatShown=Number(d.boiler_setpoint);heatRender(heatShown);}}catch(e){console.warn('Setpoint sync failed',e);heatQueue=[];try{const r=await fetch('/api/status',{cache:'no-store'});if(r.ok){const d=await r.json();heatShown=Number(d.boiler_setpoint);heatRender(heatShown);}}catch(x){}break;}}heatBusy=false;heatSending=false;refreshGardenState();}
function heatTempAjax(delta){const v=document.getElementById('heatSetpointValue');if(heatShown==null){const n=parseFloat(v?.textContent||'');heatShown=Number.isFinite(n)?n:21.5;}heatShown=Math.max(5,Math.min(30,Math.round((heatShown+delta/10)*10)/10));heatRender(heatShown);heatQueue.push(delta);heatFlush();}
async function heatTempResetAjax(){if(heatSending)return;heatBusy=true;try{const r=await fetch('/heatTemp?reset=1&ajax=1',{cache:'no-store'});if(r.ok){const d=await r.json();heatShown=Number(d.boiler_setpoint);heatRender(heatShown);}}catch(e){}finally{heatBusy=false;}}
async function setLightAjax(ch,state){if(await api('/api/light?channel='+ch+'&state='+(state?'on':'off'))){const b=document.getElementById('lightBtn'+ch);if(b){b.textContent=state?'Вимкнути':'Увімкнути';b.className=state?'off':'lightOn';b.setAttribute('onclick','setLightAjax('+ch+','+(state?0:1)+')')}}}
function escHtml(v){return String(v??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
const renderXiaomiSensors=(d)=>{if(!d||!Array.isArray(d.xiaomi_sensors))return;d.xiaomi_sensors.forEach((x,i)=>{const el=document.getElementById('xs'+i);if(!el)return;const t=x.online&&x.temperature!=null?Number(x.temperature).toFixed(1)+' °C':'—';const hum=x.humidity!=null?Number(x.humidity).toFixed(0)+'%':'—';const bat=x.battery!=null?x.battery+'%':'—';el.innerHTML='<label><input type="checkbox" id="xe'+i+'" '+(x.enabled?'checked':'')+'> Датчик '+(i+1)+'</label><input id="xn'+i+'" value="'+escHtml(x.name)+'" placeholder="Назва кімнати"><input id="xm'+i+'" value="'+escHtml(x.mac)+'" placeholder="AA:BB:CC:DD:EE:FF"><div class="sensorTemp">'+t+'</div><div class="sensorMeta">Вологість '+hum+' • Батарея '+bat+' • RSSI '+(x.rssi??'—')+'</div>';});const sel=document.getElementById('xiaomiHeatSource');if(sel)sel.value=String(d.xiaomi_heating_source??0);};
async function saveXiaomiRooms(){const p=new URLSearchParams();for(let i=0;i<4;i++){p.set('en'+i,document.getElementById('xe'+i)?.checked?'1':'0');p.set('name'+i,document.getElementById('xn'+i)?.value||'');p.set('mac'+i,document.getElementById('xm'+i)?.value||'');}p.set('source',document.getElementById('xiaomiHeatSource')?.value||'0');const m=document.getElementById('xiaomiRoomsMsg');try{const r=await fetch('/xiaomiSensors',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString(),cache:'no-store'});if(m)m.textContent=r.ok?'✓ Збережено. BLE продовжує сканування всіх увімкнених датчиків.':'Помилка збереження';}catch(e){if(m)m.textContent='Помилка збереження';}}
async function saveWaterSettings(){const form=document.querySelector('form[action="/settings"]');const m=document.getElementById('waterSaveMsg');if(!form){if(m)m.textContent='Помилка форми';return;}const p=new URLSearchParams();for(const e of form.elements){if(!e.name||!/^w[0-3](en|h|m|dur|intEn|intH|intM|d[0-6])$/.test(e.name))continue;if((e.type==='checkbox'||e.type==='radio')&&!e.checked)continue;p.append(e.name,e.value||'1');}try{const r=await fetch('/waterSettingsSave',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString(),cache:'no-store'});if(m)m.textContent=r.ok?'✓ Налаштування поливу збережено':'Помилка збереження';if(r.ok){sessionStorage.setItem('gardenScreen','water');sessionStorage.setItem('gardenAnchor','water-schedule');}}catch(e){if(m)m.textContent='Помилка збереження';}}
async function saveRainSettings(){const en=document.getElementById('rainEn').checked?1:0;const h=document.getElementById('rainDelay').value;const ok=await api('/rainSave?enabled='+en+'&hours='+encodeURIComponent(h));const m=document.getElementById('rainSaveMsg');if(m)m.textContent=ok?'✓ Збережено':'Помилка збереження'}
async function saveExtraTempSensor(){const e=document.getElementById('ds18Enable');const en=(e&&e.checked)?1:0;try{const r=await fetch('/extraTemp?enabled='+en,{cache:'no-store'});const m=document.getElementById('ds18SaveMsg');if(m)m.textContent=r.ok?(en?'✓ DS18B20 увімкнено':'✓ DS18B20 вимкнено'):'Помилка збереження'}catch(x){const m=document.getElementById('ds18SaveMsg');if(m)m.textContent='Помилка збереження'}}
async function saveMemo(){const t=document.getElementById('memoText').value;try{const r=await fetch('/memoSave',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'text='+encodeURIComponent(t)});const m=document.getElementById('memoSaveMsg');if(m)m.textContent=r.ok?'✓ Збережено':'Помилка збереження'}catch(e){const m=document.getElementById('memoSaveMsg');if(m)m.textContent='Помилка збереження'}}
async function refreshOutdoorTemp(){const e=document.getElementById('outTemp');try{const r=await fetch('https://api.open-meteo.com/v1/forecast?latitude=48.53&longitude=25.04&current=temperature_2m&timezone=Europe%2FKyiv',{cache:'no-store'});if(!r.ok)throw 0;const d=await r.json();const v=d&&d.current?d.current.temperature_2m:null;if(e)e.textContent=(v==null?'--.-':Number(v).toFixed(1))+' °C'}catch(x){if(e)e.textContent='--.- °C'}}
setInterval(refreshGardenState,1000);refreshGardenState();refreshOutdoorTemp();setInterval(refreshOutdoorTemp,600000);
if('serviceWorker' in navigator){navigator.serviceWorker.register('/sw.js').catch(()=>{});}
</script>

</body>
</html>
)rawliteral";

  return html;
}

// =====================================================
// WEB HANDLERS
// =====================================================

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");

  server.send(
    200,
    "text/html; charset=utf-8",
    createPage()
  );
}

void handleWater() {
  if (server.hasArg("z")) {
    int zone = server.arg("z").toInt();

    if (zone >= 0 && zone <= 3 && !rainBlocked()) {
      stopWatering();
      waterAllActive = false;

      waterQueue[0] = zone;
      queueCount = 1;
      queuePosition = 0;

      startNextWaterZone();
    }
  }

  server.sendHeader("Location", "/?screen=water");
  server.send(303, "text/plain", "");
}

void handleWaterAll() {
  if (!rainBlocked()) {
    stopWatering();
    waterAllActive = true;

    queueCount = 4;
    queuePosition = 0;

    for (int i = 0; i < 4; i++) {
      waterQueue[i] = i;
    }

    startNextWaterZone();
  }

  server.sendHeader("Location", "/?screen=water");
  server.send(303, "text/plain", "");
}

void handleWaterAllTogether() {
  if (!rainBlocked()) {
    stopWatering();

    waterAllActive = true;
    waterSimultaneousActive = true;
    waterSimStartMillis = millis();
    currentZone = -1;
    queueCount = 0;
    queuePosition = 0;

    for (int i = 0; i < 4; i++) {
      digitalWrite(WATER_PINS[i], RELAY_ON);
      waterSimZoneActive[i] = true;
      rememberWaterStart(i);
    }

    watering = true;

    Serial.println("WATER ALL ON");
    logEvent("WATER", "ALL_SIMULTANEOUS_START", "Зони 1-4 увімкнені одночасно");
    publishMQTTState();
  }

  server.sendHeader("Location", "/?screen=water");
  server.send(303, "text/plain", "");
}

void handleWaterStop() {
  stopWatering();
  server.sendHeader("Location", "/?screen=water");
  server.send(303, "text/plain", "");
}

void handleLight() {
  if (server.hasArg("ch") && server.hasArg("state")) {
    int ch = server.arg("ch").toInt();
    int state = server.arg("state").toInt();

    if (ch >= 0 && ch < LIGHT_COUNT) {
      // Датчик дощу тут НЕ перевіряється
      setLight(ch, state == 1);
      logEvent("LIGHT", state == 1 ? "MANUAL_ON" : "MANUAL_OFF", "Канал " + String(ch + 1));
    }
  }

  server.sendHeader("Location", "/?screen=lights");
  server.send(303, "text/plain", "");
}

void handleBoiler() {
  if (server.hasArg("mode")) {
    int mode = server.arg("mode").toInt();

    if (mode >= 0 && mode <= 2) {
      boilerMode = (BoilerMode)mode;

      if (boilerMode == BOILER_OFF) {
        setBoiler(false);
      }

      if (boilerMode == BOILER_AUTO) {
        updateBoilerSetpointBySchedule();
      }
    }
  }

  server.sendHeader("Location", "/?screen=boiler");
  server.send(303, "text/plain", "");
}

void saveHeatingSetpointOnly() {
  prefs.begin("garden", false);
  prefs.putBool("heatOv", heatingSetpointOverride);
  prefs.putFloat("heatTemp", heatingSetpoint);
  prefs.end();
}

void handleHeatingTemperature() {
  bool ajaxRequest = server.hasArg("ajax") && server.arg("ajax") == "1";
  if (server.hasArg("reset") && server.arg("reset") == "1") {
    heatingSetpointOverride = false;
    updateBoilerSetpointBySchedule();
    saveHeatingSetpointOnly();

    logEvent(
      "HEATING",
      "SETPOINT_SCHEDULE",
      "Повернення до розкладу"
    );

    if (ajaxRequest) {
      sendJson(200, buildStatusJson());
    } else {
      server.sendHeader("Location", "/?screen=heatmanual");
      server.send(303);
    }
    return;
  }

  if (!server.hasArg("delta")) {
    server.sendHeader("Location", "/?screen=heatmanual");
    server.send(303);
    return;
  }

  // +5/-5 означає +/-0.5 °C
  int deltaTenths = server.arg("delta").toInt();

  if (deltaTenths != 5 && deltaTenths != -5) {
    server.sendHeader("Location", "/?screen=heatmanual");
    server.send(303);
    return;
  }

  float delta = deltaTenths / 10.0f;

  if (vacationMode) {
    vacationTemperature += delta;

    if (vacationTemperature < 5.0) vacationTemperature = 5.0;
    if (vacationTemperature > 30.0) vacationTemperature = 30.0;

    boilerMode = BOILER_AUTO;
    boilerCurrentSetpoint = vacationTemperature;
    queueXiaomiSetpointDisplay(boilerCurrentSetpoint);

    logEvent(
      "HEATING",
      "VACATION_SETPOINT",
      String(vacationTemperature, 1) + " C"
    );
  } else {
    if (!heatingSetpointOverride) {
      updateBoilerSetpointBySchedule();
      heatingSetpoint = boilerCurrentSetpoint;
    }

    heatingSetpoint += delta;

    if (heatingSetpoint < 5.0) heatingSetpoint = 5.0;
    if (heatingSetpoint > 30.0) heatingSetpoint = 30.0;

    heatingSetpointOverride = true;
    boilerMode = BOILER_AUTO;
    boilerCurrentSetpoint = heatingSetpoint;
    queueXiaomiSetpointDisplay(boilerCurrentSetpoint);

    logEvent(
      "HEATING",
      "SETPOINT_CHANGED",
      String(heatingSetpoint, 1) + " C"
    );
  }

  saveHeatingSetpointOnly();
  if (ajaxRequest) {
    publishMQTTState();
    sendJson(200, buildStatusJson());
  } else {
    server.sendHeader("Location", "/?screen=heatmanual");
    server.send(303);
  }
}

void handleVacation() {
  if (server.hasArg("state")) {
    int state = server.arg("state").toInt();

    vacationMode = (state == 1);

    // При ввімкненні відпустки автоматично переводимо котел у AUTO
    if (vacationMode) {
      boilerMode = BOILER_AUTO;
    }

    saveSettings();
    updateBoilerSetpointBySchedule();

    logEvent(
      "BOILER",
      vacationMode ? "VACATION_ON" : "VACATION_OFF",
      vacationMode
        ? ("Температура " + String(vacationTemperature, 1) + " C")
        : "Повернення до звичайного режиму"
    );
  }

  server.sendHeader("Location", "/?screen=boiler");
  server.send(303, "text/plain", "");
}


void handleLog() {
  String logText = readLogFile();

  String page;

  page.reserve(logText.length() + 3000);

  page += R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Журнал ESP32</title>
<style>
body{
  font-family:-apple-system,BlinkMacSystemFont,Arial,sans-serif;
  background:#eef4ee;
  padding:12px;
}
.card{
  max-width:900px;
  margin:auto;
  background:var(--card);
  padding:18px;
  border-radius:18px;
  box-shadow:0 3px 12px rgba(0,0,0,.14);
}
pre{
  white-space:pre-wrap;
  word-break:break-word;
  background:#111;
  color:#eee;
  padding:12px;
  border-radius:10px;
  overflow:auto;
}
button{
  width:100%;
  padding:14px;
  margin:5px 0;
  font-size:17px;
  border:0;
  border-radius:11px;
}
</style>
</head>
<body>
<div class="card">
<h2>📋 Журнал</h2>
<a href="/"><button>← Назад</button></a>
<pre>
)rawliteral";

  // HTML escape
  logText.replace("&", "&amp;");
  logText.replace("<", "&lt;");
  logText.replace(">", "&gt;");

  page += logText;

  page += R"rawliteral(
</pre>
</div>
</body>
</html>
)rawliteral";

  server.send(
    200,
    "text/html; charset=utf-8",
    page
  );
}

void handleLogClear() {
  bool ok = clearLogFile();

  if (!ok) {
    server.send(
      500,
      "text/plain; charset=utf-8",
      "Не вдалося очистити журнал."
    );
    return;
  }

  redirectHome();
}

void handleLogEmail() {
  bool ok = sendLogByEmail();

  String page;

  page += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>E-mail</title></head><body style='font-family:Arial;padding:20px'>";

  if (ok) {
    page += "<h2>✅ Журнал надіслано</h2>";
    page += "<p>Журнал надіслано.</p>";
  } else {
    page += "<h2>❌ Помилка відправки</h2>";
    page += "<p>E-mail журнал вимкнено у V20 Lite.</p>";
  }

  page += "<p><a href='/'>← Повернутися</a></p>";
  page += "</body></html>";

  server.send(
    ok ? 200 : 500,
    "text/html; charset=utf-8",
    page
  );
}


// =====================================================
// WEB — ТЕСТ 8 РЕЛЕ
// =====================================================
void handleRelayTest() {
  if (!relayTestMode) {
    stopWatering();
    allWaterOff();
    setBoiler(false);
    for (int i = 0; i < LIGHT_COUNT; i++) setLight(i, false);
    allRelayTestOff();
    relayTestMode = true;
    logEvent("RELAY_TEST", "ENTER", "Тестовий режим увімкнено");
  }

  String page;
  page.reserve(8000);

  page += R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Тест реле</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,Arial;background:#eef4ee;margin:0;padding:14px}
.card{max-width:700px;margin:0 auto 15px;background:#fff;padding:18px;border-radius:18px;box-shadow:0 3px 12px rgba(0,0,0,.14)}
h1,h2{text-align:center}.status{font-size:18px;font-weight:700;text-align:center;margin:8px 0}
.row{border-top:1px solid #ddd;padding:14px 0}
button{width:100%;padding:14px;margin:5px 0;font-size:17px;border:0;border-radius:11px;color:white;background:#328e4c}
.off{background:#555}.stop{background:#d13636}.small{text-align:center;color:#666}
</style></head><body>
<div class="card">
<h1>🧪 Тест 7 реле</h1>
<div class="status">⚠️ Автоматика поливу та освітлення призупинена</div>
<p class="small">Relay 1–4 = полив, Relay 5–7 = освітлення. Relay 8 (GPIO12) = котел, тест заборонений. GPIO12 зарезервовано для котла. Тестуйте без підключених силових навантажень.</p>
</div>
<div class="card">
)rawliteral";

  for (int i = 0; i < 7; i++) {
    page += "<div class='row'><h2>Relay ";
    page += String(i + 1);
    if (i < 4) {
      page += " — Полив ";
      page += String(i + 1);
    } else {
      page += " — Світло ";
      page += String(i - 3);
    }
    page += "</h2><div class='status'>";
    page += relayTestState[i] ? "🟢 УВІМКНЕНО" : "⚫ ВИМКНЕНО";
    page += "</div>";

    if (relayTestState[i]) {
      page += "<a href='/relayTestSet?relay=" + String(i + 1) + "&state=0'><button class='off'>Вимкнути</button></a>";
    } else {
      page += "<a href='/relayTestSet?relay=" + String(i + 1) + "&state=1'><button>Увімкнути</button></a>";
    }
    page += "</div>";
  }

  page += R"rawliteral(
</div>
<div class="card">
<a href="/relayTestExit"><button class="stop">■ Завершити тест і вимкнути всі реле</button></a>
</div>
</body></html>
)rawliteral";

  server.send(200, "text/html; charset=utf-8", page);
}

void handleRelayTestSet() {
  if (!relayTestMode) {
    server.sendHeader("Location", "/relayTest");
    server.send(303, "text/plain", "");
    return;
  }

  if (server.hasArg("relay") && server.hasArg("state")) {
    int relayNumber = server.arg("relay").toInt();
    int state = server.arg("state").toInt();

    if (relayNumber >= 1 && relayNumber <= 7) {
      setRelayTestChannel(relayNumber - 1, state == 1);
    }
  }

  server.sendHeader("Location", "/relayTest");
  server.send(303, "text/plain", "");
}

void handleRelayTestExit() {
  allRelayTestOff();
  setBoiler(false);
  relayTestMode = false;
  stopWatering();

  for (int i = 0; i < LIGHT_COUNT; i++) setLight(i, false);

  logEvent("RELAY_TEST", "EXIT", "Тест завершено, 7 тестових реле OFF, котел OFF");
  redirectHome();
}

// =====================================================
// ЗБЕРЕЖЕННЯ НАЛАШТУВАНЬ З WEB
// =====================================================

void handleWaterSettingsSave() {
  for (int i = 0; i < 4; i++) {
    String p = "w" + String(i);

    zones[i].enabled = server.hasArg(p + "en");

    if (server.hasArg(p + "h")) {
      zones[i].hour = constrain(server.arg(p + "h").toInt(), 0, 23);
    }
    if (server.hasArg(p + "m")) {
      zones[i].minute = constrain(server.arg(p + "m").toInt(), 0, 59);
    }
    if (server.hasArg(p + "dur")) {
      zones[i].duration = constrain(server.arg(p + "dur").toInt(), 1, 240);
    }

    zones[i].intervalEnabled = server.hasArg(p + "intEn");

    if (server.hasArg(p + "intH")) {
      zones[i].intervalHours = constrain(server.arg(p + "intH").toInt(), 0, 720);
    }
    if (server.hasArg(p + "intM")) {
      zones[i].intervalMinutes = constrain(server.arg(p + "intM").toInt(), 0, 59);
    }

    zones[i].days = 0;
    for (int d = 0; d < 7; d++) {
      if (server.hasArg(p + "d" + String(d))) zones[i].days |= (1 << d);
    }
  }

  saveSettings();
  Serial.println("WATER SETTINGS SAVED");
  logEvent("WATER", "SETTINGS", "Налаштування поливу збережено");
  sendJson(200, "{\"ok\":true}");
}

void handleRainSave() {
  if (server.hasArg("enabled")) rainProtectionEnabled = server.arg("enabled").toInt() == 1;
  if (server.hasArg("hours")) rainDelayHours = constrain(server.arg("hours").toInt(), 0, 168);
  saveSettings();
  logEvent("RAIN", "SETTINGS", "Пауза після дощу: " + String(rainDelayHours) + " год");
  sendJson(200, "{\"ok\":true}");
}

void handleMemoSave() {
  if (!server.hasArg("text")) { sendJson(400, "{\"ok\":false}"); return; }
  personalMemo = server.arg("text");
  if (personalMemo.length() > 3000) personalMemo = personalMemo.substring(0, 3000);
  prefs.begin("garden", false); prefs.putString("memo", personalMemo); prefs.end();
  sendJson(200, "{\"ok\":true}");
}

void handleExtraTempSensor() {
  if (!server.hasArg("enabled")) {
    sendJson(400, "{\"ok\":false,\"error\":\"missing_enabled\"}");
    return;
  }

  extraTempSensorEnabled = server.arg("enabled").toInt() != 0;

  if (!extraTempSensorEnabled) {
    roomTemperature = -127.0;
  } else {
    // Дозволяємо негайне перше опитування після увімкнення.
    lastTempRead = 0;
  }

  prefs.begin("garden", false);
  prefs.putBool("ds18En", extraTempSensorEnabled);
  prefs.end();

  Serial.print("DS18B20 polling: ");
  Serial.println(extraTempSensorEnabled ? "ENABLED" : "DISABLED");

  String j = "{\"ok\":true,\"enabled\":";
  j += extraTempSensorEnabled ? "true" : "false";
  j += "}";
  sendJson(200, j);
}

void handleXiaomiSensorsSave() {
  for(uint8_t i=0;i<XIAOMI_SENSOR_COUNT;i++) {
    String a=String(i);
    xiaomiSensorEnabled[i]=server.hasArg("en"+a) && server.arg("en"+a)=="1";
    if(server.hasArg("name"+a)) { xiaomiRoomNames[i]=server.arg("name"+a); xiaomiRoomNames[i].trim(); if(xiaomiRoomNames[i].length()>32) xiaomiRoomNames[i]=xiaomiRoomNames[i].substring(0,32); }
    if(server.hasArg("mac"+a)) { xiaomiMacs[i]=server.arg("mac"+a); xiaomiMacs[i].trim(); xiaomiMacs[i].toUpperCase(); }
  }
  if(server.hasArg("source")) heatingXiaomiSource=constrain(server.arg("source").toInt(),0,4);
  saveSettings();
  Serial.println("XIAOMI SENSORS SETTINGS SAVED");
  sendJson(200,"{\"ok\":true}");
}

void handleSettings() {

  // -------------------------
  // WATER
  // -------------------------

  for (int i = 0; i < 4; i++) {
    String p = "w" + String(i);

    zones[i].enabled =
      server.hasArg(p + "en");

    if (server.hasArg(p + "h")) {
      zones[i].hour =
        constrain(server.arg(p + "h").toInt(), 0, 23);
    }

    if (server.hasArg(p + "m")) {
      zones[i].minute =
        constrain(server.arg(p + "m").toInt(), 0, 59);
    }

    if (server.hasArg(p + "dur")) {
      zones[i].duration =
        constrain(server.arg(p + "dur").toInt(), 1, 240);
    }

    zones[i].intervalEnabled = server.hasArg(p + "intEn");

    if (server.hasArg(p + "intH")) {
      zones[i].intervalHours =
        constrain(server.arg(p + "intH").toInt(), 0, 720);
    }

    if (server.hasArg(p + "intM")) {
      zones[i].intervalMinutes =
        constrain(server.arg(p + "intM").toInt(), 0, 59);
    }

    zones[i].days = 0;

    for (int d = 0; d < 7; d++) {
      if (server.hasArg(p + "d" + String(d))) {
        zones[i].days |= (1 << d);
      }
    }
  }

  // -------------------------
  // RAIN
  // -------------------------

  rainProtectionEnabled =
    server.hasArg("rainEn");

  if (server.hasArg("rainDelay")) {
    rainDelayHours =
      constrain(server.arg("rainDelay").toInt(), 0, 168);
  }

  // -------------------------
  // LIGHT
  // -------------------------

  for (int i = 0; i < LIGHT_COUNT; i++) {
    String p = "l" + String(i);

    lights[i].enabled =
      server.hasArg(p + "en");

    if (server.hasArg(p + "onh")) {
      lights[i].onHour =
        constrain(server.arg(p + "onh").toInt(), 0, 23);
    }

    if (server.hasArg(p + "onm")) {
      lights[i].onMinute =
        constrain(server.arg(p + "onm").toInt(), 0, 59);
    }

    if (server.hasArg(p + "offh")) {
      lights[i].offHour =
        constrain(server.arg(p + "offh").toInt(), 0, 23);
    }

    if (server.hasArg(p + "offm")) {
      lights[i].offMinute =
        constrain(server.arg(p + "offm").toInt(), 0, 59);
    }

    lights[i].days = 0;

    for (int d = 0; d < 7; d++) {
      if (server.hasArg(p + "d" + String(d))) {
        lights[i].days |= (1 << d);
      }
    }
  }

  // -------------------------
  // BOILER
  // -------------------------

  if (server.hasArg("dayTemp")) {
    boilerDayTemp =
      constrain(server.arg("dayTemp").toFloat(), 5.0f, 30.0f);
  }

  if (server.hasArg("nightTemp")) {
    boilerNightTemp =
      constrain(server.arg("nightTemp").toFloat(), 5.0f, 30.0f);
  }

  if (server.hasArg("hist")) {
    boilerHysteresis =
      0.5f; // fixed in V34.3
  }

  if (server.hasArg("dayH")) {
    boilerDayHour =
      constrain(server.arg("dayH").toInt(), 0, 23);
  }

  if (server.hasArg("dayM")) {
    boilerDayMinute =
      constrain(server.arg("dayM").toInt(), 0, 59);
  }

  if (server.hasArg("nightH")) {
    boilerNightHour =
      constrain(server.arg("nightH").toInt(), 0, 23);
  }

  if (server.hasArg("nightM")) {
    boilerNightMinute =
      constrain(server.arg("nightM").toInt(), 0, 59);
  }

  if (server.hasArg("vacTemp")) {
    vacationTemperature =
      constrain(server.arg("vacTemp").toFloat(), 5.0f, 25.0f);
  }

  // -------------------------
  // VACATION END
  // -------------------------

  vacationEndEnabled =
    server.hasArg("vacEndEnabled");

  if (server.hasArg("vacEndDate")) {
    String value = server.arg("vacEndDate");

    if (value.length() >= 16) {
      struct tm vt = {};

      vt.tm_year =
        value.substring(0, 4).toInt() - 1900;

      vt.tm_mon =
        value.substring(5, 7).toInt() - 1;

      vt.tm_mday =
        value.substring(8, 10).toInt();

      vt.tm_hour =
        value.substring(11, 13).toInt();

      vt.tm_min =
        value.substring(14, 16).toInt();

      vt.tm_sec = 0;
      vt.tm_isdst = -1;

      time_t result = mktime(&vt);

      if (result > 0) {
        vacationEndEpoch = (uint64_t)result;
      }
    }
  }

  if (!vacationEndEnabled) {
    vacationEndEpoch = 0;
  }

  saveSettings();
  updateBoilerSetpointBySchedule();

  redirectHome();
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  Serial.println("=== Garden_U_V35 SECURE + CLOUD ===");
  Serial.printf("Build: %s %s\n", __DATE__, __TIME__);

  Serial.println();

  // -------------------------
  // WATER
  // -------------------------

  for (int i = 0; i < 4; i++) {
    pinMode(WATER_PINS[i], OUTPUT);
    digitalWrite(WATER_PINS[i], RELAY_OFF);
  }

  // -------------------------
  // LIGHT
  // -------------------------

  for (int i = 0; i < LIGHT_COUNT; i++) {
    pinMode(LIGHT_PINS[i], OUTPUT);
    digitalWrite(LIGHT_PINS[i], RELAY_OFF);
  }

  // -------------------------
  // BOILER
  // -------------------------

  pinMode(BOILER_PIN, OUTPUT);
  digitalWrite(BOILER_PIN, RELAY_OFF);
  boilerState = false;

  // -------------------------
  // RAIN
  // -------------------------

  pinMode(RAIN_PIN, INPUT_PULLUP);
  previousRainState = isRaining();

  // -------------------------
  // DS18B20
  // -------------------------

  tempSensors.begin();

  // Опитування DS18B20 виконується лише коли опцію увімкнено в Налаштуваннях.

  // -------------------------
  // XIAOMI BLE / BTHOME
  // -------------------------
  // V28: запускаємо після loadSettings(), щоб застосувати MAC/назви 4 датчиків.

  // -------------------------
  // MICROSD / JOURNAL
  // -------------------------
  // Вимкнено у V20 Lite, щоб звільнити Flash для основних функцій.
  sdReady = false;

  // -------------------------
  // LOAD SETTINGS
  // -------------------------

  loadSettings();
  // V29: одразу відновлюємо фактичну уставку з Preferences/розкладу,
  // щоб після перезапуску кнопки Xiaomi не падали на 5.0 C.
  updateBoilerSetpointBySchedule();
  startXiaomiBle();
  Serial.print("DS18B20 polling: ");
  Serial.println(extraTempSensorEnabled ? "ENABLED" : "DISABLED");

  // -------------------------
  // WIFI
  // -------------------------

  loadWiFiSettings();

  if (!connectWiFi(20000UL)) {
    startWiFiSetupAP();
  }

  // -------------------------
  // REMOTE ACCESS / MQTT
  // -------------------------

  loadRemoteSettings();
  configureMqttTransport();

  Serial.print("Remote access: ");
  Serial.println(remoteAccessEnabled ? "ENABLED" : "DISABLED");

  if (remoteAccessEnabled) {
    Serial.print("MQTT host: ");
    Serial.print(remoteMqttHost);
    Serial.print(":");
    Serial.println(remoteMqttPort);

    Serial.print("MQTT TLS: ");
    Serial.println(remoteTlsEnabled ? "ON" : "OFF");
  }

  // -------------------------
  // ЧАС УКРАЇНИ
  // -------------------------

  configTzTime(
    "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
  );

  // -------------------------
  // WEB
  // -------------------------

  server.on("/", AUTH(handleRoot));

  server.on("/water", AUTH(handleWater));
  server.on("/waterAll", AUTH(handleWaterAll));
  server.on("/waterAllTogether", AUTH(handleWaterAllTogether));
  server.on("/waterStop", AUTH(handleWaterStop));

  server.on("/light", AUTH(handleLight));

  server.on("/boiler", AUTH(handleBoiler));
  server.on("/heatTemp", AUTH(handleHeatingTemperature));
  server.on("/vacation", AUTH(handleVacation));

  server.on("/settings", AUTH(handleSettings));
  server.on("/xiaomiSensors", HTTP_POST, AUTH(handleXiaomiSensorsSave));
  server.on("/waterSettingsSave", HTTP_POST, AUTH(handleWaterSettingsSave));
  server.on("/rainSave", HTTP_ANY, AUTH(handleRainSave));
  server.on("/memoSave", HTTP_POST, AUTH(handleMemoSave));

  server.on("/log", AUTH(handleLog));
  server.on("/logClear", AUTH(handleLogClear));
  server.on("/logEmail", AUTH(handleLogEmail));

  // REST API
  server.on("/api/status", HTTP_GET, AUTH(handleApiStatus));
  server.on("/api/water", HTTP_ANY, AUTH(handleApiWater));
  server.on("/api/light", HTTP_ANY, AUTH(handleApiLight));
  server.on("/api/boiler", HTTP_ANY, AUTH(handleApiBoiler));
  server.on("/api/vacation", HTTP_ANY, AUTH(handleApiVacation));
  server.on("/api/temperature", HTTP_ANY, AUTH(handleApiExternalTemperature));
  server.on("/extraTemp", HTTP_GET, AUTH(handleExtraTempSensor));
  server.on("/api/log/clear", HTTP_ANY, AUTH(handleApiLogClear));
  server.on("/api/log/email", HTTP_ANY, AUTH(handleApiLogEmail));

  // PWA
  server.on("/manifest.json", HTTP_GET, handleManifest);
  server.on("/pwa", HTTP_GET, AUTH(handlePwaInstall));
  server.on("/pwa-icon-180.png", HTTP_GET, handlePwaIcon180);
  server.on("/pwa-icon-512.png", HTTP_GET, handlePwaIcon512);
  server.on("/sw.js", HTTP_GET, handleServiceWorker);
  server.on("/wifi", HTTP_GET, AUTH(handleWiFiSettings));
  server.on("/wifiSave", HTTP_POST, AUTH(handleWiFiSave));

  server.on("/remote", HTTP_GET, AUTH(handleRemoteSettings));
  server.on("/remoteSave", HTTP_POST, AUTH(handleRemoteSave));
  server.on("/relayTest", HTTP_GET, AUTH(handleRelayTest));
  server.on("/relayTestSet", HTTP_GET, AUTH(handleRelayTestSet));
  server.on("/relayTestExit", HTTP_GET, AUTH(handleRelayTestExit));

  server.begin();

  if (webPassword.length() >= 6) {
    ArduinoOTA.setHostname(DEVICE_NAME);
    ArduinoOTA.setPassword(webPassword.c_str());
    ArduinoOTA.onStart([]() { allWaterOff(); setBoiler(false); });
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.println("OTA: ON");
  }

  Serial.println("WEB SERVER STARTED");
  Serial.print("Web auth: ");
  Serial.println(webPassword.length() ? "ON" : "OFF (задайте пароль у /remote)");
  logEvent("SYSTEM", "BOOT", "IP " + WiFi.localIP().toString());
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  server.handleClient();
  if (otaStarted) ArduinoOTA.handle();

  ensureWiFi();
  ensureMQTT();
  processLightRestore();
  processWeather();
  processPushQueue();

  processRainSensor();
  processWatering();

  checkWaterSchedule();
  checkLightSchedule();

  readTemperature();

  if (xiaomiBleScan && !xiaomiBleScan->isScanning() && !xiaomiSetpointDisplayPending) {
    xiaomiBleScan->start(0,false,true);
  }

  processXiaomiThermostatButtons();
  processXiaomiSetpointDisplay();
  processXiaomiTimeSync();

  processVacationEnd();
  updateBoilerSetpointBySchedule();
  processBoiler();

  if (
    mqttClient.connected() &&
    millis() - lastMQTTStatePublish >= MQTT_STATE_INTERVAL
  ) {
    lastMQTTStatePublish = millis();
    publishMQTTState(true);
  }

  delay(10);
}
