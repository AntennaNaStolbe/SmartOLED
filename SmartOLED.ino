// === SMART OLED для Home Assistant - AntennaNS Integration ===
/*
  Аппаратная часть:
  - Wemos D1 mini (ESP8266)
  - OLED 0.96" 128x64 I2C
  - Энкодер (CLK, DT, SW)
  - MQTT (через HA broker)

  Библиотеки:
  - GyverOLED
  - GyverEncButton
  - PubSubClient
  - ESP8266WiFi
  - GyverPortal
  - EEPROM

  Интеграция: AntennaNS для Home Assistant
  Автор: AntennaNS
*/

// === Библиотеки ===
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <GyverOLED.h>
#include <EncButton.h>
#include <GyverPortal.h>
#include <EEPROM.h>

// === Настройки отладки ===
#define DEBUG_MODE true  // true - логи включены, false - логи выключены

// === Конфигурация устройства ===
#define DEVICE_TYPE "SmartOLED"

// === Макрос для отладочного вывода ===
#if DEBUG_MODE
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTF(...)
#endif

// === Home Assistant MQTT Discovery ===
#define HA_DISCOVERY_PREFIX "homeassistant"
#define DEVICE_MODEL "SmartOLED"

// === Структура для хранения настроек ===
struct Settings {
  char wifi_ssid[32];
  char wifi_pass[32];
  char mqtt_server[40];
  char mqtt_port[6];
  char mqtt_user[32];
  char mqtt_pass[32];
  char device_id[32];
};

Settings settings = {
  "", "",           // wifi_ssid, wifi_pass
  "",               // mqtt_server
  "1883",           // mqtt_port
  "",       // mqtt_user
  "",       // mqtt_pass
  ""                // device_id
};

// === MQTT топики ===
const char* TOPIC_ACTIONS = "antennans/SmartOLED/%s/actions";
const char* TOPIC_DISPLAY_MAIN_SET    = "antennans/SmartOLED/%s/main";
const char* TOPIC_DISPLAY_SUBNAME_SET = "antennans/SmartOLED/%s/subname";
const char* TOPIC_DISPLAY_SUBVALUE_SET= "antennans/SmartOLED/%s/subvalue";

// === Пины энкодера ===
#define ENC_CLK D6
#define ENC_DT D7
#define ENC_SW D5

// === Глобальные объекты ===
WiFiClient espClient;
PubSubClient mqtt(espClient);
GyverOLED<SSD1306_128x64, OLED_NO_BUFFER> oled;
EncButton enc(ENC_CLK, ENC_DT, ENC_SW, EB_STEP4_LOW);
GyverPortal portal;

// === Структура для хранения данных экрана ===
struct ScreenData {
  String mainValue;
  String subName;
  String subValue;

  String prevMainValue;
  String prevSubName;
  String prevSubValue;
};

//=== Создаем переменные ===
ScreenData screen;
bool configMode = false;
bool isMQTTConnectingShowed = false;
unsigned long mqttConnectionAttempt = 0;
const unsigned long MQTT_TIMEOUT = 10000;
const unsigned long WIFI_TIMEOUT = 15000;

// Флаги обновления дисплея
bool mainUpdated = false;
bool subNameUpdated = false;
bool subValueUpdated = false;

bool discoverySent = false;

String deviceBlock() {

  String dev =
    "\"dev\":{"
    "\"ids\":[\"SmartOLED_" + String(settings.device_id) + "\"],"
    "\"name\":\"SmartOLED_" + String(settings.device_id) + "\","
    "\"mf\":\"AntennaNS\","
    "\"mdl\":\"SmartOLED\""
    "}";

  return dev;
}

void publishTextEntity(const char* name,
                       const char* uniq,
                       const char* cmdTopic) {
  String topic = String(HA_DISCOVERY_PREFIX) + "/text/" + uniq + "/config";

  String payload =
    "{"
    "\"name\":\"" + String(name) + "\","
    "\"uniq_id\":\"" + String(uniq) + "\","
    "\"cmd_t\":\"" + String(cmdTopic) + "\","
    "\"mode\":\"text\","
    + deviceBlock() +
    "}";

  mqtt.publish(topic.c_str(), payload.c_str(), true);
}

