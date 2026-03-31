#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <PubSubClient.h>

// User config
static const char* WIFI_SSID     = "YOURSSID";
static const char* WIFI_PASSWORD = "YOURPW";

static const char* MQTT_HOST     = "YOURMQTTIP";
static const uint16_t MQTT_PORT  = 1883;
static const char* MQTT_USER     = "YOURMQTTIPUSER";
static const char* MQTT_PASSWORD = "YOURMQTTIPPW";

static const char* DEVICE_ID_PREFIX    = "smartfan_bridge";
static const char* DEVICE_NAME_PREFIX  = "SmartFan Bridge";
static const char* DEVICE_MANUFACTURER = "schniax";
static const char* DEVICE_MODEL        = "ESP32 RS485 Bridge";
static const char* MQTT_ROOT_PREFIX    = "smartfan";

static const uint8_t NUM_NODES = 6;

// RS485
static const int RS485_RX_PIN  = 16;
static const int RS485_TX_PIN  = 17;
static const int RS485_DIR_PIN = 4;   // HIGH=TX, LOW=RX
static const uint32_t RS485_BAUD = 2400;

// Protocol
static const uint8_t DIR_EXHAUST = 0x01;
static const uint8_t DIR_SUPPLY  = 0x02;
static const uint8_t SPEED_MAX   = 100;
static const uint8_t DEFAULT_SPEED_PERCENT = 25;

// Timing
static const uint32_t WIFI_RETRY_MS               = 500;
static const uint32_t MQTT_RETRY_MS               = 2000;
static const uint32_t COMMAND_CYCLE_MS            = 1000;
static const uint32_t INTERFRAME_GAP_MS           = 10;
static const uint32_t POLL_INTERVAL_MS            = 1500;
static const uint32_t SENSOR_REQUEST_INTERVAL_MS  = 3000;
static const uint32_t SENSOR_LISTEN_WINDOW_MS     = 120;
static const uint32_t NODE_TIMEOUT_MS             = 20000;
static const uint32_t SENSOR_STALE_MS             = 60000;
static const uint32_t RX_STALE_PARTIAL_MS         = 50;

struct FanNode {
  uint8_t speedPercent = DEFAULT_SPEED_PERCENT;   // 0..100, 0 = off
  uint8_t directionRaw = DIR_EXHAUST;

  bool online = false;
  bool sensorPresent = false;
  bool hasSensorValues = false;
  uint8_t lastHumidity = 0;
  float lastTemperatureC = 0.0f;
  uint8_t lastReportedDirectionRaw = DIR_EXHAUST;

  uint32_t lastSeenMs = 0;
  uint32_t lastSensorMs = 0;
};

HardwareSerial rs485(2);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

FanNode nodes[NUM_NODES];

static uint8_t rxBuf[64];
static size_t rxLen = 0;
static uint32_t lastRxByteMs = 0;

uint32_t lastCommandCycleMs = 0;
uint32_t lastPollMs = 0;
uint32_t lastMqttRetryMs = 0;
uint32_t lastSensorRequestScheduleMs = 0;
uint32_t sensorListenUntilMs = 0;
uint8_t nextPollNode = 0;
uint8_t nextSensorNode = 0;
uint8_t lastPolledNode = 0xFF;

String gatewayId;
String gatewayName;
String mqttRoot;
String bridgeStatusTopic;

void setRs485ReceiveMode() { digitalWrite(RS485_DIR_PIN, LOW); }
void setRs485TransmitMode() { digitalWrite(RS485_DIR_PIN, HIGH); }

uint8_t calcChecksum(const uint8_t* data, size_t lenWithoutChecksum) {
  uint16_t sum = 0;
  for (size_t i = 0; i < lenWithoutChecksum; i++) sum += data[i];
  return static_cast<uint8_t>((0x55 - (sum & 0xFF)) & 0xFF);
}

