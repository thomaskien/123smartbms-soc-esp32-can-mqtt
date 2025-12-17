#include <WiFi.h>
//add via library manager of arduino-ide, search for "PubSubClient"
#include <PubSubClient.h>
#include "driver/twai.h"

// ======== FEST: ab jetzt immer 250 kbit/s ========
static const uint32_t CAN_BITRATE_KBPS = 250;

// Waveshare / dein Setup (wie bisher besprochen)
static const gpio_num_t CAN_TX_PIN = GPIO_NUM_15;
static const gpio_num_t CAN_RX_PIN = GPIO_NUM_16;

// 123BMS: ID 1, Byte 6 = SOC (wie dein Log zeigt)
static const uint32_t SOC_CAN_ID = 1;
static const int SOC_BYTE_INDEX = 6;

// MQTT
static const char* WIFI_SSID     = "DEIN_WLAN";
static const char* WIFI_PASSWORD = "DEIN_PASSWORT";
static const char* MQTT_HOST     = "10.0.83.10";
static const uint16_t MQTT_PORT  = 1883;
static const char* MQTT_CLIENT_ID = "esp32s3-123bms";
static const char* MQTT_TOPIC_SOC = "bms/123/soc";

// 1x pro Minute
static const uint32_t PUBLISH_INTERVAL_MS = 60UL * 1000UL;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

static bool twai_ok = false;
static uint8_t last_soc = 255;
static uint32_t soc_hits = 0;
static uint32_t frames_total = 0;

static uint32_t last_pub_ms = 0;
static uint32_t last_stat_ms = 0;

static void wifi_connect() {
  Serial.print("WiFi: connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - t0 > 20000) { // Retry
      Serial.println("\nWiFi: retry");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      t0 = millis();
    }
  }
  Serial.print("\nWiFi: OK IP=");
  Serial.println(WiFi.localIP());
}

static void mqtt_connect() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  if (mqtt.connected()) return;

  Serial.print("MQTT: connecting...");
  if (mqtt.connect(MQTT_CLIENT_ID)) {
    Serial.println("OK");
  } else {
    Serial.print("FAIL rc=");
    Serial.println(mqtt.state());
  }
}

static void mqtt_publish_soc(uint8_t soc) {
  mqtt_connect();
  if (!mqtt.connected()) return;

  char payload[4];
  snprintf(payload, sizeof(payload), "%u", soc);
  mqtt.publish(MQTT_TOPIC_SOC, payload, true);

  Serial.print("MQTT: ");
  Serial.print(MQTT_TOPIC_SOC);
  Serial.print("=");
  Serial.println(payload);
}

static bool twai_setup_250k_normal_with_alerts() {
  // Wie in deinem Referenzcode: NORMAL + ACCEPT_ALL + Alerts :contentReference[oaicite:5]{index=5}
  twai_general_config_t g_config =
      TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  g_config.rx_queue_len = 100;
  g_config.tx_queue_len = 0;

  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
  Serial.print("TWAI install=");
  Serial.println((int)err);
  if (err != ESP_OK) return false;

  err = twai_start();
  Serial.print("TWAI start=");
  Serial.println((int)err);
  if (err != ESP_OK) return false;

  // Alerts wie im Referenzcode (RX_DATA, BUS_ERROR, ERR_PASS, RX_QUEUE_FULL, …) :contentReference[oaicite:6]{index=6}
  uint32_t alerts = TWAI_ALERT_RX_DATA |
                    TWAI_ALERT_ERR_PASS |
                    TWAI_ALERT_BUS_ERROR |
                    TWAI_ALERT_RX_QUEUE_FULL;
  err = twai_reconfigure_alerts(alerts, NULL);
  Serial.print("TWAI alerts=");
  Serial.println((int)err);
  if (err != ESP_OK) return false;

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== 123BMS SOC -> MQTT (250k, NORMAL, Alerts) ===");

  wifi_connect();
  mqtt_connect();

  twai_ok = twai_setup_250k_normal_with_alerts();
  Serial.println(twai_ok ? "TWAI: OK" : "TWAI: FAIL");
}

void loop() {
  mqtt.loop();

  if (!twai_ok) {
    // Wenn TWAI nicht startet, wenigstens sichtbar bleiben
    static uint32_t last = 0;
    if (millis() - last > 1000) {
      last = millis();
      Serial.println("TWAI not running");
    }
    delay(10);
    return;
  }

  // Alert-gesteuertes Empfangen wie im Referenzcode: erst read_alerts, dann receive-loop :contentReference[oaicite:7]{index=7}
  uint32_t alerts_triggered = 0;
  twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(200)); // 200ms Block, aber nicht “ewig”

  if (alerts_triggered & TWAI_ALERT_RX_DATA) {
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
      frames_total++;

      // Standard-Frames (dein Log: Standard Format)
      if (msg.extd) continue;

      if (msg.identifier == SOC_CAN_ID && msg.data_length_code > SOC_BYTE_INDEX) {
        uint8_t soc = msg.data[SOC_BYTE_INDEX];
        if (soc <= 100) {
          last_soc = soc;
          soc_hits++;
        }
      }
    }
  }

  // Sehr sparsame Statusausgabe (alle 5s)
  uint32_t now = millis();
  if (now - last_stat_ms >= 5000) {
    last_stat_ms = now;
    Serial.print("frames=");
    Serial.print(frames_total);
    Serial.print(" soc_hits=");
    Serial.print(soc_hits);
    Serial.print(" last_soc=");
    if (last_soc <= 100) Serial.println(last_soc);
    else Serial.println("n/a");
  }

  // MQTT: 1x/Minute, sobald SOC bekannt
  if (last_soc <= 100 && (now - last_pub_ms >= PUBLISH_INTERVAL_MS)) {
    last_pub_ms = now;
    mqtt_publish_soc(last_soc);
  }

  // WLAN reconnect (falls nötig)
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost -> reconnect");
    wifi_connect();
  }

  delay(2);
}
