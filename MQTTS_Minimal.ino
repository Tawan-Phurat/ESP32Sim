
#include <WiFi.h>
#include "mqtt_client.h"
#include <DHT.h>
#include <Arduino_JSON.h>
#include "env.h"

// ============================================================
// CONFIGURATION
// ============================================================

#define LED_PIN 2
#define DHTPIN 14
#define DHTTYPE DHT22

#define MQTT_PORT 8883

const char *mqtt_sub_topic = "esp32/command";
const char *mqtt_pub_topic = "esp32/sensor";

const unsigned long publishInterval = 5000;

DHT dht(DHTPIN, DHTTYPE);

esp_mqtt_client_handle_t mqtt_client = NULL;

bool mqtt_connected = false;
unsigned long lastPublishTime = 0;

// ============================================================
// MQTT EVENT HANDLER
// ============================================================

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data
) {
    esp_mqtt_event_handle_t event =
        (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

        case MQTT_EVENT_CONNECTED:

            mqtt_connected = true;

            Serial.println("[MQTT] Connected");

            esp_mqtt_client_subscribe(
                event->client,
                mqtt_sub_topic,
                0
            );

            break;

        case MQTT_EVENT_DISCONNECTED:

            mqtt_connected = false;

            Serial.println("[MQTT] Disconnected");

            break;

        case MQTT_EVENT_SUBSCRIBED:

            Serial.println("[MQTT] Subscribed");

            break;

        case MQTT_EVENT_DATA: {

            if (
                event->topic_len != strlen(mqtt_sub_topic) ||
                strncmp(
                    event->topic,
                    mqtt_sub_topic,
                    event->topic_len
                ) != 0
            ) {
                break;
            }

            char payload[128];

            if (event->data_len <= 0 ||
                event->data_len >= sizeof(payload)) {
                break;
            }

            memcpy(payload, event->data, event->data_len);
            payload[event->data_len] = '\0';

            JSONVar json = JSON.parse(payload);

            if (
                JSON.typeof(json) != "undefined" &&
                json.hasOwnProperty("led")
            ) {
                int led = -1;
                JSONVar value = json["led"];

                if (JSON.typeof(value) == "number") {
                    led = (int)value;
                }
                else if (JSON.typeof(value) == "string") {
                    String s = (String)value;
                    s.trim();

                    if (s == "1") led = 1;
                    if (s == "0") led = 0;
                }

                if (led == 0 || led == 1) {
                    digitalWrite(LED_PIN, led);

                    Serial.printf(
                        "[LED] GPIO%d = %d\n",
                        LED_PIN,
                        led
                    );
                }
            }

            break;
        }

        case MQTT_EVENT_ERROR:

            Serial.println("[MQTT] Error");

            if (event->error_handle != NULL) {
                Serial.printf(
                    "TLS Error: 0x%x\n",
                    event->error_handle->esp_tls_last_esp_err
                );

                Serial.printf(
                    "Certificate Flag: 0x%x\n",
                    event->error_handle->esp_tls_cert_verify_flags
                );

                Serial.printf(
                    "Socket Error: %d\n",
                    event->error_handle->esp_transport_sock_errno
                );
            }

            break;

        default:
            break;
    }
}

// ============================================================
// MQTT INITIALIZATION
// ============================================================

void startMQTT() {

    static char mqtt_uri[192];

    snprintf(
        mqtt_uri,
        sizeof(mqtt_uri),
        "mqtts://%s:%d",
        mqtt_host,
        MQTT_PORT
    );

    esp_mqtt_client_config_t config = {};

    config.broker.address.uri = mqtt_uri;

    config.broker.verification.certificate =
        test_certificate;

    config.broker.verification.skip_cert_common_name_check =
        false;

    config.credentials.username = mqtt_username;
    config.credentials.authentication.password = mqtt_password;

    mqtt_client = esp_mqtt_client_init(&config);

    if (mqtt_client == NULL) {
        Serial.println("[MQTT] Initialization failed");
        return;
    }

    esp_mqtt_client_register_event(
        mqtt_client,
        MQTT_EVENT_ANY,
        mqtt_event_handler,
        NULL
    );

    esp_mqtt_client_start(mqtt_client);
}

// ============================================================
// PUBLISH DHT22 DATA
// ============================================================

void publishSensorData() {

    if (!mqtt_connected) return;

    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();

    if (isnan(humidity) || isnan(temperature)) {
        Serial.println("[DHT] Read failed");
        return;
    }

    JSONVar data;

    data["humidity"] = humidity;
    data["temperature"] = temperature;

    String payload = JSON.stringify(data);

    int msg_id = esp_mqtt_client_publish(
        mqtt_client,
        mqtt_pub_topic,
        payload.c_str(),
        0,
        1,
        0
    );

    if (msg_id >= 0) {
        Serial.printf(
            "[PUBLISH] %s: %s\n",
            mqtt_pub_topic,
            payload.c_str()
        );
    }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
    esp_log_level_set("periph_ctrl", ESP_LOG_ERROR);
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    dht.begin();

    // WiFi

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    Serial.print("[WiFi] Connecting");

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\n[WiFi] Connected");
    Serial.println(WiFi.localIP());

    // NTP is needed for certificate time validation.

    configTime(
        7 * 3600,
        0,
        "pool.ntp.org"
    );

    struct tm timeinfo;

    Serial.print("[NTP] Synchronizing");

    while (!getLocalTime(&timeinfo, 1000)) {
        Serial.print(".");
    }

    Serial.println("\n[NTP] Synchronized");

    // MQTT

    startMQTT();
}

// ============================================================
// LOOP
// ============================================================

void loop() {

    if (mqtt_connected &&
        millis() - lastPublishTime >= publishInterval) {

        lastPublishTime = millis();

        publishSensorData();
    }

    delay(50);
}
