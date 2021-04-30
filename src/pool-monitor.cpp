/******************************************************/
//       THIS IS A GENERATED FILE - DO NOT EDIT       //
/******************************************************/

#include "Particle.h"
#line 1 "/Users/edalquist/personal/pool-monitor/src/pool-monitor.ino"
#include "CircularBuffer.h"
#include "credentials.h"
#include "pool-monitor.h"

// This #include statement was automatically added by the Particle IDE.
void watchdogHandler();
void setup();
void loop();
void findSensors();
void mqttCallback(char* topic, byte* payload, unsigned int length);
bool mqttConnect();
void addDevice(DynamicJsonDocument* doc);
void publishDiscovery();
void updateTemperatureStatus();
double getTemp(uint8_t addr[8]);
void updateWifiStatus();
void sendMqttEvents();
bool emptyPublishQueue();
void publishJson(const char* topic, DynamicJsonDocument* doc, bool retain);
#line 6 "/Users/edalquist/personal/pool-monitor/src/pool-monitor.ino"
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#include <ArduinoJson.h>

// This #include statement was automatically added by the Particle IDE.
#include <PublishManager.h>

// This #include statement was automatically added by the Particle IDE.
#include <MQTT.h>
#include <stdarg.h>

#include <DS18B20.h>

#include "math.h"

// Device Config
SYSTEM_THREAD(ENABLED);
STARTUP(WiFi.selectAntenna(ANT_AUTO));
STARTUP(Particle.publishVitals(LONG_SLEEP_DURATION));

// Temp sensor setup
DS18B20 ds18b20(DS18B20_PIN);
// TODO make this a struct look door info on lfl
const int nSENSORS = 2;
float poolTemps[nSENSORS] = {NAN, NAN};
retained uint8_t sensorAddresses[nSENSORS][8];

// MQTT Client Setup
MQTT mqttClient(MQTT_SERVER, MQTT_PORT, mqttCallback, 512);
bool mqttDiscoveryPublished = false;

// Setup logging
SerialLogHandler logHandler;

// Particle Variables
double fromPoolTemp = 0;    // Temp of water coming from pool
double fromHeaterTemp = 0;  // Temp of water coming from heater

char configTopic[128];

// local state
bool foundSensors = false;
time32_t lastConnect = 0UL;
time32_t lastWifiEvent = 0UL;
time32_t lastTempEvent = 0UL;
uint64_t lastMqttEventSent = 0UL;
CircularBuffer<mqttevent_t> eventQueue(MAX_EVENT_QUEUE);

char formatBuffer[128];
char* jsptf(const char* format, ...) {
  va_list va;
  va_start(va, format);
  vsnprintf(formatBuffer, sizeof(formatBuffer), format, va);
  va_end(va);

  return formatBuffer;
}

// Global variable to hold the watchdog object pointer
ApplicationWatchdog* wd;
PublishManager<15> publishManager;

void watchdogHandler() {
  // In 2.0.0 and later, RESET_NO_WAIT prevents notifying the cloud of a pending
  // reset
  System.reset(RESET_NO_WAIT);
}

void setup() {
  // After 60s of no loop completion reset the device
  wd = new ApplicationWatchdog(60s, watchdogHandler);

  Serial.begin(9600);

  Particle.variable("fromPoolTemp", fromPoolTemp);
  Particle.variable("fromHeaterTemp", fromHeaterTemp);

  delay(2000);

  findSensors();

  waitFor(Time.isValid, 30000);
}

void loop() {
  publishManager.process();
  mqttConnect();

  publishDiscovery();

  updateTemperatureStatus();
  updateWifiStatus();
  sendMqttEvents();

  delay(7);
}

