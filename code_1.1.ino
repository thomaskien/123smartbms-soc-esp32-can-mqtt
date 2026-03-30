#include <WiFi.h>
#include <PubSubClient.h>
#include "driver/twai.h"

// ================== WLAN ==================
const char* WIFI_SSID     = "DEIN_WLAN";
const char* WIFI_PASSWORD = "DEIN_PASSWORT";

// ================== MQTT ==================
const char* MQTT_HOST = "10.0.83.10";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_CLIENT_ID = "esp32-123smartbms";

const char* MQTT_TOPIC_SOC            = "bms/123smartbms/soc";
const char* MQTT_TOPIC_VOLTAGE_V      = "bms/123smartbms/voltage_v";
const char* MQTT_TOPIC_CURRENT_BATT_A = "bms/123smartbms/current_battery_a";
const char* MQTT_TOPIC_CURRENT_IN_A   = "bms/123smartbms/current_in_a";
const char* MQTT_TOPIC_CURRENT_OUT_A  = "bms/123smartbms/current_out_a";
const char* MQTT_TOPIC_POWER_BATT_W   = "bms/123smartbms/power_battery_w";

// ================== CAN ==================
static const gpio_num_t CAN_TX_PIN = GPIO_NUM_15;   // TXD2
static const gpio_num_t CAN_RX_PIN = GPIO_NUM_16;   // RXD2

static const uint32_t ID_STATUS_0 = 0;  // N+0
static const uint32_t ID_STATUS_1 = 1;  // N+1
static const int SOC_BYTE_INDEX = 6;

// 123smartbms sends 250 kbit/s in this setup
static const uint32_t PUBLISH_INTERVAL_MS = 60UL * 1000UL;

// ================== STATE ==================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

bool can_ok = false;

bool have_soc = false;
uint8_t soc = 0;

bool have_status_0 = false;
float voltage_v = 0.0f;
float current_in_a = 0.0f;
float current_out_a = 0.0f;
float current_battery_a = 0.0f;
int32_t power_battery_w = 0;

uint32_t last_publish_ms = 0;

// ================== HELPERS ==================
static uint16_t be_u16(const uint8_t* d, int idx) {
  return (uint16_t(d[idx]) << 8) | uint16_t(d[idx + 1]);
}

static int16_t be_s16(const uint8_t* d, int idx) {
  return (int16_t)((uint16_t(d[idx]) << 8) | uint16_t(d[idx + 1]));
}

static void wifi_connect() {
  Serial.print("WiFi verbinden");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print('.');
    if (millis() - t0 > 20000UL) {
      Serial.println();
      Serial.println("WiFi Retry");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      t0 = millis();
    }
  }

  Serial.println();
  Serial.print("WiFi OK, IP: ");
  Serial.println(WiFi.localIP());
}

static void mqtt_connect() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  if (mqtt.connected()) return;

  Serial.print("MQTT verbinden ... ");
  if (mqtt.connect(MQTT_CLIENT_ID)) {
    Serial.println("OK");
  } else {
    Serial.print("Fehler rc=");
    Serial.println(mqtt.state());
  }
}

static void mqtt_publish_float_1(const char* topic, float value) {
  mqtt_connect();
  if (!mqtt.connected()) return;

  char payload[24];
  dtostrf(value, 0, 1, payload);
  mqtt.publish(topic, payload, true);
}

static void mqtt_publish_int(const char* topic, int32_t value) {
  mqtt_connect();
  if (!mqtt.connected()) return;

  char payload[24];
  snprintf(payload, sizeof(payload), "%ld", (long)value);
  mqtt.publish(topic, payload, true);
}

