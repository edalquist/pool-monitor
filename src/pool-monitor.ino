#include <MQTT.h>
#include <OneWire.h>
#include <papertrail.h>
#include <tgmath.h>  // Only needed for the fabs() function...thinking about getting rid of this

#include "credentials.h"
#include "pool-monitor.h"

// https://docs.particle.io/reference/device-os/api/system-thread/system-thread/#system-thread
SYSTEM_THREAD(ENABLED);

PapertrailLogHandler papertailHandler("logs5.papertrailapp.com", 31106,
                                      "PoolMonitor");
SerialLogHandler logHandler;
// OneWire option: ignore the search code for devices/device addresses
#define ONEWIRE_SEARCH 0
// OneWire option: enable the CRC code
#define ONEWIRE_CRC 1
// OneWire option: ignore 16-bit CRC code (redundant since CRC is
// eliminated on prior line)
#define ONEWIRE_CRC16 0

// ds18b20 resolution is determined by the byte written to it's configuration
// register
enum DS18B20_RESOLUTION : uint8_t {
  DS18B20_9BIT = 0x1f,   //   9 bit   93.75 ms conversion time
  DS18B20_10BIT = 0x3f,  //  10 bit  187.50 ms conversion time
  DS18B20_11BIT = 0x5F,  //  11 bit  375.00 ms conversion time
  DS18B20_12BIT = 0x7F,  //  12 bit  750.00 ms conversion time
};

// if ds18b20 resolution is less than full 12-bit, the low bits of the data
// should be masked...
enum DS18B20_RES_MASK : uint8_t {
  DS18B20_9BIT_MASK = 0xf8,
  DS18B20_10BIT_MASK = 0xfc,
  DS18B20_11BIT_MASK = 0xfe,
  DS18B20_12BIT_MASK = 0xff,
};

// ds18b20 conversion time is ALSO determined by the byte written to it's
// configuration register
enum DS18B20_CONVERSION_TIME : uint16_t {
  DS18B20_9BIT_TIME = 94,    //   9 bit   93.75 ms conversion time w/pad
  DS18B20_10BIT_TIME = 188,  //  10 bit  187.50 ms conversion time w/pad
  DS18B20_11BIT_TIME = 375,  //  11 bit  375.00 ms conversion time w/pad
  DS18B20_12BIT_TIME = 750,  //  12 bit  750.00 ms conversion time w/pad
};

#define DS18B20_PIN_ONEWIRE D0
//  match desired enumerated conversion time above
#define DS18B20_CONVERSION_TIME DS18B20_12BIT_TIME
//  match desired enumerated resolution above
#define DS18B20_RESOLUTION DS18B20_12BIT
//  match desired enumerated resolution mask above (low bits at lower
//  resolutions mean nothing)
#define DS18B20_RES_MASK DS18B20_12BIT_MASK
//  define how many DS18B20 CRC failure retries are done before moving on
#define DS18B20_CRC_RETRIES 2
//  returned when a CRC Fail Condition occurs: =2047 decimal...177 degree
//  celsius...way outside of spec
#define DS18B20_FAIL_CRC_VALUE 0x07ff
//  set to a known value, checkerboard pattern (could be used to abort a
//  "going to fail" crc check)
#define DS18B20_TEMP_HI_REG 0x55
//  set to a known value, checkerboard pattern (ditto)
#define DS18B20_TEMP_LO_REG 0xAA

//  defines project specific DS18B20 Sampling Interval, which determines how
//  often to sample the DS18B20 devices...in this code, interval reschedules
//  automatically, but could be changed or implemented as a one-shot.
#define DS18B20_SAMPLE_INTERVAL 1000

// publishing temperature differential, publish the data immediately (subject to
// PUBLISH_MIN_INTERVAL) if its temperature differential from the previous
// PUBLISHED value is greater than this number...used in a floating point
// comparison
// ...An easy way to get quick publishes during testing...grab a probe with your
// hand and raise its temperature
#define PUBLISH_TEMPERATURE_DIFF 0.2