void findSensors() {
  if (!foundSensors) {
    int foundcount = 0;
    ds18b20.resetsearch();                 // initialise for sensor search
    for (int i = 0; i < nSENSORS; i++) {   // try to read the sensor addresses
      if (ds18b20.search(sensorAddresses[i])) {
        foundcount++;
        auto addr = sensorAddresses[i];
        Log.info("DS18B20: Sensor %d at %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
          i, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
      }
    }
    foundSensors = nSENSORS == foundcount;
    Log.info("Found: %d of %d temp sensors", foundcount, nSENSORS);
  }
}

/**
 * Called by MQTT library when a subscribed topic is updated.
 */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char p[length + 1];
  memcpy(p, payload, length);
  p[length] = 0;

  publishManager.publish("mqtt/callback", topic);
  Log.info("MQTT: %s\n%s", topic, p);

  // TODO cache this in a global?
  // if (strcmp(configTopic, topic) == 0) {
  //   StaticJsonDocument<200> doc;
  //   DeserializationError error = deserializeJson(doc, payload);

  //   // Test if parsing succeeds.
  //   if (error) {
  //     Log.info("deserializeJson() failed: %s", error.c_str());
  //     // treat parse failure as an empty doc and still update config
  //   }

  //   sleepDelayOverride = doc["sleep_delay"];
  //   Log.info("Sleep Delay Override: %ld", sleepDelayOverride);
  // }
}

/**
 * Call loop, if that fails attempt to connect to MQTT server.
 *
 * @return true if MQTT server connected, false if not.
 */
bool mqttConnect() {
  if (1==1) return false;

  // Call loop, return if successful
  if (mqttClient.loop()) {
    lastConnect = Time.now();
    return true;
  }

  // Short circuit if there is no cloud connection
  if (!Particle.connected()) {
    return false;
  }

  // connect to the server
  Log.info("MQTT: Start Connect");
  mqttClient.connect(DEVICE_NAME + System.deviceID(), MQTT_USERNAME,
                     MQTT_PASSWORD);
  if (!mqttClient.isConnected()) {
    // connection failed
    // TODO need to rate limit these in case of a bad connection
    publishManager.publish("mqtt/log", "connection failed");
    Log.error("MQTT: Connect Failed");
    return false;
  }

  mqttClient.subscribe(configTopic);
  Log.info("MQTT: Subscribed - %s", configTopic);

  lastConnect = Time.now();
  publishManager.publish("mqtt/connection", "established");
  Log.info("MQTT: Connected");
  return true;
}

void addDevice(DynamicJsonDocument* doc) {
  JsonObject device = (*doc).createNestedObject("device");
  device["name"] = HA_FRIENDLY_NAME;
  device["model"] = HA_DEVICE_MODEL;
  device["manufacturer"] = "edalquist";
  JsonArray identifiers = device.createNestedArray("identifiers");
  identifiers.add(DEVICE_NAME);
  identifiers.add(System.deviceID());
}

/**
 * Publish HA MQTT discovery docs
 */
