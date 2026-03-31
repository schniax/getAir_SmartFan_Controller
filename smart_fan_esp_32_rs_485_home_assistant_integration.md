# SmartFan RS485 to ESP32 to Home Assistant

## Overview
The **getAir SmartFan** decentralized ventilation system uses a simple **RS485 multi‑drop bus** between the wall controller and up to **six fan nodes**. The controller periodically transmits command frames addressed to individual fans.

The **Viessmann Vitovent 100‑D** system appears to be the same hardware platform with different branding. The protocol described in this document was verified on a getAir SmartFan installation but is expected to work identically with Vitovent 100‑D systems.

The protocol is simple and suitable for integration into custom controllers or home‑automation systems (e.g. Home Assistant).

This project replaces the original SmartFan wall controller logic with an **ESP32-based MQTT gateway**.

The ESP32 becomes the **RS485 bus master**, sends valid SmartFan command frames, and exposes each fan node as native entities in **Home Assistant** via **MQTT Discovery**.

Current implementation status:
- Works with up to **6 nodes**
- Sends cyclic RS485 command frames
- Connects to Wi‑Fi and MQTT
- Publishes Home Assistant auto-discovery config
- Exposes per-node control for:
  - Power
  - Speed
  - Direction
- Keeps HA state synchronized with retained MQTT states

Not implemented yet:
- Sensor readback
- Heat-recovery automation
- Non-linear speed mapping refinement

---

## Protocol Summary

SmartFan uses a simple 9-byte RS485 frame:

```text
55 4D 00 96 MM NN DD SS CRC
```

Where:
- `MM` = mode byte
- `NN` = node address (`0..5`)
- `DD` = airflow direction
  - `0x01` = exhaust
  - `0x02` = supply
- `SS` = speed byte
- `CRC` = checksum over bytes 0..7

Checksum formula:

```text
CRC = (0x55 - (sum(bytes[0..7]) & 0xFF)) & 0xFF
```

The current firmware uses:
- `2400 baud`
- `8N1`
- `mode = 0x03`

---

## Hardware

### Existing controller board
The original wall controller PCB contains:
- **PIC16F1509** microcontroller
- **ST3485** RS485 transceiver

The ESP32 reuses the existing **ST3485** on the controller board.

### Required modification
To prevent bus contention, the original PIC must no longer drive the RS485 transmit input.

Typical approach:
- lift or disconnect the **PIC TX pin** that drives the ST3485 input
- connect the ESP32 TX instead

### Wiring

```text
ESP32 GPIO17 (TX) -> ST3485 DI (Pin 4)
ESP32 GPIO16 (RX) -> ST3485 RO (Pin 1)
ESP32 GND         -> Board GND
ESP32 3V3         -> 3.3 V supply
```

Notes:
- GPIO17 is used for RS485 transmit
- GPIO16 is used for RS485 receive/debug sniffing
- The current implementation only actively sends commands
- DE/RE control is not required in this setup because the existing board design already allows transmission with the reused transceiver arrangement

---

## Software Architecture

```text
Home Assistant
      |
      | MQTT
      v
ESP32 gateway
      |
      | UART 2400 8N1
      v
ST3485
      |
      | RS485 multi-drop bus
      v
SmartFan nodes 0..5
```

The ESP32 stores the desired state of each node in RAM and periodically converts this state into RS485 command frames.

---

## PlatformIO Setup

### platformio.ini

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200

lib_deps =
  knolleary/PubSubClient
```

### Required libraries
- `WiFi.h`
- `PubSubClient.h`
- `HardwareSerial.h`

---

## Current Firmware

Save this as `src/main.cpp`.

```cpp
#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <PubSubClient.h>

//
// ========================= CONFIG =========================
//

static const char* WIFI_SSID     = "SSID";
static const char* WIFI_PASSWORD = "SECRET";

static const char* MQTT_HOST     = "IP";
static const uint16_t MQTT_PORT  = 1883;
static const char* MQTT_USER     = "USER";
static const char* MQTT_PASSWORD = "PASSWORD";

static const char* DEVICE_ID           = "smartfan_bridge_1";
static const char* DEVICE_NAME         = "SmartFan Bridge";
static const char* DEVICE_MANUFACTURER = "schniAx";
static const char* DEVICE_MODEL        = "ESP32 RS485 Bridge";

static const uint8_t NUM_NODES = 6;

// RS485 UART
static const int RS485_RX_PIN = 16;   // ST3485 RO -> ESP RX
static const int RS485_TX_PIN = 17;   // ESP TX -> ST3485 DI

// Protocol constants
static const uint8_t FRAME_HEADER_0 = 0x55;
static const uint8_t FRAME_HEADER_1 = 0x4D;
static const uint8_t FRAME_CONST_2  = 0x00;
static const uint8_t FRAME_CONST_3  = 0x96;
static const uint8_t DEFAULT_MODE   = 0x03;