#define NUM_DS18B20_DEVICES 4
const uint8_t DS18B20_OneWire_ADDRESSES[NUM_DS18B20_DEVICES][8] = {
    // 1: INPUT
    0x28, 0xA2, 0xBE, 0x75, 0xD0, 0x01, 0x3C, 0x28,
    // 2: OUTPUT
    0x28, 0x50, 0xBC, 0x75, 0xD0, 0x01, 0x3C, 0xB7,
    // 1: INPUT
    0x28, 0xA2, 0xBE, 0x75, 0xD0, 0x01, 0x3C, 0x28,
    // 2: OUTPUT
    0x28, 0x50, 0xBC, 0x75, 0xD0, 0x01, 0x3C, 0xB7
    // 0x28, 0xBC, 0x75, 0x02, 0x2E, 0x19, 0x01, 0xBC  // 3: DEAD?
};

// current raw readings from temperature sensors
int16_t current_temps_raw[NUM_DS18B20_DEVICES];
// current temperature readings from sensors
float f_current_temps[NUM_DS18B20_DEVICES];
// last published temperatures readings from sensors
float f_current_temps_pub[NUM_DS18B20_DEVICES];

OneWire ds18b20_onewire(DS18B20_PIN_ONEWIRE);  // instantiate the OneWire bus

// set at the beginning of each pass through loop()
system_tick_t currentMillis;
// TESTING CODE , keeps track of the total # of DS18B20 conversions
long int conversion_count_DS18B20;
// TESTING CODE , keeps track of the total # of CRC errors in DS18B20
// conversions
long int crc_error_count_DS18B20;
// TESTING CODE , keeps track of the total # of CRC failures (all tries) in
// DS18B20 conversions
long int crc_fail_count_DS18B20;

// Used for tracking flow meters
void tick1(void);
void tick2(void);
void tick3(void);
void tick4(void);
volatile int tick1Count = 0;
volatile int tick2Count = 0;
volatile int tick3Count = 0;
volatile int tick4Count = 0;
system_tick_t lastTickReadTime = 0;

#define PUBLISH_FLOW_DIFF 0.1
#define NUM_FLOW_SENSORS 4
// current flow rate readings from sensors
float f_current_lpm[NUM_FLOW_SENSORS];
// last published current flow rate readings from sensors
float f_current_lpm_pub[NUM_FLOW_SENSORS];

#define PUBLISH_INTERVAL_SEC 60
uint32_t publishInterval = PUBLISH_INTERVAL_SEC;
system_tick_t lastPublishTime = 0;

time32_t mqttLastConnect = 0UL;
char mqttConfigTopic[128];
char haDeviceName[32];
char haStateTopic[100];

bool tempOffsetsLogged = false;
TempOffsets loadedTempOffsets;
TempOffsets tempOffsets;

// MQTT Client Setup
MQTT mqttClient(MQTT_SERVER, MQTT_PORT, /* buffer */ 512, mqttCallback);
bool mqttDiscoveryPublished = false;

// setup() runs once, when the device is first turned on.
void setup() {
  Serial.begin(9600);

  set_DS18B20_Resolutions(DS18B20_RESOLUTION);

  pinMode(D3, INPUT_PULLDOWN);
  pinMode(D4, INPUT_PULLDOWN);
  pinMode(A3, INPUT_PULLDOWN);
  pinMode(A4, INPUT_PULLDOWN);
  attachInterrupt(D3, tick1, RISING);
  attachInterrupt(D4, tick2, RISING);
  attachInterrupt(A3, tick3, RISING);
  attachInterrupt(A4, tick4, RISING);

  snprintf(mqttConfigTopic, sizeof(mqttConfigTopic), CONFIG_TOPIC, DEVICE_NAME);
  if (MQTT_TESTING) {
    snprintf(haDeviceName, sizeof(haDeviceName), "%s_test", DEVICE_NAME);
  } else {
    snprintf(haDeviceName, sizeof(haDeviceName), "%s", DEVICE_NAME);
  }
  snprintf(haStateTopic, sizeof(haStateTopic), "particle/ha/%s/state",
           haDeviceName);

  EEPROM.get(TEMP_OFFSET_ADDR, loadedTempOffsets);
  if (loadedTempOffsets.version != TEMP_OFFSET_VERSION) {
    tempOffsets = {TEMP_OFFSET_VERSION, 0, 0, 0, 0};
  } else {
    tempOffsets = loadedTempOffsets;
  }
}