void publishTrigger(const char* payloadValue) {

  String topic = String(HA_DISCOVERY_PREFIX) + "/device_automation/SmartOLED_" + String(settings.device_id) + "/" + String(payloadValue) + "/config";

  String payload =
    "{"
    "\"automation_type\":\"trigger\","
    "\"type\":\"action\","
    "\"subtype\":\"SmartOLED_" + String(settings.device_id) + "_" + String(payloadValue) + "\","
    "\"topic\":\"antennans/" + "SmartOLED/" + String(settings.device_id) + "/actions\","
    "\"payload\":\"" + String(payloadValue) + "\","
    + deviceBlock() +
    "}";

  mqtt.publish(topic.c_str(), payload.c_str(), true);
}

void publishDiscovery() {
  if (discoverySent) return;

  // === представление текстовых сущностей ===
  publishTextEntity(
    "Down text",
    ("SmartOLED_" + String(settings.device_id) + "_subvalue").c_str(),
    ("antennans/SmartOLED/" + String(settings.device_id) + "/subvalue").c_str()
  );
  
  publishTextEntity(
    "Big text",
    ("SmartOLED_" + String(settings.device_id) + "_main").c_str(),
    ("antennans/SmartOLED/" + String(settings.device_id) + "/main").c_str()
  );
  
  publishTextEntity(
    "Top text",
    ("SmartOLED_" + String(settings.device_id) + "_subname").c_str(),
    ("antennans/SmartOLED/" + String(settings.device_id) + "/subname").c_str()
  );

  // === ENCODER ACTIONS ===
  publishTrigger("1");
  publishTrigger("2");
  publishTrigger("3");
  publishTrigger("4");
  publishTrigger("5");
  publishTrigger("6");

  publishTrigger("hold");

  publishTrigger("turn_left");
  publishTrigger("turn_right");

  publishTrigger("hold_turn_left");
  publishTrigger("hold_turn_right");

  discoverySent = true;

  DEBUG_PRINTLN("HA MQTT Discovery published");
}


//======================================= РАБОТА С ДИСПЛЕЕМ ======================================
void drawTextLine(int row, uint8_t scale, const String& text) {
  const uint8_t charWidth = 6;
  const uint8_t charHeight = 8;
  uint8_t h = charHeight * scale;
  int y_px = row * charHeight;

  if (text.length() == 0) {
    oled.clear(0, y_px, 127, y_px + h - 1);
    return;
  }

  // Подсчёт количества символов (UTF-8)
  auto countSymbols = [](const String & s) -> int {
    int symbols = 0;
    for (size_t i = 0; i < s.length(); i++) {
      if ((s[i] & 0xC0) != 0x80) symbols++;
    }
    return symbols;
  };

  // Определяем текст, который помещается на экране
  String displayText = text;
  int textWidth = countSymbols(displayText) * charWidth * scale;

  if (textWidth > 128) {
    String tmp = text;
    while (tmp.length() > 0) {
      // Убираем последний символ (учитывая UTF-8)
      size_t p = tmp.length() - 1;
      while (p > 0 && (tmp[p] & 0xC0) == 0x80) p--;
      tmp.remove(p);

      displayText = tmp + "~";
      textWidth = countSymbols(displayText) * charWidth * scale;
      if (textWidth <= 128) break;
    }
  }

  // Центровка
  int xStart = (128 - textWidth) / 2;
  if (xStart < 0) xStart = 0;
  int xEnd = xStart + textWidth - 1;

  // Очищаем слева и справа
  if (xStart > 0) oled.clear(0, y_px, xStart - 1, y_px + h - 1);
  if (xEnd < 126) oled.clear(xEnd, y_px, 127, y_px + h - 1);

  // Вывод текста
  oled.setScale(scale);
  oled.setCursorXY(xStart, y_px);
  oled.print(displayText);
}

