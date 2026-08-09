#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>

namespace {

constexpr char AP_SSID[] = "Pedal-Kino";
constexpr char AP_PASSWORD[] = "pedalkino";
constexpr uint8_t BIKE_COUNT = 4;
constexpr uint32_t BROADCAST_INTERVAL_MS = 1000;
constexpr float SIGNIFICANT_POWER_CHANGE_W = 10.0F;

struct BikeState {
  bool enabled = true;
  float powerW = 0.0F;
  float energyWh = 0.0F;
  float lastBroadcastPowerW = 0.0F;
};

struct SystemState {
  float batteryPercent = 100.0F;
  float batteryVoltage = 13.4F;
  float batteryCurrentA = 0.0F;
  float loadPowerW = 0.0F;
  int32_t remainingMinutes = -1;
};

AsyncWebServer server(80);
AsyncWebSocket webSocket("/ws");
Preferences preferences;
BikeState bikes[BIKE_COUNT];
SystemState systemState;
uint32_t lastBroadcastAt = 0;
uint32_t lastEnergyUpdateAt = 0;
bool significantChangePending = true;

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

const char *powerFlowState() {
  const float generatedPowerW = bikes[0].powerW + bikes[1].powerW + bikes[2].powerW + bikes[3].powerW;
  if (generatedPowerW > systemState.loadPowerW + 5.0F) {
    return "charging";
  }
  if (systemState.loadPowerW > generatedPowerW + 5.0F) {
    return "discharging";
  }
  return "balanced";
}

String telemetryJson() {
  JsonDocument document;
  document["type"] = "telemetry";
  document["timestamp"] = millis();

  JsonObject system = document["system"].to<JsonObject>();
  system["batteryPercent"] = systemState.batteryPercent;
  system["batteryVoltage"] = systemState.batteryVoltage;
  system["batteryCurrentA"] = systemState.batteryCurrentA;
  system["batteryPowerW"] = systemState.batteryVoltage * systemState.batteryCurrentA;
  system["loadPowerW"] = systemState.loadPowerW;
  system["remainingMinutes"] = systemState.remainingMinutes;
  system["state"] = powerFlowState();
  system["connectedClients"] = webSocket.count();

  JsonArray bikeArray = document["bikes"].to<JsonArray>();
  for (uint8_t index = 0; index < BIKE_COUNT; ++index) {
    JsonObject bike = bikeArray.add<JsonObject>();
    bike["id"] = index + 1;
    bike["enabled"] = bikes[index].enabled;
    bike["powerW"] = bikes[index].enabled ? bikes[index].powerW : 0.0F;
    bike["energyWh"] = bikes[index].energyWh;
  }

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

void applyConfiguration(const JsonDocument &document) {
  const JsonArrayConst activeBikes = document["activeBikes"];
  if (activeBikes.size() != BIKE_COUNT) {
    return;
  }

  for (uint8_t index = 0; index < BIKE_COUNT; ++index) {
    bikes[index].enabled = activeBikes[index].as<bool>();
    if (!bikes[index].enabled) {
      bikes[index].powerW = 0.0F;
    }
  }
  saveBikeConfiguration();
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

void configureAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("Access Point: %s\n", AP_SSID);
  Serial.printf("Web UI: http://%s\n", WiFi.softAPIP().toString().c_str());
}

void configureWebServer() {
  webSocket.onEvent(onWebSocketEvent);
  server.addHandler(&webSocket);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "application/json", "{\"error\":\"not_found\"}");
  });
  server.begin();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  preferences.begin("pedal-kino", false);
  loadBikeConfiguration();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS could not be mounted");
  }

  configureAccessPoint();
  configureWebServer();
  lastEnergyUpdateAt = millis();
}

void loop() {
  updateEnergyCounters();

  // Sensor integration point: replace these values with measured channel data.
  setBikePower(0, bikes[0].powerW);
  setBikePower(1, bikes[1].powerW);
  setBikePower(2, bikes[2].powerW);
  setBikePower(3, bikes[3].powerW);

  const uint32_t now = millis();
  if (significantChangePending || now - lastBroadcastAt >= BROADCAST_INTERVAL_MS) {
    broadcastTelemetry();
  }
  webSocket.cleanupClients();
  delay(10);
}