double round1(double value) { return (int)(value * 10 + 0.5) / 10.0; }

// loop() runs over and over again, as quickly as it can execute.
void loop() {
  currentMillis = millis();

  if (!tempOffsetsLogged && currentMillis > 2000) {
    Log.info("TempOffsets(%d): %f, %f, %f, %f", tempOffsets.version,
             tempOffsets.offset[0], tempOffsets.offset[1],
             tempOffsets.offset[2], tempOffsets.offset[3]);
    if (loadedTempOffsets.version != TEMP_OFFSET_VERSION) {
      Log.info("TempOffsets were invalid (%d != %d) and reset: %f, %f, %f, %f",
               loadedTempOffsets.version, TEMP_OFFSET_VERSION,
               loadedTempOffsets.offset[0], loadedTempOffsets.offset[1],
               loadedTempOffsets.offset[2], loadedTempOffsets.offset[3]);
    }
    tempOffsetsLogged = true;
  }

  bool mqttConnected = mqttLoop();
  if (mqttConnected && millis() > 5000) {
    publishDiscovery();
  }

  // When ready, update the current DS18B20 temperature readings
  if (DS18B20_SamplingComplete()) {
    bool shouldPublish =
        doTemperatureCalculations() || sampleFlowMeters() ||
        (currentMillis - lastPublishTime) / 1000 > publishInterval;

    if (shouldPublish) {
      StaticJsonDocument<200> doc;
      char key[8];
      for (int i = 0; i < NUM_DS18B20_DEVICES; i++) {
        snprintf(key, sizeof(key), "temp_%d", i);
        doc[key] = round1(f_current_temps[i] + tempOffsets.offset[i]);
      }
      for (int i = 0; i < NUM_DS18B20_DEVICES; i++) {
        snprintf(key, sizeof(key), "flow_%d", i);
        doc[key] = round1(f_current_lpm[i]);
      }

      serializeJson(doc, Serial);
      Serial.println();
      mqttPublishJson(haStateTopic, &doc, false);
      lastPublishTime = currentMillis;
    }
  }
}

bool sampleFlowMeters() {
  double secondsPassed = (currentMillis - lastTickReadTime) / 1000.0;

  f_current_lpm[0] = readLpm(currentMillis, secondsPassed, &tick1Count);
  f_current_lpm[1] = readLpm(currentMillis, secondsPassed, &tick2Count);
  f_current_lpm[2] = readLpm(currentMillis, secondsPassed, &tick3Count);
  f_current_lpm[3] = readLpm(currentMillis, secondsPassed, &tick4Count);
  lastTickReadTime = currentMillis;

  bool changed = false;
  for (int i = 0; i < NUM_FLOW_SENSORS; i++) {
    changed = changed ||
              fabs(f_current_lpm_pub[i] - f_current_lpm[i]) > PUBLISH_FLOW_DIFF;
  }
  if (changed) {
    memcpy(f_current_lpm_pub, f_current_lpm,
           sizeof(f_current_lpm[0]) * NUM_FLOW_SENSORS);
  }
  return changed;
}

// lpm   85 < .. <  62
// gph 1347        982 (2100 rated 1.6A)
// (0.166 GPM / Ft2)
// gpm 12*13*0.166 = 25.896 > 1553.76gph ideal
double readLpm(system_tick_t currentTime, double secondsPassed,
               volatile int* ticksCount) {
  int ticks = *ticksCount;
  *ticksCount = 0;
  return (ticks / secondsPassed) / 0.5;
}

