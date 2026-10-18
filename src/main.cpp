#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

#include "measurement_hardware.h"

namespace {

constexpr char AP_SSID[] = "Pedal-Kino";
constexpr char AP_PASSWORD[] = "pedalkino";
constexpr char OTA_PASSWORD[] = "pedalkino";
constexpr char CALIBRATION_PASSWORD[] = "pedalkino";
constexpr uint8_t BIKE_COUNT = 4;
constexpr uint8_t MAX_COMPLETED_RIDERS = 32;
constexpr size_t RIDER_NAME_LENGTH = 32;
constexpr uint32_t BROADCAST_INTERVAL_MS = 1000;
constexpr float SIGNIFICANT_POWER_CHANGE_W = 10.0F;
constexpr float CURRENT_NOISE_FLOOR_A = 0.10F;
constexpr float BIKE_POWER_NOISE_FLOOR_W = 5.0F;
constexpr uint8_t LED_READY_PIN = 18;
constexpr uint8_t LED_FAULT_PIN = 19;
constexpr uint8_t LED_ON = LOW;
constexpr uint8_t LED_OFF = HIGH;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
constexpr uint32_t RESTART_DELAY_MS = 1500;
constexpr uint32_t CURRENT_ZERO_CALIBRATION_DURATION_MS = 30000;
constexpr uint32_t VOLTAGE_CALIBRATION_DURATION_MS = 30000;

struct BikeState {
  bool enabled = true;
  uint16_t riderId = 0;
  char riderName[RIDER_NAME_LENGTH] = {};
  float currentA = 0.0F;
  float powerW = 0.0F;
  float energyWh = 0.0F;
  float lastBroadcastPowerW = 0.0F;
};

struct RiderResult {
  uint16_t riderId = 0;
  uint8_t bikeId = 0;
  float energyWh = 0.0F;
  char riderName[RIDER_NAME_LENGTH] = {};
};

struct SystemState {
  float batteryVoltage = 13.4F;
  float batteryCurrentA = 0.0F;
  float batteryPowerW = 0.0F;
  bool batteryCurrentCalibrated = false;
};

AsyncWebServer server(80);
AsyncWebSocket webSocket("/ws");
Preferences preferences;
measurement::Hardware measurementHardware;
BikeState bikes[BIKE_COUNT];
RiderResult completedRiders[MAX_COMPLETED_RIDERS];
uint8_t completedRiderCount = 0;
uint16_t nextRiderId = BIKE_COUNT + 1;
SystemState systemState;
uint32_t lastBroadcastAt = 0;
uint32_t lastEnergyUpdateAt = 0;
bool significantChangePending = true;
bool startupFault = false;
bool measurementFault = true;
bool otaInProgress = false;
bool otaFault = false;
bool accessPointMode = true;
uint32_t restartAtMs = 0;
bool currentZeroCalibrationActive = false;
uint32_t currentZeroCalibrationStartedAt = 0;
uint32_t currentZeroCalibrationSums[BIKE_COUNT] = {};
uint16_t currentZeroCalibrationSampleCount = 0;
bool voltageCalibrationActive = false;
uint8_t voltageCalibrationPoint = 0;
uint32_t voltageCalibrationStartedAt = 0;
uint32_t voltageCalibrationSum = 0;
uint16_t voltageCalibrationSampleCount = 0;
String networkSsid;

void updateStatusLeds() {
  const bool fault = startupFault || measurementFault || otaFault;
  digitalWrite(LED_READY_PIN, fault ? LED_OFF : LED_ON);
  digitalWrite(LED_FAULT_PIN, fault ? LED_ON : LED_OFF);
}

void configureStatusLeds() {
  // Preload the inactive level before changing the pins to outputs. Both LEDs
  // are active LOW according to the schematic.
  digitalWrite(LED_READY_PIN, LED_OFF);
  digitalWrite(LED_FAULT_PIN, LED_OFF);
  pinMode(LED_READY_PIN, OUTPUT);
  pinMode(LED_FAULT_PIN, OUTPUT);
  updateStatusLeds();
}

void saveBikeConfiguration() {
  uint8_t enabledMask = 0;
  for (uint8_t index = 0; index < BIKE_COUNT; ++index) {
    if (bikes[index].enabled) {
      enabledMask |= 1U << index;
    }
  }
  preferences.putUChar("bikeMask", enabledMask);
}

void loadBikeConfiguration() {
  const uint8_t enabledMask = preferences.getUChar("bikeMask", 0x0F);
  for (uint8_t index = 0; index < BIKE_COUNT; ++index) {
    bikes[index].enabled = enabledMask & (1U << index);
  }
}

void setRiderName(const uint16_t riderId, const char *requestedName) {
  if (riderId == 0) return;
  String name = requestedName;
  name.trim();
  if (name.length() >= RIDER_NAME_LENGTH) {
    name = name.substring(0, RIDER_NAME_LENGTH - 1);
  }

  for (BikeState &bike : bikes) {
    if (bike.riderId == riderId) {
      name.toCharArray(bike.riderName, sizeof(bike.riderName));
    }
  }
  for (uint8_t index = 0; index < completedRiderCount; ++index) {
    if (completedRiders[index].riderId == riderId) {
      name.toCharArray(completedRiders[index].riderName,
                       sizeof(completedRiders[index].riderName));
    }
  }
  significantChangePending = true;
}

void currentCalibrationKey(char *key, const size_t keySize,
                           const uint8_t channel, const uint8_t point) {
  snprintf(key, keySize, "c%up%u", channel, point);
}

void loadCalibration() {
  for (uint8_t channel = 0; channel < BIKE_COUNT; ++channel) {
    char maskKey[8];
    snprintf(maskKey, sizeof(maskKey), "c%uMask", channel);
    const uint8_t mask = preferences.getUChar(maskKey, 0);
    for (uint8_t point = 0;
         point < measurement::CURRENT_CALIBRATION_POINT_COUNT; ++point) {
      char rawKey[8];
      currentCalibrationKey(rawKey, sizeof(rawKey), channel, point);
      measurementHardware.setCurrentCalibration(
          channel, point, preferences.getUShort(rawKey, 0),
          (mask & (1U << point)) != 0);
    }
  }

  const uint8_t voltageMask = preferences.getUChar("vMask", 0);
  for (uint8_t point = 0;
       point < measurement::VOLTAGE_CALIBRATION_POINT_COUNT; ++point) {
    char rawKey[8];
    snprintf(rawKey, sizeof(rawKey), "vp%u", point);
    measurementHardware.setVoltageCalibration(
        point, preferences.getUShort(rawKey, 0),
        (voltageMask & (1U << point)) != 0);
  }

  const uint8_t batteryCurrentMask = preferences.getUChar("bMask", 0);
  for (uint8_t point = 0;
       point < measurement::BATTERY_CURRENT_CALIBRATION_POINT_COUNT; ++point) {
    char rawKey[8];
    snprintf(rawKey, sizeof(rawKey), "bp%u", point);
    measurementHardware.setBatteryCurrentCalibration(
        point, preferences.getUShort(rawKey, 0),
        (batteryCurrentMask & (1U << point)) != 0);
  }
}

void saveCurrentCalibration(const uint8_t channel, const uint8_t point,
                            const uint16_t raw, const bool calibrated) {
  char maskKey[8];
  snprintf(maskKey, sizeof(maskKey), "c%uMask", channel);
  uint8_t mask = preferences.getUChar(maskKey, 0);
  if (calibrated) mask |= 1U << point;
  else mask &= ~(1U << point);
  preferences.putUChar(maskKey, mask);

  char rawKey[8];
  currentCalibrationKey(rawKey, sizeof(rawKey), channel, point);
  if (calibrated) preferences.putUShort(rawKey, raw);
  else preferences.remove(rawKey);
  measurementHardware.setCurrentCalibration(channel, point, raw, calibrated);
}

void saveVoltageCalibration(const uint8_t point, const uint16_t raw,
                            const bool calibrated) {
  uint8_t mask = preferences.getUChar("vMask", 0);
  if (calibrated) mask |= 1U << point;
  else mask &= ~(1U << point);
  preferences.putUChar("vMask", mask);

  char rawKey[8];
  snprintf(rawKey, sizeof(rawKey), "vp%u", point);
  if (calibrated) preferences.putUShort(rawKey, raw);
  else preferences.remove(rawKey);
  measurementHardware.setVoltageCalibration(point, raw, calibrated);
}

void saveBatteryCurrentCalibration(const uint8_t point, const uint16_t raw,
                                   const bool calibrated) {
  uint8_t mask = preferences.getUChar("bMask", 0);
  if (calibrated) mask |= 1U << point;
  else mask &= ~(1U << point);
  preferences.putUChar("bMask", mask);

  char rawKey[8];
  snprintf(rawKey, sizeof(rawKey), "bp%u", point);
  if (calibrated) preferences.putUShort(rawKey, raw);
  else preferences.remove(rawKey);
  measurementHardware.setBatteryCurrentCalibration(point, raw, calibrated);
}

const char *powerFlowState() {
  if (!systemState.batteryCurrentCalibrated) return "unknown";
  if (systemState.batteryPowerW > 5.0F) return "charging";
  if (systemState.batteryPowerW < -5.0F) return "discharging";
  return "balanced";
}

void addCalibrationJson(JsonObject calibration, const bool includeLiveValues) {
  const measurement::Snapshot &measurements = measurementHardware.snapshot();
  calibration["averagingWindowMs"] = measurement::ADC_AVERAGING_WINDOW_MS;
  calibration["currentRawPerAmp"] = measurement::CURRENT_RAW_PER_AMP;
  JsonObject zeroCalibration = calibration["currentZeroCalibration"].to<JsonObject>();
  zeroCalibration["active"] = currentZeroCalibrationActive;
  zeroCalibration["durationMs"] = CURRENT_ZERO_CALIBRATION_DURATION_MS;
  zeroCalibration["sampleCount"] = currentZeroCalibrationSampleCount;
  const uint32_t zeroElapsedMs = currentZeroCalibrationActive
                                     ? millis() - currentZeroCalibrationStartedAt
                                     : 0;
  zeroCalibration["elapsedMs"] =
      min(zeroElapsedMs, CURRENT_ZERO_CALIBRATION_DURATION_MS);
  JsonArray currentCalibrations = calibration["currents"].to<JsonArray>();
  for (uint8_t channel = 0; channel < BIKE_COUNT; ++channel) {
    JsonObject currentCalibration = currentCalibrations.add<JsonObject>();
    currentCalibration["channel"] = channel;
    if (includeLiveValues) {
      currentCalibration["liveRaw"] = measurements.current[channel].raw;
    }
    JsonArray points = currentCalibration["points"].to<JsonArray>();
    for (uint8_t point = 0;
         point < measurement::CURRENT_CALIBRATION_POINT_COUNT; ++point) {
      const measurement::CalibrationPoint &saved =
          measurementHardware.currentCalibration(channel, point);
      JsonObject entry = points.add<JsonObject>();
      entry["valueA"] = saved.value;
      entry["calibrated"] = saved.calibrated;
      if (saved.calibrated) entry["raw"] = saved.raw;
      else entry["raw"] = nullptr;
    }
  }

  JsonObject voltageCalibration = calibration["voltage"].to<JsonObject>();
  if (includeLiveValues) {
    voltageCalibration["liveRaw"] = measurements.batteryVoltage.raw;
  }
  JsonObject voltageAveraging =
      voltageCalibration["averaging"].to<JsonObject>();
  voltageAveraging["active"] = voltageCalibrationActive;
  voltageAveraging["point"] = voltageCalibrationPoint;
  voltageAveraging["durationMs"] = VOLTAGE_CALIBRATION_DURATION_MS;
  voltageAveraging["sampleCount"] = voltageCalibrationSampleCount;
  const uint32_t voltageElapsedMs = voltageCalibrationActive
                                        ? millis() - voltageCalibrationStartedAt
                                        : 0;
  voltageAveraging["elapsedMs"] =
      min(voltageElapsedMs, VOLTAGE_CALIBRATION_DURATION_MS);
  JsonArray voltagePoints = voltageCalibration["points"].to<JsonArray>();
  for (uint8_t point = 0;
       point < measurement::VOLTAGE_CALIBRATION_POINT_COUNT; ++point) {
    const measurement::CalibrationPoint &saved =
        measurementHardware.voltageCalibration(point);
    JsonObject entry = voltagePoints.add<JsonObject>();
    entry["valueV"] = saved.value;
    entry["calibrated"] = saved.calibrated;
    if (saved.calibrated) entry["raw"] = saved.raw;
    else entry["raw"] = nullptr;
  }

  JsonObject batteryCurrentCalibration =
      calibration["batteryCurrent"].to<JsonObject>();
  if (includeLiveValues) {
    batteryCurrentCalibration["liveRaw"] = measurements.batteryCurrent.raw;
  }
  batteryCurrentCalibration["calibrated"] =
      measurementHardware.batteryCurrentIsCalibrated();
  JsonArray batteryCurrentPoints =
      batteryCurrentCalibration["points"].to<JsonArray>();
  for (uint8_t point = 0;
       point < measurement::BATTERY_CURRENT_CALIBRATION_POINT_COUNT; ++point) {
    const measurement::CalibrationPoint &saved =
        measurementHardware.batteryCurrentCalibration(point);
    JsonObject entry = batteryCurrentPoints.add<JsonObject>();
    entry["valueA"] = saved.value;
    entry["calibrated"] = saved.calibrated;
    if (saved.calibrated) entry["raw"] = saved.raw;
    else entry["raw"] = nullptr;
  }
}

String calibrationJson() {
  JsonDocument document;
  JsonObject root = document.to<JsonObject>();
  root["format"] = "pedal-kino-calibration";
  root["version"] = 1;
  addCalibrationJson(root, false);
  String output;
  serializeJsonPretty(document, output);
  return output;
}

String telemetryJson() {
  const measurement::Snapshot &measurements = measurementHardware.snapshot();
  JsonDocument document;
  document["type"] = "telemetry";
  document["timestamp"] = millis();

  JsonObject system = document["system"].to<JsonObject>();
  system["batteryPercent"] = nullptr;
  system["batteryVoltage"] = systemState.batteryVoltage;
  if (systemState.batteryCurrentCalibrated) {
    system["batteryCurrentA"] = systemState.batteryCurrentA;
    system["batteryPowerW"] = systemState.batteryPowerW;
  } else {
    system["batteryCurrentA"] = nullptr;
    system["batteryPowerW"] = nullptr;
  }
  system["batteryCurrentCalibrated"] =
      systemState.batteryCurrentCalibrated;
  system["loadPowerW"] = nullptr;
  system["remainingMinutes"] = nullptr;
  system["state"] = powerFlowState();
  system["connectedClients"] = webSocket.count();
  system["ready"] = !startupFault && !measurementFault && !otaFault;
  system["fault"] = startupFault || measurementFault || otaFault;

  JsonObject network = document["network"].to<JsonObject>();
  network["mode"] = accessPointMode ? "accessPoint" : "station";
  network["ssid"] = networkSsid;
  network["ip"] = accessPointMode ? WiFi.softAPIP().toString()
                                  : WiFi.localIP().toString();
  network["hostname"] = accessPointMode ? nullptr : "pedal-kino.local";

  JsonObject ota = document["ota"].to<JsonObject>();
  ota["enabled"] = true;
  ota["inProgress"] = otaInProgress;

  JsonArray bikeArray = document["bikes"].to<JsonArray>();
  for (uint8_t index = 0; index < BIKE_COUNT; ++index) {
    JsonObject bike = bikeArray.add<JsonObject>();
    bike["id"] = index + 1;
    bike["riderId"] = bikes[index].riderId;
    bike["riderName"] = bikes[index].riderName;
    bike["enabled"] = bikes[index].enabled;
    bike["currentA"] = bikes[index].enabled ? bikes[index].currentA : 0.0F;
    bike["voltageV"] = systemState.batteryVoltage;
    bike["powerW"] = bikes[index].enabled ? bikes[index].powerW : 0.0F;
    bike["energyWh"] = bikes[index].energyWh;
    bike["adcRaw"] = measurements.current[index].raw;
    bike["adcRawMin"] = measurements.current[index].rawMin;
    bike["adcRawMax"] = measurements.current[index].rawMax;
    bike["measurementValid"] = measurements.current[index].valid;
  }

  JsonArray leaderboard = document["leaderboard"].to<JsonArray>();
  for (uint8_t index = 0; index < completedRiderCount; ++index) {
    JsonObject result = leaderboard.add<JsonObject>();
    result["riderId"] = completedRiders[index].riderId;
    result["riderName"] = completedRiders[index].riderName;
    result["bikeId"] = completedRiders[index].bikeId;
    result["energyWh"] = completedRiders[index].energyWh;
  }

  JsonObject raw = document["measurements"].to<JsonObject>();
  raw["sequence"] = measurements.sequence;
  raw["sampledAtMs"] = measurements.sampledAtMs;
  JsonObject voltage = raw["batteryVoltage"].to<JsonObject>();
  voltage["pin"] = 33;
  voltage["valueV"] = measurements.batteryVoltage.value;
  voltage["adcRaw"] = measurements.batteryVoltage.raw;
  voltage["adcRawMin"] = measurements.batteryVoltage.rawMin;
  voltage["adcRawMax"] = measurements.batteryVoltage.rawMax;
  voltage["valid"] = measurements.batteryVoltage.valid;
  JsonObject batteryCurrent = raw["batteryCurrent"].to<JsonObject>();
  batteryCurrent["pin"] = 32;
  batteryCurrent["valueA"] = systemState.batteryCurrentCalibrated
                                  ? systemState.batteryCurrentA
                                  : 0.0F;
  batteryCurrent["adcRaw"] = measurements.batteryCurrent.raw;
  batteryCurrent["adcRawMin"] = measurements.batteryCurrent.rawMin;
  batteryCurrent["adcRawMax"] = measurements.batteryCurrent.rawMax;
  batteryCurrent["valid"] = measurements.batteryCurrent.valid;
  batteryCurrent["calibrated"] = systemState.batteryCurrentCalibrated;

  addCalibrationJson(document["calibration"].to<JsonObject>(), true);

  String output;
  serializeJson(document, output);
  return output;
}

void broadcastTelemetry() {
  const String payload = telemetryJson();
  webSocket.textAll(payload);
  for (BikeState &bike : bikes) {
    bike.lastBroadcastPowerW = bike.powerW;
  }
  lastBroadcastAt = millis();
  significantChangePending = false;
}

void setBikePower(const uint8_t index, const float powerW) {
  if (index >= BIKE_COUNT) {
    return;
  }

  const float boundedPowerW = bikes[index].enabled ? max(0.0F, powerW) : 0.0F;
  if (fabsf(boundedPowerW - bikes[index].lastBroadcastPowerW) >= SIGNIFICANT_POWER_CHANGE_W) {
    significantChangePending = true;
  }
  bikes[index].powerW = boundedPowerW;
}

void beginCurrentZeroCalibration() {
  currentZeroCalibrationActive = true;
  currentZeroCalibrationStartedAt = millis();
  currentZeroCalibrationSampleCount = 0;
  for (uint8_t channel = 0; channel < BIKE_COUNT; ++channel) {
    currentZeroCalibrationSums[channel] = 0;
  }
  significantChangePending = true;
}

void collectCurrentZeroCalibration(const measurement::Snapshot &snapshot) {
  if (!currentZeroCalibrationActive) return;
  for (uint8_t channel = 0; channel < BIKE_COUNT; ++channel) {
    currentZeroCalibrationSums[channel] += snapshot.current[channel].raw;
  }
  ++currentZeroCalibrationSampleCount;
  if (millis() - currentZeroCalibrationStartedAt <
          CURRENT_ZERO_CALIBRATION_DURATION_MS ||
      currentZeroCalibrationSampleCount == 0) {
    return;
  }
  for (uint8_t channel = 0; channel < BIKE_COUNT; ++channel) {
    const uint16_t zeroRaw = static_cast<uint16_t>(
        (currentZeroCalibrationSums[channel] +
         currentZeroCalibrationSampleCount / 2) /
        currentZeroCalibrationSampleCount);
    saveCurrentCalibration(channel, 0, zeroRaw, true);
  }
  currentZeroCalibrationActive = false;
  significantChangePending = true;
}

void beginVoltageCalibration(const uint8_t point) {
  voltageCalibrationActive = true;
  voltageCalibrationPoint = point;
  voltageCalibrationStartedAt = millis();
  voltageCalibrationSum = 0;
  voltageCalibrationSampleCount = 0;
  significantChangePending = true;
}

void collectVoltageCalibration(const measurement::Snapshot &snapshot) {
  if (!voltageCalibrationActive) return;
  voltageCalibrationSum += snapshot.batteryVoltage.raw;
  ++voltageCalibrationSampleCount;
  if (millis() - voltageCalibrationStartedAt <
          VOLTAGE_CALIBRATION_DURATION_MS ||
      voltageCalibrationSampleCount == 0) {
    return;
  }
  const uint16_t raw = static_cast<uint16_t>(
      (voltageCalibrationSum + voltageCalibrationSampleCount / 2) /
      voltageCalibrationSampleCount);
  saveVoltageCalibration(voltageCalibrationPoint, raw, true);
  voltageCalibrationActive = false;
  significantChangePending = true;
}

void updateMeasurements() {
  if (!measurementHardware.update(millis())) {
    return;
  }

  const measurement::Snapshot &snapshot = measurementHardware.snapshot();
  collectCurrentZeroCalibration(snapshot);
  collectVoltageCalibration(snapshot);
  systemState.batteryVoltage = snapshot.batteryVoltage.value;
  systemState.batteryCurrentCalibrated =
      measurementHardware.batteryCurrentIsCalibrated();
  if (systemState.batteryCurrentCalibrated) {
    systemState.batteryCurrentA = snapshot.batteryCurrent.value;
    if (fabsf(systemState.batteryCurrentA) < CURRENT_NOISE_FLOOR_A) {
      systemState.batteryCurrentA = 0.0F;
    }
    systemState.batteryPowerW =
        systemState.batteryVoltage * systemState.batteryCurrentA;
  } else {
    systemState.batteryCurrentA = 0.0F;
    systemState.batteryPowerW = 0.0F;
  }
  measurementFault = !snapshot.batteryVoltage.valid;
  if (systemState.batteryCurrentCalibrated) {
    measurementFault = measurementFault || !snapshot.batteryCurrent.valid;
  }
  for (uint8_t index = 0; index < BIKE_COUNT; ++index) {
    measurementFault = measurementFault || !snapshot.current[index].valid;
    float currentA = fabsf(snapshot.current[index].value);
    if (currentA < CURRENT_NOISE_FLOOR_A) {
      currentA = 0.0F;
    }
    float powerW = currentA * systemState.batteryVoltage;
    if (powerW < BIKE_POWER_NOISE_FLOOR_W) {
      currentA = 0.0F;
      powerW = 0.0F;
    }
    bikes[index].currentA = currentA;
    setBikePower(index, powerW);
  }
  updateStatusLeds();
}

void updateEnergyCounters() {
  const uint32_t now = millis();
  if (lastEnergyUpdateAt == 0) {
    lastEnergyUpdateAt = now;
    return;
  }

  const float elapsedHours = static_cast<float>(now - lastEnergyUpdateAt) / 3600000.0F;
  for (BikeState &bike : bikes) {
    bike.energyWh += bike.powerW * elapsedHours;
  }
  lastEnergyUpdateAt = now;
}

void advanceRider(const uint8_t bikeIndex) {
  if (bikeIndex >= BIKE_COUNT) return;
  if (completedRiderCount == MAX_COMPLETED_RIDERS) {
    for (uint8_t index = 1; index < MAX_COMPLETED_RIDERS; ++index) {
      completedRiders[index - 1] = completedRiders[index];
    }
    --completedRiderCount;
  }
  RiderResult &result = completedRiders[completedRiderCount++];
  result.riderId = bikes[bikeIndex].riderId;
  result.bikeId = bikeIndex + 1;
  result.energyWh = bikes[bikeIndex].energyWh;
  strlcpy(result.riderName, bikes[bikeIndex].riderName,
          sizeof(result.riderName));
  bikes[bikeIndex].riderId = nextRiderId++;
  bikes[bikeIndex].riderName[0] = '\0';
  bikes[bikeIndex].energyWh = 0.0F;
  significantChangePending = true;
}

void resetRanking() {
  completedRiderCount = 0;
  nextRiderId = BIKE_COUNT + 1;
  memset(completedRiders, 0, sizeof(completedRiders));
  for (uint8_t index = 0; index < BIKE_COUNT; ++index) {
    bikes[index].riderId = index + 1;
    bikes[index].riderName[0] = '\0';
    bikes[index].energyWh = 0.0F;
  }
  lastEnergyUpdateAt = millis();
  significantChangePending = true;
}

void applyConfiguration(const JsonDocument &document) {
  const JsonArrayConst activeBikes = document["activeBikes"];
  if (activeBikes.size() == BIKE_COUNT) {
    for (uint8_t index = 0; index < BIKE_COUNT; ++index) {
      bikes[index].enabled = activeBikes[index].as<bool>();
      if (!bikes[index].enabled) {
        bikes[index].powerW = 0.0F;
      }
    }
    saveBikeConfiguration();
  }

  const JsonObjectConst wifi = document["wifi"];
  const char *ssid = wifi["ssid"] | "";
  const char *password = wifi["password"] | "";
  if (ssid[0] != '\0') {
    preferences.putString("wifiSsid", ssid);
    preferences.putString("wifiPass", password);
    restartAtMs = millis() + RESTART_DELAY_MS;
  }
  significantChangePending = true;
}

void applyCalibration(const JsonDocument &document) {
  const char *password = document["password"] | "";
  if (strcmp(password, CALIBRATION_PASSWORD) != 0) return;
  const char *sensor = document["sensor"] | "";
  const uint8_t point = document["point"] | 255;
  const bool calibrated = document["calibrated"] | false;
  const measurement::Snapshot &snapshot = measurementHardware.snapshot();

  if (strcmp(sensor, "voltage") == 0) {
    if (point >= measurement::VOLTAGE_CALIBRATION_POINT_COUNT) return;
    if (point != 0 && point != 3) return;
    if (calibrated) {
      if (snapshot.sequence == 0 || voltageCalibrationActive) return;
      beginVoltageCalibration(point);
    } else {
      if (voltageCalibrationActive && voltageCalibrationPoint == point) {
        voltageCalibrationActive = false;
      }
      saveVoltageCalibration(point, 0, false);
    }
  } else if (strcmp(sensor, "batteryCurrent") == 0) {
    if (point >= measurement::BATTERY_CURRENT_CALIBRATION_POINT_COUNT) return;
    if (calibrated && snapshot.sequence == 0) return;
    saveBatteryCurrentCalibration(point, snapshot.batteryCurrent.raw,
                                  calibrated);
  } else {
    return;
  }
  significantChangePending = true;
}

void handleWebSocketMessage(void *argument, uint8_t *data, const size_t length) {
  AwsFrameInfo *frame = static_cast<AwsFrameInfo *>(argument);
  if (!frame->final || frame->index != 0 || frame->len != length || frame->opcode != WS_TEXT) {
    return;
  }

  JsonDocument document;
  if (deserializeJson(document, data, length) != DeserializationError::Ok) {
    return;
  }

  const char *type = document["type"] | "";
  if (strcmp(type, "config") == 0) {
    applyConfiguration(document);
  } else if (strcmp(type, "calibration") == 0) {
    applyCalibration(document);
  } else if (strcmp(type, "currentZeroCalibration") == 0) {
    const char *password = document["password"] | "";
    if (strcmp(password, CALIBRATION_PASSWORD) == 0) {
      beginCurrentZeroCalibration();
    }
  } else if (strcmp(type, "resetEnergy") == 0) {
    const uint8_t bike = document["bike"] | 0;
    if (bike >= 1 && bike <= BIKE_COUNT) {
      bikes[bike - 1].energyWh = 0.0F;
      significantChangePending = true;
    }
  } else if (strcmp(type, "nextRider") == 0) {
    const uint8_t bike = document["bike"] | 0;
    if (bike >= 1 && bike <= BIKE_COUNT) {
      advanceRider(bike - 1);
    }
  } else if (strcmp(type, "riderName") == 0) {
    const uint16_t riderId = document["riderId"] | 0;
    const char *name = document["name"] | "";
    setRiderName(riderId, name);
  } else if (strcmp(type, "resetRanking") == 0) {
    resetRanking();
  } else if (strcmp(type, "requestState") == 0) {
    significantChangePending = true;
  }
}

void onWebSocketEvent(AsyncWebSocket *socket, AsyncWebSocketClient *client,
                      const AwsEventType event, void *argument, uint8_t *data,
                      const size_t length) {
  if (event == WS_EVT_CONNECT) {
    client->text(telemetryJson());
  } else if (event == WS_EVT_DATA) {
    handleWebSocketMessage(argument, data, length);
  }
}

bool configureAccessPoint() {
  WiFi.mode(WIFI_AP);
  const bool started = WiFi.softAP(AP_SSID, AP_PASSWORD);
  accessPointMode = true;
  networkSsid = AP_SSID;
  Serial.printf("Access Point: %s\n", AP_SSID);
  Serial.printf("Web UI: http://%s\n", WiFi.softAPIP().toString().c_str());
  return started;
}

bool configureNetwork() {
  const String stationSsid = preferences.isKey("wifiSsid")
                                 ? preferences.getString("wifiSsid", "")
                                 : String();
  const String stationPassword = preferences.isKey("wifiPass")
                                     ? preferences.getString("wifiPass", "")
                                     : String();
  if (!stationSsid.isEmpty()) {
    Serial.printf("Connecting to WiFi: %s\n", stationSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("pedal-kino");
    WiFi.begin(stationSsid.c_str(), stationPassword.c_str());
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
      accessPointMode = false;
      networkSsid = stationSsid;
      Serial.printf("Connected. Web UI: http://%s\n",
                    WiFi.localIP().toString().c_str());
      if (MDNS.begin("pedal-kino")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS: http://pedal-kino.local");
      }
      return true;
    }
    Serial.println("Home WiFi unavailable; starting fallback access point");
    WiFi.disconnect(true);
  }
  return configureAccessPoint();
}

void configureWebServer() {
  webSocket.onEvent(onWebSocketEvent);
  server.addHandler(&webSocket);
  server.on("/calibration.json", HTTP_GET,
            [](AsyncWebServerRequest *request) {
              AsyncWebServerResponse *response = request->beginResponse(
                  200, "application/json", calibrationJson());
              response->addHeader(
                  "Content-Disposition",
                  "attachment; filename=\"pedal-kino-calibration.json\"");
              response->addHeader("Cache-Control", "no-store");
              request->send(response);
            });
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "application/json", "{\"error\":\"not_found\"}");
  });
  server.begin();
}