// === Отображение режима конфигурации на дисплее ===
void showConfigMode() {
  oled.clear();

  drawTextLine(0, 2, "Режим");
  drawTextLine(2, 2, "настройки");

  oled.setScale(1);
  oled.setCursor(0, 5);
  oled.print("WiFi: SmartOLED_setup");
  oled.setCursor(0, 6);
  oled.print("IP: 192.168.4.1");
  oled.setCursor(0, 7);
  oled.print("подкл. для настройки");
  oled.update();
}

// === Отображение подключения к WiFi на дисплее ===
void showWiFiConnecting() {
  oled.clear();

  drawTextLine(0, 2, "WiFi");

  oled.setScale(1);
  oled.setCursor(0, 3);
  oled.print("Подключение WiFi...");
  oled.update();
}

// === Отображение подключения к MQTT на дисплее ===
void showMQTTConnecting() {
  oled.clear();

  drawTextLine(0, 2, "MQTT");

  oled.setScale(1);
  oled.setCursor(0, 3);
  oled.print("Подключение MQTT...");
  oled.update();
}

// === Отображение ошибки подключения на дисплее ===
void showConnectionError(const String & reason) {
  oled.clear();

  drawTextLine(0, 2, "Ошибка");

  oled.setScale(1);
  oled.setCursor(0, 3);
  oled.print("Причина: " + reason);
  oled.setCursor(0, 5);
  if (reason == "MQTT lost") {
    oled.print("Пытаюсь подключится...");
  } else {
    oled.print("Запуск настроек...");
  }
  oled.update();
}

// === Полное обновление дисплея ===
void drawFullScreen() {
  oled.clear();

  drawTextLine(2, 3,  screen.mainValue);
  drawTextLine(0, 1, screen.subName);
  drawTextLine(6, 2, screen.subValue);

  oled.update();
}

// === Частичное обновление дисплея (обновление строки) ===
void updatePartial() {
  bool updated = false;

  if (screen.mainValue != screen.prevMainValue) {
    screen.prevMainValue = screen.mainValue;
    drawTextLine(2, 3, screen.mainValue);
    updated = true;
  }

  if (screen.subName != screen.prevSubName) {
    screen.prevSubName = screen.subName;
    drawTextLine(0, 1, screen.subName);
    updated = true;
  }

  if (screen.subValue != screen.prevSubValue) {
    screen.prevSubValue = screen.subValue;
    drawTextLine(6, 2, screen.subValue);
    updated = true;
  }

  if (updated) oled.update();
}

//======================================= РАБОТА С WiFi ======================================

// === Попытка подключения к WiFi с таймаутом ===
bool connectWiFiWithTimeout() {
  DEBUG_PRINT("Connecting to WiFi: ");
  DEBUG_PRINTLN(settings.wifi_ssid);

  showWiFiConnecting();

  WiFi.begin(settings.wifi_ssid, settings.wifi_pass);

  unsigned long wifiTimeout = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    DEBUG_PRINT(".");

    if (millis() - wifiTimeout > WIFI_TIMEOUT) {
      DEBUG_PRINTLN("WiFi connection timeout!");
      return false;
    }
  }

  DEBUG_PRINTLN("WiFi connected!");
  DEBUG_PRINT("IP: ");
  DEBUG_PRINTLN(WiFi.localIP());
  return true;
}

//======================================= РАБОТА С MQTT ======================================