void tick1() { tick1Count++; }
void tick2() { tick2Count++; }
void tick3() { tick3Count++; }
void tick4() { tick4Count++; }

#define HA_SENSOR_EXPIRATION_SEC 300
static const char* HA_DEVICE_MODEL = "photon";
static const char* HA_TEMP_SENSOR_ID = "temp_sensor";
static const char* HA_FLOW_SENSOR_ID = "flow_sensor";

void publishDiscovery() {
  if (mqttDiscoveryPublished) {
    return;
  }
  Log.info("MQTT: Start Publish Discovery: %s", haDeviceName);
  String configTopic;

  StaticJsonDocument<512> doc;

  // Build device descriptions
  haDiscoveryAddDevice(&doc);
  doc["state_topic"] = haStateTopic;
  doc["expire_after"] = HA_SENSOR_EXPIRATION_SEC;
  doc["force_update"] = (bool)true;

  for (int i = 1; i <= NUM_FLOW_SENSORS; i++) {
    doc["unit_of_measurement"] = "L/min";
    doc["name"] = String::format("%s Flow Sensor %d", HA_FRIENDLY_NAME, i);
    doc["unique_id"] =
        String::format("%s_%s_%d", haDeviceName, HA_FLOW_SENSOR_ID, i);
    doc["value_template"] = String::format("{{ value_json.flow_%d }}", i);
    configTopic =
        String::format("%s/sensor/%s/%s_%d/config", MQTT_HA_DISCOVERY_TOPIC,
                       haDeviceName, HA_FLOW_SENSOR_ID, i);
    mqttPublishJson(configTopic.c_str(), &doc, true);
    doc.garbageCollect();
  }

  doc["device_class"] = "temperature";
  doc["unit_of_measurement"] = "°F";
  for (int i = 1; i <= NUM_DS18B20_DEVICES; i++) {
    doc["name"] = String::format("%s Temp Sensor %d", HA_FRIENDLY_NAME, i);
    doc["unique_id"] =
        String::format("%s_%s_%d", haDeviceName, HA_TEMP_SENSOR_ID, i);
    doc["value_template"] = String::format("{{ value_json.temp_%d }}", i);
    configTopic =
        String::format("%s/sensor/%s/%s_%d/config", MQTT_HA_DISCOVERY_TOPIC,
                       haDeviceName, HA_TEMP_SENSOR_ID, i);
    mqttPublishJson(configTopic.c_str(), &doc, true);
    doc.garbageCollect();
  }

  mqttDiscoveryPublished = true;
}

void haDiscoveryAddDevice(JsonDocument* doc) {
  JsonObject device = (*doc).createNestedObject("device");
  device["name"] = HA_FRIENDLY_NAME;
  device["model"] = HA_DEVICE_MODEL;
  device["manufacturer"] = "edalquist";
  JsonArray identifiers = device.createNestedArray("identifiers");
  identifiers.add(haDeviceName);
  identifiers.add(System.deviceID());
}

/**
 * Publish JSON to MQTT
 */
void mqttPublishJson(const char* topic, JsonDocument* doc, bool retain) {
  String formattedTopic;
  if (MQTT_TESTING) {
    formattedTopic = String::format("TEST/%s", topic);
  } else {
    formattedTopic = String(topic);
  }

  size_t docSize = measureJson(*doc);
  char output[docSize + 1];
  serializeJson(*doc, output, sizeof(output));

  // Log.info("MQTT: %s\t%d bytes: %s", formattedTopic.c_str(), docSize,
  // output);
  mqttClient.publish(formattedTopic, output, retain);
}

/**
 * Call loop, if that fails attempt to connect to MQTT server.
 *
 * @return true if MQTT server connected, false if not.
 */