String bytesToHex(const uint8_t* data, size_t len) {
  String s;
  s.reserve(len * 3);
  for (size_t i = 0; i < len; i++) {
    if (i) s += ' ';
    if (data[i] < 0x10) s += '0';
    s += String(data[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void printFrame(const char* prefix, const uint8_t* data, size_t len) {
  Serial.print(prefix);
  Serial.print(' ');
  Serial.println(bytesToHex(data, len));
}

const char* directionRawToText(uint8_t raw) {
  return (raw == DIR_SUPPLY) ? "supply" : "exhaust";
}

uint8_t directionTextToRaw(const String& txt) {
  return (txt == "supply") ? DIR_SUPPLY : DIR_EXHAUST;
}

uint8_t sanitizeSpeedPercent(int value) {
  if (value < 0) return 0;
  if (value > SPEED_MAX) return SPEED_MAX;
  return static_cast<uint8_t>(value);
}

uint8_t effectiveSpeedRaw(uint8_t nodeIndex) {
  return sanitizeSpeedPercent(nodes[nodeIndex].speedPercent);
}

void initGatewayIdentity() {
  uint64_t mac = ESP.getEfuseMac();
  char suffix[13];
  snprintf(suffix, sizeof(suffix), "%012llX", static_cast<unsigned long long>(mac & 0xFFFFFFFFFFFFULL));
  gatewayId = String(DEVICE_ID_PREFIX) + "_" + suffix;
  gatewayName = String(DEVICE_NAME_PREFIX) + " " + suffix;
  mqttRoot = String(MQTT_ROOT_PREFIX) + "/" + gatewayId;
  bridgeStatusTopic = mqttRoot + "/bridge/status";
}

String topicBase(uint8_t nodeIndex) {
  return mqttRoot + "/node" + String(nodeIndex);
}

String objectId(const char* suffix, uint8_t nodeIndex) {
  return gatewayId + "_node" + String(nodeIndex) + "_" + String(suffix);
}

String discoveryObjectTopic(const char* component, const char* suffix, uint8_t nodeIndex) {
  return "homeassistant/" + String(component) + "/" + objectId(suffix, nodeIndex) + "/config";
}

void mqttPublishRetained(const String& topic, const String& payload) {
  mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

void mqttClearRetained(const String& topic) {
  mqttClient.publish(topic.c_str(), "", true);
}

void publishAvailability(bool online) {
  mqttClient.publish(bridgeStatusTopic.c_str(), online ? "online" : "offline", true);
}

String deviceBlockJson() {
  return "\"device\":{"
         "\"identifiers\":[\"" + gatewayId + "\"],"
         "\"name\":\"" + gatewayName + "\" ,"
         "\"manufacturer\":\"" + String(DEVICE_MANUFACTURER) + "\" ,"
         "\"model\":\"" + String(DEVICE_MODEL) + "\""
         "},";
}

bool sensorValueFresh(uint8_t nodeIndex) {
  return nodes[nodeIndex].hasSensorValues && (millis() - nodes[nodeIndex].lastSensorMs <= SENSOR_STALE_MS);
}

void publishNodeState(uint8_t nodeIndex) {
  const String base = topicBase(nodeIndex);
  const FanNode& node = nodes[nodeIndex];
  const bool fresh = sensorValueFresh(nodeIndex);

  mqttPublishRetained(base + "/speed/state", String(node.speedPercent));
  mqttPublishRetained(base + "/direction/state", directionRawToText(node.directionRaw));
  mqttPublishRetained(base + "/online/state", node.online ? "ON" : "OFF");
  mqttPublishRetained(base + "/sensor_present/state", node.sensorPresent ? "ON" : "OFF");

  if (fresh) {
    mqttPublishRetained(base + "/humidity/state", String(node.lastHumidity));
    mqttPublishRetained(base + "/temperature/state", String(node.lastTemperatureC, 1));
    mqttPublishRetained(base + "/reported_direction/state", directionRawToText(node.lastReportedDirectionRaw));
  } else {
    mqttClearRetained(base + "/humidity/state");
    mqttClearRetained(base + "/temperature/state");
    mqttClearRetained(base + "/reported_direction/state");
  }
}

void publishAllStates() {
  for (uint8_t i = 0; i < NUM_NODES; i++) publishNodeState(i);
}

void publishDiscoveryForNode(uint8_t nodeIndex) {
  const String base = topicBase(nodeIndex);
  const String dev  = deviceBlockJson();

  {
    String payload = "{" + dev;
    payload += "\"name\":\"Node " + String(nodeIndex) + " Speed\",";
    payload += "\"unique_id\":\"" + objectId("speed", nodeIndex) + "\",";
    payload += "\"availability_topic\":\"" + bridgeStatusTopic + "\",";
    payload += "\"payload_available\":\"online\",";
    payload += "\"payload_not_available\":\"offline\",";
    payload += "\"command_topic\":\"" + base + "/speed/set\",";
    payload += "\"state_topic\":\"" + base + "/speed/state\",";
    payload += "\"min\":0,";
    payload += "\"max\":100,";
    payload += "\"step\":1,";
    payload += "\"mode\":\"slider\",";
    payload += "\"icon\":\"mdi:fan-speed-1\"}";
    mqttPublishRetained(discoveryObjectTopic("number", "speed", nodeIndex), payload);
  }

  {
    String payload = "{" + dev;
    payload += "\"name\":\"Node " + String(nodeIndex) + " Direction\",";
    payload += "\"unique_id\":\"" + objectId("direction", nodeIndex) + "\",";
    payload += "\"availability_topic\":\"" + bridgeStatusTopic + "\",";
    payload += "\"payload_available\":\"online\",";
    payload += "\"payload_not_available\":\"offline\",";
    payload += "\"command_topic\":\"" + base + "/direction/set\",";
    payload += "\"state_topic\":\"" + base + "/direction/state\",";
    payload += "\"options\":[\"exhaust\",\"supply\"],";
    payload += "\"icon\":\"mdi:swap-horizontal\"}";
    mqttPublishRetained(discoveryObjectTopic("select", "direction", nodeIndex), payload);
  }

  {
    String payload = "{" + dev;
    payload += "\"name\":\"Node " + String(nodeIndex) + " Online\",";
    payload += "\"unique_id\":\"" + objectId("online", nodeIndex) + "\",";
    payload += "\"availability_topic\":\"" + bridgeStatusTopic + "\",";
    payload += "\"payload_available\":\"online\",";
    payload += "\"payload_not_available\":\"offline\",";
    payload += "\"state_topic\":\"" + base + "/online/state\",";
    payload += "\"payload_on\":\"ON\",";
    payload += "\"payload_off\":\"OFF\",";
    payload += "\"device_class\":\"connectivity\"}";
    mqttPublishRetained(discoveryObjectTopic("binary_sensor", "online", nodeIndex), payload);
  }

  {
    String payload = "{" + dev;
    payload += "\"name\":\"Node " + String(nodeIndex) + " Sensor Present\",";
    payload += "\"unique_id\":\"" + objectId("sensor_present", nodeIndex) + "\",";
    payload += "\"availability_topic\":\"" + bridgeStatusTopic + "\",";
    payload += "\"payload_available\":\"online\",";
    payload += "\"payload_not_available\":\"offline\",";
    payload += "\"state_topic\":\"" + base + "/sensor_present/state\",";
    payload += "\"payload_on\":\"ON\",";
    payload += "\"payload_off\":\"OFF\",";
    payload += "\"icon\":\"mdi:thermometer-check\"}";
    mqttPublishRetained(discoveryObjectTopic("binary_sensor", "sensor_present", nodeIndex), payload);
  }

  {
    String payload = "{" + dev;
    payload += "\"name\":\"Node " + String(nodeIndex) + " Humidity\",";
    payload += "\"unique_id\":\"" + objectId("humidity", nodeIndex) + "\",";
    payload += "\"availability_topic\":\"" + bridgeStatusTopic + "\",";
    payload += "\"payload_available\":\"online\",";
    payload += "\"payload_not_available\":\"offline\",";
    payload += "\"state_topic\":\"" + base + "/humidity/state\",";
    payload += "\"unit_of_measurement\":\"%\",";
    payload += "\"device_class\":\"humidity\",";
    payload += "\"state_class\":\"measurement\",";
    payload += "\"icon\":\"mdi:water-percent\"}";
    mqttPublishRetained(discoveryObjectTopic("sensor", "humidity", nodeIndex), payload);
  }

  {
    String payload = "{" + dev;
    payload += "\"name\":\"Node " + String(nodeIndex) + " Temperature\",";
    payload += "\"unique_id\":\"" + objectId("temperature", nodeIndex) + "\",";
    payload += "\"availability_topic\":\"" + bridgeStatusTopic + "\",";
    payload += "\"payload_available\":\"online\",";
    payload += "\"payload_not_available\":\"offline\",";
    payload += "\"state_topic\":\"" + base + "/temperature/state\",";
    payload += "\"unit_of_measurement\":\"°C\",";
    payload += "\"device_class\":\"temperature\",";
    payload += "\"state_class\":\"measurement\",";
    payload += "\"icon\":\"mdi:thermometer\"}";
    mqttPublishRetained(discoveryObjectTopic("sensor", "temperature", nodeIndex), payload);
  }

  {
    String payload = "{" + dev;
    payload += "\"name\":\"Node " + String(nodeIndex) + " Reported Direction\",";
    payload += "\"unique_id\":\"" + objectId("reported_direction", nodeIndex) + "\",";
    payload += "\"availability_topic\":\"" + bridgeStatusTopic + "\",";
    payload += "\"payload_available\":\"online\",";
    payload += "\"payload_not_available\":\"offline\",";
    payload += "\"state_topic\":\"" + base + "/reported_direction/state\",";
    payload += "\"icon\":\"mdi:compass-outline\"}";
    mqttPublishRetained(discoveryObjectTopic("sensor", "reported_direction", nodeIndex), payload);
  }
}

void publishDiscoveryAll() {
  for (uint8_t i = 0; i < NUM_NODES; i++) publishDiscoveryForNode(i);
}

bool parseTopic(const String& topic, int& nodeIndex, String& field) {
  const String prefix = mqttRoot + "/node";
  if (!topic.startsWith(prefix) || !topic.endsWith("/set")) return false;

  const int nodeStart = prefix.length();
  const int slashAfterNode = topic.indexOf('/', nodeStart);
  if (slashAfterNode < 0) return false;

  const String idxStr = topic.substring(nodeStart, slashAfterNode);
  nodeIndex = idxStr.toInt();
  if (nodeIndex < 0 || nodeIndex >= NUM_NODES) return false;

  const int slashAfterField = topic.indexOf('/', slashAfterNode + 1);
  if (slashAfterField < 0) return false;

  field = topic.substring(slashAfterNode + 1, slashAfterField);
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr(topic);
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += static_cast<char>(payload[i]);

  Serial.print("MQTT [");
  Serial.print(topicStr);
  Serial.print("] = ");
  Serial.println(msg);

  int nodeIndex = -1;
  String field;
  if (!parseTopic(topicStr, nodeIndex, field)) return;

  FanNode& node = nodes[nodeIndex];

  if (field == "speed") {
    node.speedPercent = sanitizeSpeedPercent(msg.toInt());
  } else if (field == "direction") {
    if (msg != "exhaust" && msg != "supply") return;
    node.directionRaw = directionTextToRaw(msg);
  } else {
    return;
  }

  publishNodeState(static_cast<uint8_t>(nodeIndex));
}

void subscribeTopics() {
  for (uint8_t i = 0; i < NUM_NODES; i++) {
    const String base = topicBase(i);
    mqttClient.subscribe((base + "/speed/set").c_str());
    mqttClient.subscribe((base + "/direction/set").c_str());
  }
}

String makeClientId() {
  return "esp32-" + gatewayId;
}

bool connectMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048);

  Serial.print("Connecting MQTT... ");
  const bool ok = mqttClient.connect(
    makeClientId().c_str(),
    MQTT_USER,
    MQTT_PASSWORD,
    bridgeStatusTopic.c_str(),
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

void connectWiFiBlocking() {
  Serial.print("Connecting WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(WIFI_RETRY_MS);
    Serial.print('.');
  }

  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

void buildCommandFrame(uint8_t requestByte, uint8_t node, uint8_t direction, uint8_t speed, uint8_t* outFrame, size_t& outLen) {
  outFrame[0] = 0x55;
  outFrame[1] = 0x4D;
  outFrame[2] = 0x00;
  outFrame[3] = requestByte;   // 0x96 = command, 0x97 = command + sensor request
  outFrame[4] = 0x03;
  outFrame[5] = node;
  outFrame[6] = direction;
  outFrame[7] = speed;
  outFrame[8] = calcChecksum(outFrame, 8);
  outLen = 9;
}

void buildPollFrame(uint8_t node, uint8_t* outFrame, size_t& outLen) {
  outFrame[0] = 0x55;
  outFrame[1] = 0x4D;
  outFrame[2] = 0x00;
  outFrame[3] = 0x6E;
  outFrame[4] = 0x01;
  outFrame[5] = node;
  outFrame[6] = calcChecksum(outFrame, 6);
  outFrame[7] = 0x00;
  outLen = 8;
}

void rs485WriteFrame(const uint8_t* data, size_t len, const char* tag) {
  setRs485TransmitMode();
  rs485.write(data, len);
  rs485.flush();
  setRs485ReceiveMode();
  printFrame(tag, data, len);
}

void handleAckFrame(uint8_t nodeIndex) {
  if (nodeIndex >= NUM_NODES) return;
  FanNode& node = nodes[nodeIndex];
  node.online = true;
  node.sensorPresent = true;
  node.lastSeenMs = millis();
  publishNodeState(nodeIndex);
  Serial.print("[RX ACK] node=");
  Serial.println(nodeIndex);
}

void handleSensorFrame(uint8_t nodeIndex, uint8_t reportedDirection, uint8_t humidity, uint16_t rawTemp) {
  if (nodeIndex >= NUM_NODES) return;

  FanNode& node = nodes[nodeIndex];
  node.online = true;
  node.sensorPresent = true;
  node.hasSensorValues = true;
  node.lastHumidity = humidity;
  node.lastTemperatureC = static_cast<float>(rawTemp) / 10.0f;
  node.lastReportedDirectionRaw = (reportedDirection == DIR_SUPPLY) ? DIR_SUPPLY : DIR_EXHAUST;
  node.lastSeenMs = millis();
  node.lastSensorMs = millis();

  publishNodeState(nodeIndex);

  Serial.print("[RX SENSOR] node=");
  Serial.print(nodeIndex);
  Serial.print(" dir=");
  Serial.print(directionRawToText(node.lastReportedDirectionRaw));
  Serial.print(" hum=");
  Serial.print(humidity);
  Serial.print(" temp=");
  Serial.println(node.lastTemperatureC, 1);
}

bool tryParseAt(size_t pos) {
  if (pos >= rxLen) return false;

  if (rxLen - pos >= 8 && rxBuf[pos] == 0x55 && rxBuf[pos + 1] == 0x53 && rxBuf[pos + 2] == 0x00 && rxBuf[pos + 3] == 0x6E && rxBuf[pos + 4] == 0x02) {
    handleAckFrame(rxBuf[pos + 6]);
    memmove(rxBuf, rxBuf + pos + 8, rxLen - (pos + 8));
    rxLen -= (pos + 8);
    return true;
  }

  if (rxLen - pos >= 7 && rxBuf[pos] == 0x53 && rxBuf[pos + 1] == 0x00 && rxBuf[pos + 2] == 0x6E && rxBuf[pos + 3] == 0x02) {
    const uint8_t mappedNode = (lastPolledNode < NUM_NODES) ? lastPolledNode : 0;
    handleAckFrame(mappedNode);
    memmove(rxBuf, rxBuf + pos + 7, rxLen - (pos + 7));
    rxLen -= (pos + 7);
    return true;
  }

  if (rxLen - pos >= 11 && rxBuf[pos] == 0x55 && rxBuf[pos + 1] == 0x53 && rxBuf[pos + 2] == 0x00 && rxBuf[pos + 3] == 0x98 && rxBuf[pos + 4] == 0x05) {
    const uint16_t rawTemp = (static_cast<uint16_t>(rxBuf[pos + 8]) << 8) | rxBuf[pos + 9];
    handleSensorFrame(rxBuf[pos + 5], rxBuf[pos + 6], rxBuf[pos + 7], rawTemp);
    memmove(rxBuf, rxBuf + pos + 11, rxLen - (pos + 11));
    rxLen -= (pos + 11);
    return true;
  }

  if (rxLen - pos >= 10 && rxBuf[pos] == 0x53 && rxBuf[pos + 1] == 0x00 && rxBuf[pos + 2] == 0x98 && rxBuf[pos + 3] == 0x05) {
    const uint16_t rawTemp = (static_cast<uint16_t>(rxBuf[pos + 7]) << 8) | rxBuf[pos + 8];
    handleSensorFrame(rxBuf[pos + 4], rxBuf[pos + 5], rxBuf[pos + 6], rawTemp);
    memmove(rxBuf, rxBuf + pos + 10, rxLen - (pos + 10));
    rxLen -= (pos + 10);
    return true;
  }

  return false;
}

void processRs485Receive() {
  while (rs485.available()) {
    const int value = rs485.read();
    if (value < 0) break;
    lastRxByteMs = millis();

    if (rxLen < sizeof(rxBuf)) {
      rxBuf[rxLen++] = static_cast<uint8_t>(value);
    } else {
      memmove(rxBuf, rxBuf + 1, sizeof(rxBuf) - 1);
      rxBuf[sizeof(rxBuf) - 1] = static_cast<uint8_t>(value);
      rxLen = sizeof(rxBuf);
    }
  }

  if (rxLen > 0 && (millis() - lastRxByteMs) > RX_STALE_PARTIAL_MS && rxLen < 7) {
    rxLen = 0;
  }

  bool progress;
  do {
    progress = false;
    for (size_t i = 0; i < rxLen; i++) {
      if (tryParseAt(i)) {
        progress = true;
        break;
      }
    }
  } while (progress);
}

void sendCommandFrame(uint8_t nodeIndex, bool requestSensor) {
  uint8_t frame[9];
  size_t len = 0;
  buildCommandFrame(requestSensor ? 0x97 : 0x96, nodeIndex, nodes[nodeIndex].directionRaw, effectiveSpeedRaw(nodeIndex), frame, len);
  rs485WriteFrame(frame, len, requestSensor ? "[TX CMD+SENSOR]" : "[TX CMD]");
}

void sendPollFrame(uint8_t nodeIndex) {
  uint8_t frame[8];
  size_t len = 0;
  buildPollFrame(nodeIndex, frame, len);
  lastPolledNode = nodeIndex;
  rs485WriteFrame(frame, len, "[TX POLL]");
}

void runSensorListenWindow() {
  if (sensorListenUntilMs == 0) return;
  if (millis() >= sensorListenUntilMs) {
    sensorListenUntilMs = 0;
    return;
  }
  processRs485Receive();
}

void runCommandScheduler() {
  if (sensorListenUntilMs != 0) return;
  const uint32_t now = millis();
  if (now - lastCommandCycleMs < COMMAND_CYCLE_MS) return;

  lastCommandCycleMs = now;
  for (uint8_t i = 0; i < NUM_NODES; i++) {
    sendCommandFrame(i, false);
    processRs485Receive();
    delay(INTERFRAME_GAP_MS);
  }
}

void runPollScheduler() {
  if (sensorListenUntilMs != 0) return;
  const uint32_t now = millis();
  if (now - lastPollMs < POLL_INTERVAL_MS) return;

  lastPollMs = now;
  sendPollFrame(nextPollNode);
  nextPollNode = (nextPollNode + 1) % NUM_NODES;
}

void runSensorRequestScheduler() {
  if (sensorListenUntilMs != 0) return;
  const uint32_t now = millis();
  if (now - lastSensorRequestScheduleMs < SENSOR_REQUEST_INTERVAL_MS) return;

  lastSensorRequestScheduleMs = now;
  const uint8_t nodeIndex = nextSensorNode;
  nextSensorNode = (nextSensorNode + 1) % NUM_NODES;

  sendCommandFrame(nodeIndex, true);
  sensorListenUntilMs = millis() + SENSOR_LISTEN_WINDOW_MS;
  Serial.print("[RX WINDOW] listening ");
  Serial.print(SENSOR_LISTEN_WINDOW_MS);
  Serial.println(" ms after 0x97");
}

void refreshNodeTimeouts() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_NODES; i++) {
    FanNode& node = nodes[i];
    const bool wasOnline = node.online;
    const bool wasSensorPresent = node.sensorPresent;
    const bool hadSensorValues = node.hasSensorValues;

    if (node.hasSensorValues && (now - node.lastSensorMs) > SENSOR_STALE_MS) {
      node.hasSensorValues = false;
    }

    node.online = (now - node.lastSeenMs) <= NODE_TIMEOUT_MS;
    node.sensorPresent = sensorValueFresh(i);

    if (node.online != wasOnline || node.sensorPresent != wasSensorPresent || node.hasSensorValues != hadSensorValues) {
      publishNodeState(i);
    }
  }
}

void setupDefaultState() {
  for (uint8_t i = 0; i < NUM_NODES; i++) {
    nodes[i].speedPercent = DEFAULT_SPEED_PERCENT;
    nodes[i].directionRaw = DIR_EXHAUST;
    nodes[i].online = false;
    nodes[i].sensorPresent = false;
    nodes[i].hasSensorValues = false;
    nodes[i].lastHumidity = 0;
    nodes[i].lastTemperatureC = 0.0f;
    nodes[i].lastReportedDirectionRaw = DIR_EXHAUST;
    nodes[i].lastSeenMs = 0;
    nodes[i].lastSensorMs = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("SmartFan ESP start");

  setupDefaultState();
  initGatewayIdentity();
  Serial.print("Gateway ID: ");
  Serial.println(gatewayId);
  Serial.print("MQTT root: ");
  Serial.println(mqttRoot);

  pinMode(RS485_DIR_PIN, OUTPUT);
  setRs485ReceiveMode();
  rs485.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  connectWiFiBlocking();
  connectMQTT();
}

void loop() {
  processRs485Receive();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectWiFiBlocking();
  }

  if (!mqttClient.connected()) {
    const uint32_t now = millis();
    if (now - lastMqttRetryMs >= MQTT_RETRY_MS) {
      lastMqttRetryMs = now;
      connectMQTT();
    }
  } else {
    mqttClient.loop();
  }

  runSensorListenWindow();
  runSensorRequestScheduler();
  runCommandScheduler();
  runPollScheduler();
  processRs485Receive();
  refreshNodeTimeouts();
}