// === Подключение к MQTT ===
bool reconnectMQTT() {
  if (mqtt.connected()) return true;

  String clientId = "SmartOLED-" + String(settings.device_id) + "-" + String(random(0xffff), HEX);

  DEBUG_PRINT("MQTT connecting to ");
  DEBUG_PRINT(settings.mqtt_server);
  DEBUG_PRINT(":");
  DEBUG_PRINTLN(settings.mqtt_port);

  if (!isMQTTConnectingShowed) {
    showMQTTConnecting();
    isMQTTConnectingShowed = true;
  }

  if (mqtt.connect(clientId.c_str(), settings.mqtt_user, settings.mqtt_pass)) {
    DEBUG_PRINTLN("MQTT connected!");
    publishDiscovery();
    subscribeTextFields(); // подписка на командные топики для получения текста
    drawFullScreen();

    return true;
  } else {
    DEBUG_PRINT("MQTT failed, rc=");
    DEBUG_PRINTLN(mqtt.state());
    return false;
  }
}

// === Попытка подключения к MQTT с таймаутом ===
bool connectMQTTWithTimeout() {
  mqttConnectionAttempt = millis();

  while (!reconnectMQTT()) {
    delay(500);

    if (millis() - mqttConnectionAttempt > MQTT_TIMEOUT) {
      DEBUG_PRINTLN("MQTT connection timeout!");
      return false;
    }
  }

  return true;
}

// === Callback от MQTT ===
void mqttCallback(char* topic, byte* payload, unsigned int len) {
    payload[len] = '\0';
    String msg = String((char*)payload);

    char main_topic[50], subname_topic[50], subvalue_topic[50];
    snprintf(main_topic, sizeof(main_topic), TOPIC_DISPLAY_MAIN_SET, settings.device_id);
    snprintf(subname_topic, sizeof(subname_topic), TOPIC_DISPLAY_SUBNAME_SET, settings.device_id);
    snprintf(subvalue_topic, sizeof(subvalue_topic), TOPIC_DISPLAY_SUBVALUE_SET, settings.device_id);

    if (strcmp(topic, main_topic) == 0) {
        screen.mainValue = msg;
        mainUpdated = true;
    }
    else if (strcmp(topic, subname_topic) == 0) {
        screen.subName = msg;
        subNameUpdated = true;
    }
    else if (strcmp(topic, subvalue_topic) == 0) {
        screen.subValue = msg;
        subValueUpdated = true;
    }

    DEBUG_PRINT("Received MQTT: ");
    DEBUG_PRINT(topic);
    DEBUG_PRINT(" -> ");
    DEBUG_PRINTLN(msg);
}

// одписка на топики текстов
void subscribeTextFields() {
  char topic[50];

  snprintf(topic, sizeof(topic), TOPIC_DISPLAY_MAIN_SET, settings.device_id);
  mqtt.subscribe(topic);

  snprintf(topic, sizeof(topic), TOPIC_DISPLAY_SUBNAME_SET, settings.device_id);
  mqtt.subscribe(topic);

  snprintf(topic, sizeof(topic), TOPIC_DISPLAY_SUBVALUE_SET, settings.device_id);
  mqtt.subscribe(topic);
}

// === Отправка действий энкодера ===
void sendEncoderAction(const char* action) {
  char topic[64];
  snprintf(topic, sizeof(topic), TOPIC_ACTIONS, settings.device_id);

  mqtt.publish(topic, action);

  DEBUG_PRINT("Encoder action sent: ");
  DEBUG_PRINTLN(action);
}


//======================================= GYVER PORTAL ======================================

// === Построение интерфейса ===
void build() {
  GP.BUILD_BEGIN();
  GP.THEME(GP_DARK);

  GP.TITLE("Настройка SmartOLED");
  GP.HR();

  GP.FORM_BEGIN("/save");

  GP.BLOCK_TAB_BEGIN("Настройки WiFi");
  GP.TEXT("wifi_ssid", "WiFi SSID", settings.wifi_ssid);
  GP.TEXT("wifi_pass", "WiFi Password", settings.wifi_pass, "password");
  GP.BLOCK_END();

  GP.BLOCK_TAB_BEGIN("Настройки MQTT");
  GP.TEXT("mqtt_server", "MQTT Server", settings.mqtt_server);
  GP.TEXT("mqtt_port", "MQTT Port", settings.mqtt_port);
  GP.TEXT("mqtt_user", "MQTT User", settings.mqtt_user);
  GP.TEXT("mqtt_pass", "MQTT Password", settings.mqtt_pass, "password");
  GP.BLOCK_END();

  GP.BLOCK_TAB_BEGIN("Настройки устройства");
  GP.TEXT("device_id", "Device ID(для HA)", settings.device_id);
  GP.BLOCK_END();

  GP.SUBMIT("Сохранить");
  GP.FORM_END();

  GP.BUILD_END();
}