bool mqttLoop() {
  // Call loop, return if successful
  if (mqttClient.loop()) {
    return true;
  }

  // Short circuit if there is no cloud connection
  if (!Particle.connected()) {
    return false;
  }

  // try to connect to the server, at most every 3s
  if ((Time.now() - mqttLastConnect) < 3) {
    return false;
  }

  Log.info("MQTT: Start Connect");
  mqttClient.connect(DEVICE_NAME + System.deviceID(), MQTT_USERNAME,
                     MQTT_PASSWORD);
  mqttLastConnect = Time.now();
  if (!mqttClient.isConnected()) {
    // connection failed
    // TODO need to rate limit these in case of a bad connection
    // publishManager.publish("mqtt/log", "connection failed");
    Log.error("MQTT: Connect Failed");
    return false;
  }

  mqttClient.subscribe(mqttConfigTopic);
  Log.info("MQTT: Subscribed - %s", mqttConfigTopic);

  mqttLastConnect = Time.now();
  // publishManager.publish("mqtt/connection", "established");
  Log.info("MQTT: Connected");
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char p[length + 1];
  memcpy(p, payload, length);
  p[length] = 0;
  Log.info("MQTT Callback: %s\n%s", topic, p);

  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, p);
  // Test if parsing succeeds.
  if (error) {
    Log.error("deserializeJson() failed: %s", error.c_str());
    return;
  }

  serializeJson(doc, Serial);
  Serial.println();

  if (doc["interval"] > 1) {
    publishInterval = doc["interval"];
    Log.info("Updating publish interval to %d", publishInterval);
  }

  if (doc["calibrate"]) {
    Log.info("Calibrating temp sensors: TempOffsets(%d): %f, %f, %f, %f",
             tempOffsets.version, tempOffsets.offset[0], tempOffsets.offset[1],
             tempOffsets.offset[2], tempOffsets.offset[3]);

    // Calculate average of current temps
    float averageTemp = f_current_temps[0];
    for (int i = 1; i < NUM_DS18B20_DEVICES; i++) {
      averageTemp += f_current_temps[i];
    }
    averageTemp = averageTemp / NUM_DS18B20_DEVICES;

    Log.info("Average temp: %f", averageTemp);

    // Assign offset from average for each temp
    for (int i = 0; i < NUM_DS18B20_OFFSETS; i++) {
      if (i < NUM_DS18B20_DEVICES) {
        tempOffsets.offset[i] = averageTemp - f_current_temps[i];
      } else {
        tempOffsets.offset[i] = 0;
      }
    }

    EEPROM.put(TEMP_OFFSET_ADDR, tempOffsets);
    Log.info("Calibration complete: TempOffsets(%d): %f, %f, %f, %f",
             tempOffsets.version, tempOffsets.offset[0], tempOffsets.offset[1],
             tempOffsets.offset[2], tempOffsets.offset[3]);
  }
}

// this function sets the resolution for ALL ds18b20s on an instantiated OneWire
void set_DS18B20_Resolutions(uint8_t resolution) {
  ds18b20_onewire.reset();  // onewire intialization sequence, to be followed by
                            // other commands
  ds18b20_onewire.write(
      0xcc);  // onewire "SKIP ROM" command, selects ALL ds18b20s on bus
  ds18b20_onewire.write(
      0x4e);  // onewire "WRITE SCRATCHPAD" command (requires write to 3
              // registers: 2 hi-lo regs, 1 config reg)
  ds18b20_onewire.write(
      DS18B20_TEMP_HI_REG);  // 1) write known value to temp hi register
  ds18b20_onewire.write(
      DS18B20_TEMP_LO_REG);  // 2) write known value to temp lo register
  ds18b20_onewire.write(resolution);  // 3) write selected resolution to
                                      // configuration registers of all ds18b20s
}

// this function intitalizes simultaneous temperature conversions for ALL
// ds18b20s on an instantiated OneWire
void start_DS18B20_Conversions() {
  ds18b20_onewire.reset();  // onewire intitialization sequence, to be followed
                            // by other commands
  ds18b20_onewire.write(0xcc);  // onewire "SKIP ROM" command, addresses ALL (or
                                // one if there is only one) ds18b20s on bus
  ds18b20_onewire.write(0x44);  // onewire wire "CONVERT T" command, starts
                                // temperature conversion on ALL ds18b20s
}