// Timing
static const uint32_t SEND_INTERVAL_MS = 1000;
static const uint32_t MQTT_RETRY_MS    = 2000;
static const uint32_t WIFI_RETRY_MS    = 500;

//
// ========================= TYPES =========================
//

struct FanNode {
  bool power;
  uint8_t speed;         // 0..100 from Home Assistant
  uint8_t directionRaw;  // 0x01 = exhaust, 0x02 = supply
};

//
// ========================= GLOBALS =========================
//

HardwareSerial rs485(2);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

FanNode nodes[NUM_NODES];
uint8_t frame[9];

unsigned long lastSendMs = 0;
unsigned long lastMqttRetryMs = 0;

//
// ========================= HELPERS =========================
//

uint8_t calcCRC(const uint8_t* data) {
  uint16_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += data[i];
  }
  return (uint8_t)((0x55 - (sum & 0xFF)) & 0xFF);
}

void buildFrame(uint8_t mode, uint8_t node, uint8_t direction, uint8_t speed, uint8_t* outFrame) {
  outFrame[0] = FRAME_HEADER_0;
  outFrame[1] = FRAME_HEADER_1;
  outFrame[2] = FRAME_CONST_2;
  outFrame[3] = FRAME_CONST_3;
  outFrame[4] = mode;
  outFrame[5] = node;
  outFrame[6] = direction;
  outFrame[7] = speed;
  outFrame[8] = calcCRC(outFrame);
}

void printFrame(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}

const char* directionRawToText(uint8_t raw) {
  if (raw == 0x02) return "supply";
  return "exhaust";
}

uint8_t directionTextToRaw(const String& txt) {
  if (txt == "supply") {
    return 0x02;
  }
  return 0x01;
}

uint8_t effectiveSpeed(uint8_t nodeIndex) {
  return nodes[nodeIndex].power ? nodes[nodeIndex].speed : 0;
}

String topicBase(uint8_t nodeIndex) {
  return "smartfan/node" + String(nodeIndex);
}

String objectId(const char* suffix, uint8_t nodeIndex) {
  return "smartfan_node" + String(nodeIndex) + "_" + String(suffix);
}