void publishDiscovery() {
  // TODO add a way to reset mqttDiscoveryPublished remotely
  if (mqttDiscoveryPublished) {
    return;
  }
  if (!mqttConnect()) {
    return;
  }

  auto expiration = (LONG_SLEEP_DURATION * 4).count();

  DynamicJsonDocument doc(512);

  // Build device descriptions
  doc.clear();
  addDevice(&doc);
  doc["name"] = jsptf("%s Pool Temp °F", HA_FRIENDLY_NAME);
  doc["unique_id"] = jsptf("%s_%s", DEVICE_NAME, HA_POOL_TEMP_ID);
  doc["device_class"] = "temperature";
  doc["unit_of_measurement"] = "F";
  doc["state_topic"] = jsptf(HA_TEMP_TOPIC, MQTT_DEVICE_NAME);
  doc["value_template"] = jsptf("{{ value_json.%s }}", HA_POOL_TEMP_ID);
  doc["expire_after"] = expiration;
  doc["force_update"] = (bool)true;
  publishJson(jsptf("%s/sensor/%s/%s/config", MQTT_HA_DISCOVERY_TOPIC,
                    MQTT_DEVICE_NAME, HA_POOL_TEMP_ID),
              &doc, true);

  doc.clear();
  addDevice(&doc);
  doc["name"] = jsptf("%s Heater Temp °F", HA_FRIENDLY_NAME);
  doc["unique_id"] = jsptf("%s_%s", DEVICE_NAME, HA_HEAT_TEMP_ID);
  doc["device_class"] = "temperature";
  doc["unit_of_measurement"] = "F";
  doc["state_topic"] = jsptf(HA_TEMP_TOPIC, MQTT_DEVICE_NAME);
  doc["value_template"] = jsptf("{{ value_json.%s }}", HA_HEAT_TEMP_ID);
  doc["expire_after"] = expiration;
  doc["force_update"] = (bool)true;
  publishJson(jsptf("%s/sensor/%s/%s/config", MQTT_HA_DISCOVERY_TOPIC,
                    MQTT_DEVICE_NAME, HA_HEAT_TEMP_ID),
              &doc, true);

  doc.clear();
  addDevice(&doc);
  doc["name"] = jsptf("%s WiFi Strength", HA_FRIENDLY_NAME);
  doc["unique_id"] = jsptf("%s_%s", DEVICE_NAME, HA_WIFI_STRENGTH_ID);
  doc["device_class"] = "signal_strength";
  doc["unit_of_measurement"] = "dBm";
  doc["state_topic"] = jsptf(HA_WIFI_TOPIC, MQTT_DEVICE_NAME);
  doc["value_template"] = "{{ value_json.strength }}";
  doc["expire_after"] = expiration;
  doc["force_update"] = (bool)true;
  publishJson(jsptf("%s/sensor/%s/%s/config", MQTT_HA_DISCOVERY_TOPIC,
                    MQTT_DEVICE_NAME, HA_WIFI_STRENGTH_ID),
              &doc, true);

  doc.clear();
  addDevice(&doc);
  doc["name"] = jsptf("%s WiFi Quality", HA_FRIENDLY_NAME);
  doc["unique_id"] = jsptf("%s_%s", DEVICE_NAME, HA_WIFI_QUALITY_ID);
  doc["device_class"] = "signal_strength";
  doc["unit_of_measurement"] = "dBm";
  doc["state_topic"] = jsptf(HA_WIFI_TOPIC, MQTT_DEVICE_NAME);
  doc["value_template"] = "{{ value_json.quality }}";
  doc["expire_after"] = expiration;
  doc["force_update"] = (bool)true;
  publishJson(jsptf("%s/sensor/%s/%s/config", MQTT_HA_DISCOVERY_TOPIC,
                    MQTT_DEVICE_NAME, HA_WIFI_QUALITY_ID),
              &doc, true);

  // TODO add last update timestamp topic

  mqttDiscoveryPublished = true;
  Log.info("MQTT: Published Discovery");
}

void updateTemperatureStatus() {
  // TODO do we want a running average here?
  float maxDiff = 0;
  for (int i = 0; i < nSENSORS; i++) {
    float temp = getTemp(sensorAddresses[i]);
    if (!isnan(temp)) {
      if (!isnan(poolTemps[i])) {
        maxDiff = max(maxDiff, fabs(temp - poolTemps[i]));
      }
      poolTemps[i] = temp;
    }
  }

  if (maxDiff < 0.1 && Time.now() <= (lastTempEvent + EVENT_REFRESH_INTRV.count())) {
    return;
  }

  mqttevent_t mqttEvent;
  mqttEvent.timestamp = System.millis();
  mqttEvent.tempEvent = {true, poolTemps[FROM_POOL_PIN], poolTemps[FROM_HEAT_PIN]};
  eventQueue.put(mqttEvent);
  lastTempEvent = Time.now();

  publishManager.publish("poolTemp", String::format("pool: %f, heater: %f", poolTemps[FROM_POOL_PIN], poolTemps[FROM_HEAT_PIN]));
}