static void publish_all_values() {
  if (have_soc) {
    mqtt_publish_int(MQTT_TOPIC_SOC, soc);
  }

  if (have_status_0) {
    mqtt_publish_float_1(MQTT_TOPIC_VOLTAGE_V, voltage_v);
    mqtt_publish_float_1(MQTT_TOPIC_CURRENT_BATT_A, current_battery_a);
    mqtt_publish_float_1(MQTT_TOPIC_CURRENT_IN_A, current_in_a);
    mqtt_publish_float_1(MQTT_TOPIC_CURRENT_OUT_A, current_out_a);
    mqtt_publish_int(MQTT_TOPIC_POWER_BATT_W, power_battery_w);
  }

  Serial.print("Publish: SOC=");
  if (have_soc) Serial.print(soc);
  else Serial.print("n/a");

  Serial.print(" V=");
  if (have_status_0) Serial.print(voltage_v, 1);
  else Serial.print("n/a");

  Serial.print(" Ibat=");
  if (have_status_0) Serial.print(current_battery_a, 1);
  else Serial.print("n/a");

  Serial.print(" Iin=");
  if (have_status_0) Serial.print(current_in_a, 1);
  else Serial.print("n/a");

  Serial.print(" Iout=");
  if (have_status_0) Serial.print(current_out_a, 1);
  else Serial.print("n/a");

  Serial.print(" Pbat=");
  if (have_status_0) Serial.println(power_battery_w);
  else Serial.println("n/a");
}

static bool twai_setup() {
  twai_general_config_t g_config =
    TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);

  g_config.rx_queue_len = 100;
  g_config.tx_queue_len = 0;

  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
  Serial.print("twai_driver_install = ");
  Serial.println((int)err);
  if (err != ESP_OK) return false;

  err = twai_start();
  Serial.print("twai_start = ");
  Serial.println((int)err);
  if (err != ESP_OK) return false;

  uint32_t alerts =
      TWAI_ALERT_RX_DATA |
      TWAI_ALERT_RX_QUEUE_FULL |
      TWAI_ALERT_BUS_ERROR |
      TWAI_ALERT_ERR_PASS;

  err = twai_reconfigure_alerts(alerts, NULL);
  Serial.print("twai_reconfigure_alerts = ");
  Serial.println((int)err);
  if (err != ESP_OK) return false;

  return true;
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== 123smartbms CAN -> MQTT v1.1 ===");

  wifi_connect();
  mqtt_connect();

  can_ok = twai_setup();
  if (can_ok) {
    Serial.println("TWAI running (NORMAL, 250k)");
  } else {
    Serial.println("TWAI init failed");
  }
}

// ================== LOOP ==================
void loop() {
  mqtt.loop();

  if (!can_ok) {
    delay(1000);
    return;
  }

  uint32_t alerts_triggered = 0;
  twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(200));

  if (alerts_triggered & (TWAI_ALERT_RX_DATA | TWAI_ALERT_RX_QUEUE_FULL)) {
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
      if (msg.extd) continue;
      if (msg.rtr) continue;

      if (msg.identifier == ID_STATUS_0 && msg.data_length_code >= 8) {
        uint16_t raw_voltage = be_u16(msg.data, 0);
        int16_t raw_current_in = be_s16(msg.data, 2);
        int16_t raw_current_out = be_s16(msg.data, 4);
        int16_t raw_current_batt = be_s16(msg.data, 6);

        voltage_v = raw_voltage * 0.1f;
        current_in_a = raw_current_in * 0.1f;
        current_out_a = raw_current_out * 0.1f;
        current_battery_a = raw_current_batt * 0.1f;
        power_battery_w = (int32_t)(voltage_v * current_battery_a);

        have_status_0 = true;
      }

      if (msg.identifier == ID_STATUS_1 && msg.data_length_code > SOC_BYTE_INDEX) {
        uint8_t raw_soc = msg.data[SOC_BYTE_INDEX];
        if (raw_soc <= 100) {
          soc = raw_soc;
          have_soc = true;
        }
      }
    }
  }

  uint32_t now = millis();
  if (now - last_publish_ms >= PUBLISH_INTERVAL_MS) {
    last_publish_ms = now;
    publish_all_values();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi verloren, reconnect ...");
    wifi_connect();
  }

  delay(2);
}