void configureOta() {
  ArduinoOTA.setHostname("pedal-kino");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    otaFault = false;
    digitalWrite(LED_READY_PIN, LED_OFF);
    digitalWrite(LED_FAULT_PIN, LED_ON);
    Serial.println("OTA update started");
  });
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    Serial.println("OTA update finished");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    otaFault = true;
    updateStatusLeds();
    Serial.printf("OTA error: %u\n", error);
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready as pedal-kino (port 3232)");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  configureStatusLeds();
  preferences.begin("pedal-kino", false);
  loadBikeConfiguration();
  for (uint8_t index = 0; index < BIKE_COUNT; ++index) {
    bikes[index].riderId = index + 1;
  }

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS could not be mounted");
    startupFault = true;
  }

  measurementHardware.begin();
  loadCalibration();
  updateMeasurements();
  if (!configureNetwork()) {
    Serial.println("Network could not be started");
    startupFault = true;
    updateStatusLeds();
  }
  configureWebServer();
  configureOta();
  lastEnergyUpdateAt = millis();
}

void loop() {
  if (restartAtMs != 0 &&
      static_cast<int32_t>(millis() - restartAtMs) >= 0) {
    ESP.restart();
  }
  ArduinoOTA.handle();
  updateEnergyCounters();

  updateMeasurements();

  const uint32_t now = millis();
  if (significantChangePending || now - lastBroadcastAt >= BROADCAST_INTERVAL_MS) {
    broadcastTelemetry();
  }
  webSocket.cleanupClients();
  delay(10);
}