void mqttPublishRetained(const String& topic, const String& payload) {
  mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

void publishAvailability(bool online) {
  mqttClient.publish(
    "smartfan/bridge/status",
    online ? "online" : "offline",
    true
  );
}

void publishNodeState(uint8_t nodeIndex) {
  const String base = topicBase(nodeIndex);

  mqttPublishRetained(base + "/power/state", nodes[nodeIndex].power ? "ON" : "OFF");
  mqttPublishRetained(base + "/speed/state", String(nodes[nodeIndex].speed));
  mqttPublishRetained(base + "/direction/state", directionRawToText(nodes[nodeIndex].directionRaw));
}

void publishAllStates() {
  for (uint8_t i = 0; i < NUM_NODES; i++) {
    publishNodeState(i);
  }
}

//
// ========================= MQTT DISCOVERY =========================
//

String deviceBlockJson() {
  return "\"device\":{"
         "\"identifiers\":[\"" + String(DEVICE_ID) + "\"],"
         "\"name\":\"" + String(DEVICE_NAME) + "\","
         "\"manufacturer\":\"" + String(DEVICE_MANUFACTURER) + "\","
         "\"model\":\"" + String(DEVICE_MODEL) + "\""
         "},";
}

void publishDiscoveryForNode(uint8_t nodeIndex) {
  const String base = topicBase(nodeIndex);
  const String dev  = deviceBlockJson();

  {
    String payload =
      "{"
        + dev +
        "\"name\":\"Node " + String(nodeIndex) + " Power\","
        "\"unique_id\":\"" + objectId("power", nodeIndex) + "\","
        "\"availability_topic\":\"smartfan/bridge/status\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"command_topic\":\"" + base + "/power/set\","
        "\"state_topic\":\"" + base + "/power/state\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"icon\":\"mdi:power\""
      "}";

    mqttPublishRetained(
      "homeassistant/switch/" + objectId("power", nodeIndex) + "/config",
      payload
    );
  }

  {
    String payload =
      "{"
        + dev +
        "\"name\":\"Node " + String(nodeIndex) + " Speed\","
        "\"unique_id\":\"" + objectId("speed", nodeIndex) + "\","
        "\"availability_topic\":\"smartfan/bridge/status\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"command_topic\":\"" + base + "/speed/set\","
        "\"state_topic\":\"" + base + "/speed/state\","
        "\"min\":0,"
        "\"max\":100,"
        "\"step\":1,"
        "\"mode\":\"slider\","
        "\"icon\":\"mdi:fan-speed-1\""
      "}";

    mqttPublishRetained(
      "homeassistant/number/" + objectId("speed", nodeIndex) + "/config",
      payload
    );
  }

  {
    String payload =
      "{"
        + dev +
        "\"name\":\"Node " + String(nodeIndex) + " Direction\","
        "\"unique_id\":\"" + objectId("direction", nodeIndex) + "\","
        "\"availability_topic\":\"smartfan/bridge/status\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"command_topic\":\"" + base + "/direction/set\","
        "\"state_topic\":\"" + base + "/direction/state\","
        "\"options\":[\"exhaust\",\"supply\"],"
        "\"icon\":\"mdi:swap-horizontal\""
      "}";

    mqttPublishRetained(
      "homeassistant/select/" + objectId("direction", nodeIndex) + "/config",
      payload
    );
  }
}

void publishDiscoveryAll() {
  for (uint8_t i = 0; i < NUM_NODES; i++) {
    publishDiscoveryForNode(i);
  }
}

//
// ========================= MQTT =========================
//

bool parseTopic(const String& topic, int& nodeIndex, String& field) {
  const String prefix = "smartfan/node";
  if (!topic.startsWith(prefix)) return false;
  if (!topic.endsWith("/set")) return false;

  int nodeStart = prefix.length();
  int slashAfterNode = topic.indexOf('/', nodeStart);
  if (slashAfterNode < 0) return false;

  String idxStr = topic.substring(nodeStart, slashAfterNode);
  if (idxStr.length() == 0) return false;

  nodeIndex = idxStr.toInt();
  if (nodeIndex < 0 || nodeIndex >= NUM_NODES) return false;

  int slashAfterField = topic.indexOf('/', slashAfterNode + 1);
  if (slashAfterField < 0) return false;

  field = topic.substring(slashAfterNode + 1, slashAfterField);
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr(topic);
  String msg;
  msg.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("MQTT [");
  Serial.print(topicStr);
  Serial.print("] = ");
  Serial.println(msg);

  int nodeIndex = -1;
  String field;

  if (!parseTopic(topicStr, nodeIndex, field)) {
    Serial.println("Ignoring unknown topic");
    return;
  }

  if (field == "power") {
    if (msg == "ON") {
      nodes[nodeIndex].power = true;
    } else if (msg == "OFF") {
      nodes[nodeIndex].power = false;
    } else {
      Serial.println("Invalid power payload");
      return;
    }
  }
  else if (field == "speed") {
    int value = msg.toInt();
    if (value < 0 || value > 100) {
      Serial.println("Invalid speed payload");
      return;
    }
    nodes[nodeIndex].speed = (uint8_t)value;
  }
  else if (field == "direction") {
    if (msg != "exhaust" && msg != "supply") {
      Serial.println("Invalid direction payload");
      return;
    }
    nodes[nodeIndex].directionRaw = directionTextToRaw(msg);
  }
  else {
    Serial.println("Unknown field");
    return;
  }

  publishNodeState((uint8_t)nodeIndex);
}

void subscribeTopics() {
  for (uint8_t i = 0; i < NUM_NODES; i++) {
    const String base = topicBase(i);
    mqttClient.subscribe((base + "/power/set").c_str());
    mqttClient.subscribe((base + "/speed/set").c_str());
    mqttClient.subscribe((base + "/direction/set").c_str());
  }
}

String makeClientId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[32];
  snprintf(buf, sizeof(buf), "esp32-smartfan-%08lX", (uint32_t)(mac & 0xFFFFFFFF));
  return String(buf);
}

bool connectMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);

  Serial.print("Connecting MQTT... ");

  const String clientId = makeClientId();

  bool ok = mqttClient.connect(
    clientId.c_str(),
    MQTT_USER,
    MQTT_PASSWORD,
    "smartfan/bridge/status",
    1,
    true,
    "offline"
  );

  if (!ok) {
    Serial.print("failed, rc=");
    Serial.println(mqttClient.state());
    return false;
  }

  Serial.println("connected");

  publishAvailability(true);
  subscribeTopics();
  publishDiscoveryAll();
  publishAllStates();

  return true;
}

//
// ========================= WIFI =========================
//

