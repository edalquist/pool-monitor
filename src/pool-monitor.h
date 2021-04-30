#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#include <ArduinoJson.h>

#define DS18B20_PIN D0
#define FROM_POOL_PIN 0
#define FROM_HEAT_PIN 1

// Header Like Section
struct tempevent_t {
  bool set = false;
  double fromPoolTemp;
  double fromHeaterTemp;
};

struct wifievent_t {
  bool set = false;
  int8_t rssi;
  float strength;
  float quality;
};

struct mqttevent_t {
  uint64_t timestamp = 0UL;
  struct tempevent_t tempEvent;
  struct wifievent_t wifiEvent;
};

void findSensors();
bool mqttConnect();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void addDevice(DynamicJsonDocument* doc);
void publishDiscovery();
void updateTemperatureStatus();
double getTemp(uint8_t addr[8]);
void updateWifiStatus();
void sendWifiStatus();
void updateTempState();
void sendMqttEvents();
char* jsptf(const char* format, ...);
void publishJson(const char* topic, DynamicJsonDocument* doc, bool retain);


static const char* HA_DEVICE_MODEL = "photon";
static const char* HA_POOL_TEMP_ID = "pool_temperature";
static const char* HA_HEAT_TEMP_ID = "heater_temperature";
static const char* HA_WIFI_STRENGTH_ID = "wifi_strength";
static const char* HA_WIFI_QUALITY_ID = "wifi_quality";
static const char* HA_TEMP_TOPIC = "particle/ha/%s/temperature";
static const char* HA_WIFI_TOPIC = "particle/ha/%s/wifi";


static const int DS18B20_MAXRETRY = 3;
static const uint32_t DS18B20_msSampleTime = 2500;
static const uint32_t DS18B20_msPublishTime = 30000;

static const std::chrono::duration<int, std::milli> EVENT_RATE_LIMIT_MILLIS =
    250ms;
static const std::chrono::duration<int> LONG_SLEEP_DURATION =
    60min;  // How long to sleep
static const std::chrono::duration<int> EVENT_REFRESH_INTRV =
    5min;  // Repeat event if it hasn't happened in this time

// How long to wait for the open/close relay to stabilze before reporting
static const std::chrono::duration<uint64_t, std::milli> DEBOUNCE_DELAY_MILLIS =
    500ms;

// How many open/close events to queue at most
static const size_t MAX_EVENT_QUEUE = 20;

static const double EMA_ALPHA = 0.80;

static LEDStatus doorOpenLED(RGB_COLOR_RED, LED_PATTERN_FADE, LED_SPEED_NORMAL,
                             LED_PRIORITY_IMPORTANT);
static LEDStatus doorClosedLED(RGB_COLOR_GREEN, LED_PATTERN_FADE,
                               LED_SPEED_NORMAL, LED_PRIORITY_IMPORTANT);
static LEDStatus sensorErrorLED(RGB_COLOR_ORANGE, LED_PATTERN_BLINK,
                                LED_SPEED_NORMAL, LED_PRIORITY_IMPORTANT);
