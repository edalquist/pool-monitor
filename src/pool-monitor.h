#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#include <ArduinoJson.h>

static const char* CONFIG_TOPIC = "particle/config/%s";

void set_DS18B20_Resolutions(uint8_t resolution);
bool doTemperatureCalculations();
bool DS18B20_SamplingComplete();
bool sampleFlowMeters();

bool mqttConnect();
void mqttCallback(char* topic, byte* payload, unsigned int length);
