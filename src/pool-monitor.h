#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#include <ArduinoJson.h>

#define TEMP_OFFSET_ADDR 0
#define TEMP_OFFSET_VERSION 3
#define NUM_DS18B20_OFFSETS 4
struct TempOffsets {
  uint8_t version;
  float offset[NUM_DS18B20_OFFSETS];
};

static const char* CONFIG_TOPIC = "particle/config/%s";

void set_DS18B20_Resolutions(uint8_t resolution);
bool doTemperatureCalculations();
bool DS18B20_SamplingComplete();
bool sampleFlowMeters();

bool mqttLoop();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttPublishJson(const char* topic, JsonDocument* doc, bool retain);

char* jsptf(const char* format, ...);

void publishDiscovery();
void haDiscoveryAddDevice(JsonDocument* doc);