void connectWiFiBlocking() {
  Serial.print("Connecting WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(WIFI_RETRY_MS);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

//
// ========================= RS485 SEND =========================
//

void sendAllNodeFrames() {
  for (uint8_t i = 0; i < NUM_NODES; i++) {
    const uint8_t speed = effectiveSpeed(i);
    const uint8_t dir   = nodes[i].directionRaw;

    buildFrame(DEFAULT_MODE, i, dir, speed, frame);
    rs485.write(frame, sizeof(frame));
    rs485.flush();

    Serial.print("RS485 node ");
    Serial.print(i);
    Serial.print(" -> ");
    printFrame(frame, sizeof(frame));

    delay(10);
  }
}

//
// ========================= SETUP / LOOP =========================
//

void setupDefaultState() {
  for (uint8_t i = 0; i < NUM_NODES; i++) {
    nodes[i].power = false;
    nodes[i].speed = 47;
    nodes[i].directionRaw = 0x01;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("SmartFan ESP start");

  setupDefaultState();

  rs485.begin(2400, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  connectWiFiBlocking();
  connectMQTT();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectWiFiBlocking();
  }

  if (!mqttClient.connected()) {
    const unsigned long now = millis();
    if (now - lastMqttRetryMs >= MQTT_RETRY_MS) {
      lastMqttRetryMs = now;
      connectMQTT();
    }
  } else {
    mqttClient.loop();
  }

  const unsigned long now = millis();
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    sendAllNodeFrames();
  }
}
```

---

## What Each Part Does

### 1. `FanNode`
Each physical fan node has one in-memory state structure:

```cpp
struct FanNode {
  bool power;
  uint8_t speed;
  uint8_t directionRaw;
};
```

This allows the ESP32 to decouple:
- incoming MQTT commands
- outgoing RS485 frames

The ESP always sends from this local state table.

### 2. `calcCRC()`
Calculates the checksum required by the SmartFan frame.
Without a valid checksum, the fan nodes will ignore the command.

### 3. `buildFrame()`
Builds one complete 9-byte command frame for a selected node.
This is the protocol core.

### 4. `publishDiscoveryForNode()`
Publishes Home Assistant MQTT Discovery payloads for one node.
This automatically creates the following entities per node:
- switch: power
- number: speed
- select: direction

### 5. `parseTopic()`
Decodes MQTT topics like:

```text
smartfan/node3/speed/set
```

and extracts:
- node index = `3`
- field = `speed`

This is required because the firmware handles all nodes with one generic callback.

### 6. `mqttCallback()`
Applies commands from Home Assistant to the local `nodes[]` state.
Examples:
- `ON` / `OFF`
- speed `0..100`
- direction `exhaust` / `supply`

### 7. `sendAllNodeFrames()`
This is the actual bus output scheduler.
It loops over all 6 nodes and sends one frame per node.

Important:
- the SmartFan bus expects periodic command frames
- the current firmware refreshes all nodes once per second

### 8. `effectiveSpeed()`
Implements a simple rule:
- if power is off, send speed `0`
- otherwise send the configured speed value

This keeps the HA power switch behavior intuitive.

---

## MQTT Topics

For each node, the ESP subscribes to:

```text
smartfan/node0/power/set
smartfan/node0/speed/set
smartfan/node0/direction/set
...
smartfan/node5/power/set
smartfan/node5/speed/set
smartfan/node5/direction/set
```

The ESP publishes retained state topics:

```text
smartfan/node0/power/state
smartfan/node0/speed/state
smartfan/node0/direction/state
```

Availability topic:

```text
smartfan/bridge/status
```

---

## Home Assistant Result

After boot and MQTT connect, Home Assistant should automatically create entities for each node.

Per node:
- `Node X Power`
- `Node X Speed`
- `Node X Direction`

This means up to 18 control entities for 6 nodes.

---

## Build and Flash

1. Create a PlatformIO project for `esp32dev`
2. Put the firmware into `src/main.cpp`
3. Set Wi‑Fi and MQTT credentials in the config section
4. Build and upload
5. Open serial monitor at `115200 baud`

Typical serial output:

```text
SmartFan ESP start
Connecting WiFi...
WiFi connected, IP: 192.168.x.x
Connecting MQTT... connected
RS485 node 0 -> 55 4D 00 96 03 00 01 00 19
```

---

## Current Limitations

### 1. Speed is passed through directly
The current implementation sends the Home Assistant value `0..100` directly as protocol byte 7.
This works for testing, but the original controller appears to use a non-linear mapping.

A future improvement is to map HA percentages to known stable fan values.

### 2. No sensor readback yet
The firmware currently only transmits.
Incoming data from the fan nodes is not yet decoded. (if there are any)


---

## Recommended Next Steps

1. Add speed mapping from HA percentages to measured protocol values
2. Add fan pairing / zone abstraction
3. Add sensor parsing from incoming UART data
4. Add fault handling and watchdog behavior

---

## Practical Notes

- Keep only **one** active source file with `setup()` and `loop()` in `src/`
- The USB serial monitor is separate from the RS485 UART on GPIO16/GPIO17
- MQTT Discovery payloads are published as retained messages so Home Assistant recreates entities reliably after reboot
- The ESP32 is the active master on the bus in this setup

---

## Result

This project provides a working bridge from SmartFan RS485 to Home Assistant:

```text
Home Assistant -> MQTT -> ESP32 -> RS485 -> SmartFan nodes
```

It is already sufficient for stable manual control of all six fan nodes from Home Assistant.

