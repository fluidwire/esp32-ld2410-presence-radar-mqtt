/*
 * ESP32 LD2410 mmWave Presence Radar (MQTT)
 * -----------------------------------------
 * Reads a Hi-Link HLK-LD2410 24 GHz FMCW radar module over UART and publishes
 * human presence to MQTT. Unlike a PIR sensor, the LD2410 keeps reporting a
 * *stationary* target -- someone sitting still at a desk, reading in bed,
 * watching TV -- so occupancy automations stop timing out on people who have
 * simply stopped moving.
 *
 * What gets published:
 *
 *   MQTT_TOPIC_STATE      "occupied" / "clear"        (retained, on change)
 *   MQTT_TOPIC_EVENT      {"event":"occupied", ...}   (on change, not retained)
 *   MQTT_TOPIC_TELEMETRY  {"presence":true,"moving":{...},"stationary":{...}}
 *                         every TELEMETRY_MS while the radar is connected
 *
 * "occupied" is asserted the instant the radar sees a moving OR stationary
 * target. "clear" is only published after CLEAR_HOLD_MS with no target at all,
 * so a brief dropout (someone bends out of view) does not flap the state.
 *
 * Required libraries (install via Library Manager):
 *   - "ld2410" by Nick Reynolds
 *   - "PubSubClient" by Nick O'Leary
 *
 * Wiring (LD2410 UART is 3.3 V logic, power is 5 V):
 *   LD2410 VCC -> ESP32 5V (VIN)
 *   LD2410 GND -> ESP32 GND
 *   LD2410 TX  -> ESP32 GPIO16 (UART2 RX)
 *   LD2410 RX  -> ESP32 GPIO17 (UART2 TX)
 *   LD2410 OUT -> (not used; the sketch reads the UART data frames instead)
 *
 *   The module talks at 256000 baud by default. If you changed it with the
 *   HLKRadarTool app, update RADAR_BAUD below.
 *
 * Setup:
 *   1. Copy config.h.example to config.h
 *   2. Fill in WiFi + MQTT broker details.
 *   3. Flash. Open Serial Monitor at 115200.
 *
 * Sample MQTT payloads:
 *   fluidwire/demo/presence/state      occupied
 *   fluidwire/demo/presence/event      {"event":"occupied","moving":true,"stationary":false,"distance_cm":182,"uptime_s":41}
 *   fluidwire/demo/presence/telemetry  {"presence":true,"moving":{"detected":true,"distance_cm":182,"energy":67},
 *                                       "stationary":{"detected":true,"distance_cm":190,"energy":41},"uptime_s":45}
 *
 * (c) 2026 Fluidwire. Released under the MIT License.
 * https://github.com/fluidwire
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ld2410.h>
#include "config.h"

// --- Radar UART -------------------------------------------------------------
const int      RADAR_RX_PIN   = 16;        // ESP32 RX  <- LD2410 TX
const int      RADAR_TX_PIN   = 17;        // ESP32 TX  -> LD2410 RX
const uint32_t RADAR_BAUD     = 256000UL;  // LD2410 factory default

// --- Behaviour --------------------------------------------------------------
// How long the radar must report NO target before we publish "clear".
const uint32_t CLEAR_HOLD_MS  = 5UL * 1000UL;

// How often to publish the full telemetry frame (distance + energy).
const uint32_t TELEMETRY_MS   = 5UL * 1000UL;

// If no valid frame has arrived from the radar for this long, warn on Serial
// and publish presence=false telemetry so the dashboard shows the sensor is gone.
const uint32_t RADAR_LOST_MS  = 10UL * 1000UL;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
ld2410       radar;

bool     occupied        = false;  // last published state
uint32_t lastTargetSeen  = 0;      // millis() when the radar last reported any target
uint32_t lastFrame       = 0;      // millis() when radar.read() last returned a frame
uint32_t lastTelemetry   = 0;
bool     radarLostLogged = false;

void connectWiFi() {
  Serial.print(F("[wifi] connecting to "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  Serial.print(F("[wifi] IP: "));
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  while (!mqtt.connected()) {
    Serial.print(F("[mqtt] connecting... "));
    bool ok = strlen(MQTT_USER) > 0
      ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
      : mqtt.connect(MQTT_CLIENT_ID);
    if (ok) {
      Serial.println(F("ok"));
      // Re-assert the retained state so subscribers see it after a reconnect.
      mqtt.publish(MQTT_TOPIC_STATE, occupied ? "occupied" : "clear", true);
    } else {
      Serial.print(F("failed state="));
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

void setupRadar() {
  Serial2.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  delay(200);
  Serial.print(F("[radar] starting LD2410 on UART2... "));
  if (radar.begin(Serial2)) {
    Serial.println(F("ok"));
    if (radar.requestFirmwareVersion()) {
      Serial.print(F("[radar] firmware v"));
      Serial.print(radar.firmware_major_version);
      Serial.print('.');
      Serial.print(radar.firmware_minor_version);
      Serial.print('.');
      Serial.println(radar.firmware_bugfix_version, HEX);
    }
  } else {
    Serial.println(F("no response (check wiring/baud) - will keep listening"));
  }
}

void publishState(bool nowOccupied) {
  occupied = nowOccupied;
  const char *state = occupied ? "occupied" : "clear";
  mqtt.publish(MQTT_TOPIC_STATE, state, true);   // retained

  uint16_t dist = radar.movingTargetDetected()
    ? radar.movingTargetDistance()
    : radar.stationaryTargetDistance();

  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"event\":\"%s\",\"moving\":%s,\"stationary\":%s,\"distance_cm\":%u,\"uptime_s\":%lu}",
           state,
           radar.movingTargetDetected() ? "true" : "false",
           radar.stationaryTargetDetected() ? "true" : "false",
           (unsigned)dist,
           (unsigned long)(millis() / 1000UL));
  mqtt.publish(MQTT_TOPIC_EVENT, buf);
  Serial.print(F("[mqtt] "));
  Serial.println(buf);
}

void publishTelemetry(bool radarOk) {
  char buf[256];
  if (radarOk) {
    snprintf(buf, sizeof(buf),
             "{\"presence\":%s,"
             "\"moving\":{\"detected\":%s,\"distance_cm\":%u,\"energy\":%u},"
             "\"stationary\":{\"detected\":%s,\"distance_cm\":%u,\"energy\":%u},"
             "\"uptime_s\":%lu}",
             radar.presenceDetected() ? "true" : "false",
             radar.movingTargetDetected() ? "true" : "false",
             (unsigned)radar.movingTargetDistance(),
             (unsigned)radar.movingTargetEnergy(),
             radar.stationaryTargetDetected() ? "true" : "false",
             (unsigned)radar.stationaryTargetDistance(),
             (unsigned)radar.stationaryTargetEnergy(),
             (unsigned long)(millis() / 1000UL));
  } else {
    snprintf(buf, sizeof(buf),
             "{\"presence\":false,\"radar\":\"offline\",\"uptime_s\":%lu}",
             (unsigned long)(millis() / 1000UL));
  }
  mqtt.publish(MQTT_TOPIC_TELEMETRY, buf);
  Serial.print(F("[mqtt] "));
  Serial.println(buf);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("[boot] ESP32 LD2410 presence radar -> MQTT"));

  setupRadar();
  connectWiFi();
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  // Drain the radar UART. read() returns true each time a complete data frame
  // has been parsed; the LD2410 streams one roughly every 100 ms.
  if (radar.read()) {
    lastFrame = millis();
    radarLostLogged = false;
  }

  uint32_t now = millis();
  bool radarOk = (lastFrame != 0) && (now - lastFrame < RADAR_LOST_MS);

  if (!radarOk && !radarLostLogged && lastFrame != 0) {
    Serial.println(F("[radar] no frames for 10 s - check wiring / 5 V supply"));
    radarLostLogged = true;
  }

  // --- presence state machine ---
  if (radarOk && radar.presenceDetected()) {
    lastTargetSeen = now;
    if (!occupied) publishState(true);
  } else if (occupied && (now - lastTargetSeen >= CLEAR_HOLD_MS)) {
    publishState(false);
  }

  // --- periodic telemetry ---
  if (now - lastTelemetry >= TELEMETRY_MS || lastTelemetry == 0) {
    lastTelemetry = now;
    publishTelemetry(radarOk);
  }

  delay(10);
}