// === Запуск портала настройки ===
void startConfigPortal() {
  configMode = true;
  showConfigMode();

  DEBUG_PRINTLN("Starting config portal (simple mode)...");

  // Создаем точку доступа
  WiFi.mode(WIFI_AP);
  WiFi.softAP("SmartOLED_setup", "");

  DEBUG_PRINTLN("AP started: SmartOLED_setup");
  DEBUG_PRINT("IP: ");
  DEBUG_PRINTLN(WiFi.softAPIP());

  // Настраиваем портал
  portal.attachBuild(build);
  portal.attach(action);
  portal.start();

  // Цикл обработки
  while (configMode) {
    portal.tick();
    delay(100);
  }
}

// === Обработка сохранения настроек ===
void action() {
  if (portal.form()) {
    portal.copyStr("wifi_ssid", settings.wifi_ssid);
    portal.copyStr("wifi_pass", settings.wifi_pass);
    portal.copyStr("mqtt_server", settings.mqtt_server);
    portal.copyStr("mqtt_port", settings.mqtt_port);
    portal.copyStr("mqtt_user", settings.mqtt_user);
    portal.copyStr("mqtt_pass", settings.mqtt_pass);
    portal.copyStr("device_id", settings.device_id);

    DEBUG_PRINTLN("Settings updated!");

    // Сохраняем в EEPROM
    saveSettings();

    // Перезагружаемся
    DEBUG_PRINTLN("Restarting...");
    ESP.restart();
  }
}

//======================================= РАБОТА С EEPROM ======================================

// === Сохранение настроек в EEPROM ===
void saveSettings() {
  EEPROM.begin(sizeof(Settings));
  EEPROM.put(0, settings);
  EEPROM.commit();
  EEPROM.end();
  DEBUG_PRINTLN("Settings saved to EEPROM!");
}

// === Загрузка настроек из EEPROM ===
void loadSettings() {
  EEPROM.begin(sizeof(Settings));
  EEPROM.get(0, settings);
  EEPROM.end();

  // Проверяем валидность настроек
  if (strlen(settings.device_id) == 0) {
    DEBUG_PRINTLN("No saved settings, using defaults");
    // Восстанавливаем дефолтные значения
    strcpy(settings.mqtt_server, "");
    strcpy(settings.mqtt_port, "1883");
    strcpy(settings.mqtt_user, "");
    strcpy(settings.mqtt_pass, "");
    strcpy(settings.device_id, "");
  } else {
    DEBUG_PRINTLN("Settings loaded from EEPROM");
  }
}

// === Очистка EEPROM (дя сброса настроек) ===
void clearEEPROM() {
  DEBUG_PRINTLN("Clearing EEPROM...");

  EEPROM.begin(sizeof(Settings));

  // Заполняем всю область EEPROM нулями
  for (int i = 0; i < sizeof(Settings); i++) {
    EEPROM.write(i, 0);
  }

  EEPROM.commit();
  EEPROM.end();

  DEBUG_PRINTLN("EEPROM cleared successfully!");
  DEBUG_PRINTLN("Device will restart...");

  delay(1000);
  ESP.restart();
}

//======================================= НАСТРОЙКА И ЦИКЛ ======================================