double getTemp(uint8_t addr[8]) {
  double _temp;
  int   i = 0;

  do {
    _temp = ds18b20.getTemperature(addr);
  } while (!ds18b20.crcCheck() && DS18B20_MAXRETRY > i++);

  if (i < DS18B20_MAXRETRY) {
    _temp = ds18b20.convertToFahrenheit(_temp);
  }
  else {
    _temp = NAN;
    Log.info("DS18B20: Max Retries for %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
  }

  return _temp;
}

void updateWifiStatus() {
  // Short circuit if there is no cloud connection OR not within refresh
  // interval
  if (!Particle.connected() ||
      Time.now() <= (lastWifiEvent + EVENT_REFRESH_INTRV.count())) {
    return;
  }

  WiFiSignal sig = WiFi.RSSI();

  mqttevent_t mqttEvent;
  mqttEvent.timestamp = System.millis();
  mqttEvent.wifiEvent = {true, (int8_t)WiFi.RSSI(), sig.getStrengthValue(),
                         sig.getQualityValue()};
  eventQueue.put(mqttEvent);
  lastWifiEvent = Time.now();
}

time_t lastEventTimestamp = 0UL;
void sendMqttEvents() {
  // Short-circuit if queue empty, no mqtt connect, or rate limited
  if (eventQueue.empty() || !mqttConnect() ||
      System.millis() <=
          (lastMqttEventSent + EVENT_RATE_LIMIT_MILLIS.count())) {
    return;
  }

  // Don't send next event until the same amount of time has passed as between
  // the original events OR 10s has passed as a fail-safe
  if (lastMqttEventSent > 0 && lastEventTimestamp > 0 &&
      !(System.millis() <= (lastMqttEventSent + 10000)) &&
      (System.millis() - lastMqttEventSent) / 1000 >
          eventQueue.peek().timestamp - lastEventTimestamp) {
    return;
  }
  Log.info("Event Queue: %d", eventQueue.size());
  mqttevent_t nextEvent = eventQueue.get();

  if (nextEvent.tempEvent.set) {
    DynamicJsonDocument doc(256);
    doc[HA_POOL_TEMP_ID] = nextEvent.tempEvent.fromPoolTemp;
    doc[HA_HEAT_TEMP_ID] = nextEvent.tempEvent.fromHeaterTemp;
    publishJson(jsptf(HA_TEMP_TOPIC, MQTT_DEVICE_NAME), &doc, true);
  } else if (nextEvent.wifiEvent.set) {
    DynamicJsonDocument doc(256);
    doc["rssi"] = nextEvent.wifiEvent.rssi;
    doc["strength"] = nextEvent.wifiEvent.strength;
    doc["quality"] = nextEvent.wifiEvent.quality;
    publishJson(jsptf(HA_WIFI_TOPIC, MQTT_DEVICE_NAME), &doc, true);
  } else {
    Log.error("UNKNOWN MQTT EVENT");
  }

  lastMqttEventSent = System.millis();

  // Capture previous event timestamp and then zero out global var to signal we
  // need a new event
  lastEventTimestamp = nextEvent.timestamp;
  nextEvent.timestamp = 0UL;
}

bool emptyPublishQueue() {
  publishManager.process();
  return publishManager.cacheSize() <= 0;
}

/**
 * Publish JSON to MQTT
 */
void publishJson(const char* topic, DynamicJsonDocument* doc, bool retain) {
  String formattedTopic;
  if (MQTT_TESTING) {
    formattedTopic = String::format("TEST/%s", topic);
  } else {
    formattedTopic = String(topic);
  }

  char output[measureJson(*doc) + 1];
  serializeJson(*doc, output, sizeof(output));

  Log.info("MQTT: %s\t%s", formattedTopic.c_str(), output);
  mqttClient.publish(formattedTopic, output, retain);
  publishManager.publish("mqtt/publishJson", formattedTopic);
}