// this function returns the RAW temperature conversion result of a SINGLE
// selected DS18B20 device (via it's address) If there is a CRC failure in the
// process, the previously converted result is just re-read...a new conversion
// is not started. It is reattempted up to DS18B20_CRC_RETRIES times The pointer
// to a particular DS18B20 was addeed as a parameter for testing purposes  to
// check if a particular DS18B20 device was having issues with the OneWire
// Protocol.   I'm leaving it for now
int16_t read_DS18B20_Conversion(
    const uint8_t addr[8],
    uint8_t ptr)  // if ONLY_ONE DS18B20, take out address reference:
                  // read_DS18B20_Conversion(uint8_t ptr)
{
  byte data[9];
  bool crc_error;
  int crc_retries = 0;

  do {
    ds18b20_onewire.reset();  // onewire intitialization sequence, to be
                              // followed by other commands
    ds18b20_onewire.select(
        addr);  // issues onewire "MATCH ROM" address which selects a SPECIFIC
                // (only one) ds18b20 device
                // if ONLY_ONE DS18B20, replace the line above
                // "ds18b20_onewire.select(addr);" with the one directly below
                //  ds18b20_onewire.write(0xcc);      // onewire "SKIP ROM"
                //  command, selects the ONLY_ONE ds18b20 on bus without needing
                //  address
                //
    ds18b20_onewire.write(
        0xBE);  // onewire "READ SCRATCHPAD" command, to access selected
                // ds18b20's scratchpad reading the bytes (9 available) of the
                // selected ds18b20's scratchpad
    for (int i = 0; i < 9; i++) data[i] = ds18b20_onewire.read();
    // check the crc
    crc_error = (data[8] != OneWire::crc8(data, 8));

    // TESTING Debug Code for CRC --------------------
    //  All of this code simply prints out CRC failures and their successful
    //  resolutions.   The failing CRC data can be compared to the passing CRC
    //  data...its a simple logic analyzer for OneWire CRC failures... this can
    //  be commented out if there is no interest in seeing the CRC errors if
    //  they occur
    float temperature;
    temperature =
        ((int16_t)((data[1] << 8) | (data[0] & DS18B20_RES_MASK))) / 16 * 1.8 +
        32;
    if (crc_error) crc_error_count_DS18B20++;

    if (crc_error && crc_retries <= DS18B20_CRC_RETRIES)
      Serial.printlnf(
          "  CRC err #%02d:  %02x %02x %02x %02x %02x %02x %02x %02x %02x  "
          "device: %02d temp: %0.1f",
          (crc_retries + 1), data[8], data[7], data[6], data[5], data[4],
          data[3], data[2], data[1], data[0], ptr, temperature);
    else if (!crc_error && (crc_retries > 0)) {
      Serial.printlnf(
          "  Actual Data:  %02x %02x %02x %02x %02x %02x %02x %02x %02x  "
          "device: %02d temp: %0.1f",
          data[8], data[7], data[6], data[5], data[4], data[3], data[2],
          data[1], data[0], ptr, temperature);
      Serial.println();
    } else if (crc_error)
      Serial.println();
    // TESTING ----------------------------

  } while ((crc_error && (crc_retries++ < DS18B20_CRC_RETRIES)));

  // if the temperature conversion was successfully read, pass it back...else
  // return the CRC FAIL value
  return (int16_t)(crc_error ? DS18B20_FAIL_CRC_VALUE
                             : ((data[1] << 8) | (data[0] & DS18B20_RES_MASK)));
}