void setup() {
#if DEBUG_MODE
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== AntennaNS Smart OLED ===");
#endif

  // Инициализация дисплея
  oled.init();
  oled.clear();

  // Показ заставки
  oled.setScale(2);
  oled.setCursor(10, 1);
  oled.print("SmartOLED");
  oled.setScale(1);
  oled.setCursor(25, 4);
  oled.print("By AntennaNS");
  oled.update();
  delay(2000);

  // Инициализация значений по умолчанию
  screen.mainValue = "В";
  screen.subName = "ЖДУ СООБЩЕНИЙ";
  screen.subValue = "MQTT";
  screen.prevMainValue = "";
  screen.prevSubName = "";
  screen.prevSubValue = "";

  // Загружаем настройки из EEPROM
  loadSettings();

  // Проверяем, есть ли сохраненные настройки WiFi
  bool hasWiFiSettings = (strlen(settings.wifi_ssid) > 0);

  if (hasWiFiSettings) {
    DEBUG_PRINTLN("Trying to connect with saved settings...");

    // Пытаемся подключиться к WiFi
    if (!connectWiFiWithTimeout()) {
      showConnectionError("WiFi timeout");
      delay(2000);
      startConfigPortal();
    } else {
      // WiFi подключен, пробуем MQTT
      mqtt.setServer(settings.mqtt_server, atoi(settings.mqtt_port));
      mqtt.setCallback(mqttCallback);
      mqtt.setBufferSize(4096); // уеличиваем буфер PubSubClient. Без увеличения discovery не работает

      if (!connectMQTTWithTimeout()) {
        showConnectionError("MQTT timeout");
        delay(2000);
        startConfigPortal();
      } else {
        DEBUG_PRINTLN("Connected successfully!");
      }
    }
  } else {
    // Нет сохраненных настроек - запускаем портал
    DEBUG_PRINTLN("No saved settings, starting config portal");
    startConfigPortal();
  }
}

void loop() {
  if (configMode) {
    // В режиме конфигурации просто обрабатываем портал
    portal.tick();
    return;
  }

  // Поддержание MQTT соединения
  if (!mqtt.connected()) {
    DEBUG_PRINTLN("MQTT disconnected!");
    if (!connectMQTTWithTimeout()) {
      showConnectionError("MQTT lost");
      delay(2000);
    }
  }

  mqtt.loop();

  // ===== Обновление дисплея =====
  if (mainUpdated || subNameUpdated || subValueUpdated) {
    if (mainUpdated && subNameUpdated && subValueUpdated) {
      drawFullScreen(); // Все три строки новые — рисуем весь экран
    } else {
      updatePartial();  // Только часть
    }

    // Сбрасываем флаги
    mainUpdated = subNameUpdated = subValueUpdated = false;
  }

  // Обработка энкодера
  enc.tick();

  // Обработка кликов
  if (enc.hasClicks()) {
    byte clicks = enc.getClicks();
    if (clicks <= 6) {
      DEBUG_PRINT("Encoder clicks: ");
      DEBUG_PRINTLN(clicks);
      sendEncoderAction(String(clicks).c_str()); // Преобразуем число в const char*
    } else if (clicks == 10) {
      clearEEPROM();
    }
  }

  //обработка долгого нажатия
  if (enc.hold()) {
    DEBUG_PRINTLN("Encoder hold!");
    sendEncoderAction("hold");
  }

  //обработка поворота энкодера влево (обычного)
  if (enc.left()) {
    DEBUG_PRINTLN("Encoder turn left!");
    sendEncoderAction("turn_left");
  }

  //обработка поворота энкодера с удержанием влево
  if (enc.leftH()) {
    DEBUG_PRINTLN("Encoder hold turn left!");
    sendEncoderAction("hold_turn_left");
  }

  //обработка поворота энкодера впарво (обычного)
  if (enc.right()) {
    DEBUG_PRINTLN("Encoder turn right!");
    sendEncoderAction("turn_right");
  }

  //обработка поворота энкодера с удержанием влево
  if (enc.rightH()) {
    DEBUG_PRINTLN("Encoder hold turn right!");
    sendEncoderAction("hold_turn_right");
  }
}