// This code starts a conversion on all DS18B20s simultaneously, and then, later
// when the conversions are finished, reads the results There is only one
// sampled conversion for each DS18B20..if the sampled conversion fails the CRC
// checks, a previous sampled conversion is kept Since there is no rush to get
// these conversions recorded, this function is designed so
// ...that only one conversion read happens on any given pass through it.  This
// avoids cramming
// ...a bunch of execution time into one particular pass of the user code.
bool DS18B20_SamplingComplete() {
  static long prior_DS18B20_interval_start = 10000;
  static long prior_DS18B20_conversion_start = 10000;
  static long current_DS18B20_interval_start = 20000;
  static int16_t temperature_read_raw;
  static uint8_t DS18B20_ptr = 0;
  static bool DS18B20_conversion_reads_in_progress = false;

  // Enter the code body ONLY if within a valid DS18B20 sampling interval window
  // AND prior DS18B20 temperature conversions have had time to complete
  if (((currentMillis - prior_DS18B20_conversion_start) >=
       DS18B20_CONVERSION_TIME) &&
      ((currentMillis - prior_DS18B20_interval_start) >=
       DS18B20_SAMPLE_INTERVAL)) {
    if (!DS18B20_conversion_reads_in_progress && (DS18B20_ptr == 0)) {
      // starts temperature conversions on all DS18B20 devices attached to the
      // OneWire bus
      start_DS18B20_Conversions();
      prior_DS18B20_conversion_start =
          millis();  // capture conversion start so the "reads" can be scheduled
      current_DS18B20_interval_start =
          prior_DS18B20_conversion_start;  // capture the start time so next
                                           // interval can be scheduled
      DS18B20_conversion_reads_in_progress = true;
      conversion_count_DS18B20 +=
          NUM_DS18B20_DEVICES;  // TESTING: keeps track of the # of temperature
                                // conversions since reset
    } else if (DS18B20_conversion_reads_in_progress) {
      // reads one of the DS18B20 temperature conversions
      temperature_read_raw = read_DS18B20_Conversion(
          DS18B20_OneWire_ADDRESSES[DS18B20_ptr],
          DS18B20_ptr);  // if ONLY_ONE DS18B20, take out the address reference

      if (temperature_read_raw != DS18B20_FAIL_CRC_VALUE)
        current_temps_raw[DS18B20_ptr] = temperature_read_raw;
      else
        crc_fail_count_DS18B20++;  // TESTING else keep the old value, there
                                   // were CRC failures on the intial read AND
                                   // retries

      if (++DS18B20_ptr >= NUM_DS18B20_DEVICES)
        DS18B20_conversion_reads_in_progress =
            false;  // all DS18B20 conversions have been read
    } else {  // all sampled conversion have been recorded, so setup for the
              // next DS18B20 sample interval
      DS18B20_ptr = 0;                //  setup to read the sensors again
      prior_DS18B20_interval_start =  // check if (for any reason) it took
                                      // longer than DS18B20_SAMPLE_INTERVAL to
                                      // get the conversions
          ((currentMillis - current_DS18B20_interval_start) >
           DS18B20_SAMPLE_INTERVAL)
              ? millis()
              : current_DS18B20_interval_start;
      return (true);
    }
  }
  return (false);
}

// This does the temperature calculations (from the RAW values) and stores them,
// the latest results are updated and always available within these global
// arrays: RAW values: current_temps_raw[NUM_DS18B20_DEVICES], these are the
// integer values read from the sensors current temperatures:
// f_current_temps[NUM_DS18B20_DEVICES]
bool doTemperatureCalculations() {
  float temperature;
  bool changed = false;
  for (uint8_t i = 0; i < NUM_DS18B20_DEVICES; i++) {
    // temperature = current_temps_raw[i] / 16.0;  // this is the Celsius
    // calculation read from the ds18b20
    temperature =
        current_temps_raw[i] / 16.0 * 1.8 +
        32;  // this is the Farenheit calculation read from the ds18b20
    // force a publish if temperature has changed by more than 1 degree since
    // last published
    changed = changed || fabs(f_current_temps_pub[i] - temperature) >
                             PUBLISH_TEMPERATURE_DIFF;
    f_current_temps[i] = temperature;
  }
  if (changed) {
    memcpy(f_current_temps_pub, f_current_temps,
           sizeof(f_current_temps[0]) * NUM_DS18B20_DEVICES);
  }
  return changed;
}
