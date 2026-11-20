#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "mqtt_client.h"
#include <time.h>
#include <DHT.h>
#include <Arduino_JSON.h>
#include "env.h"

// ============================================================
// 1. WIFI CONFIGURATION
// ============================================================

//const char *ssid     = "Your Wifi SSID";
//const char *password = "Your Wifi Password";

// ============================================================
// 2. MQTT CONFIGURATION
// ============================================================

const char *mqtt_host     = "easyfarm.duckdns.org";
const int   mqtt_port     = 8883;

//const char *mqtt_username = "Your MQTT Username";
//const char *mqtt_password = "Your MQTT password";

const char *mqtt_topic         = "esp32/command";
const char *mqtt_publish_topic = "esp32/sensor";

// ============================================================
// 3. TLS CERTIFICATE
// ============================================================

// const char *test_certificate = R"EOF(        // อย่าลืมเอาเครื่องหมาย //ข้างหน้าทุกบรรทัด เริ่มจากบรรทัดนี้ออก
//-----BEGIN CERTIFICATE-----
// put your ca.crt here  วางข้อมูล ca.crt ไว้ตรงนี้  
//-----END CERTIFICATE-----
//)EOF";                                       //  อย่าลืมเอาเครื่องหมาย //ข้างหน้าทุกบรรทัด ถึงตรงนี้ัออก

// ============================================================
// 4. NTP CONFIGURATION
// ============================================================

const char *ntpServer = "pool.ntp.org";

const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

// ============================================================
// 5. HARDWARE, DHT22 & GLOBAL VARIABLES
// ============================================================

#define LED_PIN 2
#define DHTPIN 14
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

unsigned long lastPublishTime = 0;
const unsigned long publishInterval = 5000;

esp_mqtt_client_handle_t mqtt_client = NULL;

bool mqtt_connected  = false;
bool mqtt_subscribed = false;

unsigned long mqtt_start_time = 0;
bool mqtt_timeout_reported = false;

// ============================================================
// PRINT SECTION
// ============================================================

void printSection(const char *name) {

    Serial.println();
    Serial.println("================================================");
    Serial.printf(" TEST: %s\n", name);
    Serial.println("================================================");
}

// ============================================================
// TEST 1: WIFI
// ============================================================

bool testWiFi() {

    printSection("WIFI CONNECTION");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int count = 0;

    while (WiFi.status() != WL_CONNECTED && count < 40) {

        delay(500);
        Serial.print(".");
        count++;
    }

    if (WiFi.status() != WL_CONNECTED) {

        Serial.println("\n[FAIL] WiFi Connection");

        return false;
    }

    Serial.println("\n[PASS] WiFi Connected");

    Serial.print("SSID       : ");
    Serial.println(WiFi.SSID());

    Serial.print("IP Address : ");
    Serial.println(WiFi.localIP());

    Serial.print("Gateway    : ");
    Serial.println(WiFi.gatewayIP());

    Serial.print("RSSI       : ");
    Serial.println(WiFi.RSSI());

    return true;
}

// ============================================================
// TEST 2: NTP
// ============================================================

bool testNTP() {

    printSection("NTP TIME SYNCHRONIZATION");

    configTime(
        gmtOffset_sec,
        daylightOffset_sec,
        ntpServer
    );

    struct tm timeinfo;

    Serial.print("Syncing NTP");

    for (int i = 0; i < 40; i++) {

        if (getLocalTime(&timeinfo, 500)) {

            Serial.println("\n[PASS] NTP Synchronized");

            Serial.printf(
                "ESP32 Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec
            );

            return true;
        }

        Serial.print(".");
    }

    Serial.println("\n[FAIL] NTP Synchronization");

    return false;
}

// ============================================================
// TEST 3: DNS
// ============================================================

bool testDNS() {

    printSection("DNS RESOLUTION");

    IPAddress brokerIP;

    if (WiFi.hostByName(mqtt_host, brokerIP)) {

        Serial.println("[PASS] DNS Resolution");

        Serial.print("Domain : ");
        Serial.println(mqtt_host);

        Serial.print("IP     : ");
        Serial.println(brokerIP);

        return true;
    }

    Serial.println("[FAIL] DNS Resolution");

    return false;
}

// ============================================================
// TEST 4: TCP
// ============================================================

bool testTCP() {

    printSection("TCP CONNECTION");

    WiFiClient tcpClient;

    Serial.printf(
        "Connecting %s:%d\n",
        mqtt_host,
        mqtt_port
    );

    if (tcpClient.connect(mqtt_host, mqtt_port, 10000)) {

        Serial.println("[PASS] TCP Port OPEN");

        tcpClient.stop();

        return true;
    }

    Serial.println("[FAIL] TCP Connection");

    tcpClient.stop();

    return false;
}

// ============================================================
// TEST 5: TLS HANDSHAKE
// ============================================================

bool testTLS() {

    printSection("TLS HANDSHAKE / CERTIFICATE");

    WiFiClientSecure tlsClient;

    tlsClient.setCACert(test_certificate);
    tlsClient.setHandshakeTimeout(15);

    Serial.print("TLS Hostname : ");
    Serial.println(mqtt_host);

    Serial.print("TLS Port     : ");
    Serial.println(mqtt_port);

    Serial.println("Verifying Certificate...");

    if (tlsClient.connect(mqtt_host, mqtt_port)) {

        Serial.println("[PASS] TLS Handshake");
        Serial.println("[PASS] Certificate Verification");
        Serial.println("[PASS] Hostname Verification");

        tlsClient.stop();

        return true;
    }

    Serial.println("[FAIL] TLS Handshake");
    Serial.println("TLS connection failed.");

    tlsClient.stop();

    return false;
}

// ============================================================
// HANDLE COMMAND PAYLOAD ("led": 0 หรือ 1) WITH Arduino_JSON
// ============================================================

void handleCommandMessage(const char *data, int data_len) {

    if (data == NULL || data_len <= 0 || data_len >= 512) {

        Serial.println("[WARN] Invalid command payload length");

        return;
    }

    char jsonBuf[512];
    memcpy(jsonBuf, data, data_len);
    jsonBuf[data_len] = '\0';

    JSONVar doc = JSON.parse(jsonBuf);

    if (JSON.typeof(doc) == "undefined" || !doc.hasOwnProperty("led")) {

        Serial.println("[WARN] Invalid JSON or missing 'led' property");

        return;
    }

    int ledVal = -1;
    JSONVar ledVar = doc["led"];
    String ledType = JSON.typeof(ledVar);

    if (ledType == "number") {

        ledVal = (int)ledVar;
    } else if (ledType == "string") {

        String sVal = (String)ledVar;
        sVal.trim();
        if (sVal == "1") {

            ledVal = 1;
        } else if (sVal == "0") {

            ledVal = 0;
        }
    }

    if (ledVal != 0 && ledVal != 1) {

        Serial.printf("[WARN] Invalid 'led' value in payload: %s\n", jsonBuf);

        return;
    }

    digitalWrite(LED_PIN, ledVal == 1 ? HIGH : LOW);

    Serial.printf("[LED] GPIO %d -> %s (led=%d)\n", LED_PIN, ledVal == 1 ? "HIGH" : "LOW", ledVal);
}

// ============================================================
// TEST 6: MQTT EVENT HANDLER
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

        case MQTT_EVENT_BEFORE_CONNECT:

            Serial.println("[MQTT] Before Connect");

            break;

        case MQTT_EVENT_CONNECTED:

            mqtt_connected = true;

            Serial.println();
            Serial.println("[PASS] MQTT CONNECTED");
            Serial.println("[PASS] MQTT Authentication Accepted");

            Serial.print("Subscribe Topic: ");
            Serial.println(mqtt_topic);

            esp_mqtt_client_subscribe(
                event->client,
                mqtt_topic,
                0
            );

            break;

        case MQTT_EVENT_DISCONNECTED:

            mqtt_connected = false;
            mqtt_subscribed = false;

            Serial.println("[MQTT] DISCONNECTED");

            break;

        case MQTT_EVENT_SUBSCRIBED:

            mqtt_subscribed = true;

            Serial.println("[PASS] MQTT SUBSCRIBE");

            Serial.printf(
                "Message ID: %d\n",
                event->msg_id
            );

            break;

        case MQTT_EVENT_UNSUBSCRIBED:

            Serial.println("[MQTT] UNSUBSCRIBED");

            break;

        case MQTT_EVENT_DATA:

            Serial.println();
            Serial.println("[PASS] MQTT DATA RECEIVED");

            Serial.printf(
                "Topic: %.*s\n",
                event->topic_len,
                event->topic
            );

            Serial.printf(
                "Data : %.*s\n",
                event->data_len,
                event->data
            );

            if (
                event->topic_len == (int)strlen(mqtt_topic) &&
                strncmp(event->topic, mqtt_topic, event->topic_len) == 0
            ) {

                handleCommandMessage(event->data, event->data_len);
            }

            break;

        case MQTT_EVENT_PUBLISHED:

            Serial.println("[MQTT] Message Published");

            break;

        case MQTT_EVENT_ERROR: {

            mqtt_connected = false;
            mqtt_subscribed = false;

            Serial.println();
            Serial.println("========== MQTT ERROR ==========");

            if (event->error_handle != NULL) {

                auto *err = event->error_handle;

                Serial.printf(
                    "Error Type       : %d\n",
                    err->error_type
                );

                Serial.printf(
                    "ESP TLS Error    : 0x%x\n",
                    err->esp_tls_last_esp_err
                );

                Serial.printf(
                    "TLS Stack Error  : 0x%x\n",
                    err->esp_tls_stack_err
                );

                Serial.printf(
                    "Certificate Flag : 0x%x\n",
                    err->esp_tls_cert_verify_flags
                );

                Serial.printf(
                    "Socket Error     : %d\n",
                    err->esp_transport_sock_errno
                );

                Serial.printf(
                    "MQTT Return Code : %d\n",
                    err->connect_return_code
                );
            }

            Serial.println("================================");

            if (event->client != NULL) {

                esp_mqtt_client_reconnect(event->client);
            }

            break;
        }

        default:

            Serial.printf(
                "[MQTT] Event ID: %ld\n",
                (long)event_id
            );

            break;
    }
}

// ============================================================
// TEST 7: MQTT CONNECTION
// ============================================================

void testMQTT() {

    printSection("MQTT AUTHENTICATION");

    esp_mqtt_client_config_t mqtt_cfg = {};

    mqtt_cfg.broker.address.uri =
        "mqtts://easyfarm.duckdns.org:8883";

    mqtt_cfg.broker.verification.certificate =
        test_certificate;

    mqtt_cfg.broker.verification.skip_cert_common_name_check =
        false;

    mqtt_cfg.credentials.username =
        mqtt_username;

    mqtt_cfg.credentials.authentication.password =
        mqtt_password;

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    if (mqtt_client == NULL) {

        Serial.println("[FAIL] MQTT Client Initialization");

        return;
    }

    esp_err_t register_result =
        esp_mqtt_client_register_event(
            mqtt_client,
            MQTT_EVENT_ANY,
            mqtt_event_handler,
            NULL
        );

    if (register_result != ESP_OK) {

        Serial.printf(
            "[FAIL] Event Registration: %s\n",
            esp_err_to_name(register_result)
        );

        return;
    }

    Serial.println("[PASS] MQTT Event Registration");

    Serial.println("Starting MQTT Client...");

    esp_err_t result =
        esp_mqtt_client_start(mqtt_client);

    if (result != ESP_OK) {

        Serial.printf(
            "[FAIL] MQTT Start: %s\n",
            esp_err_to_name(result)
        );

        return;
    }

    mqtt_start_time = millis();

    Serial.println("[PASS] MQTT Client Started");
    Serial.println("Waiting for Broker...");
}

// ============================================================
// PUBLISH DHT22 JSON DATA
// ============================================================

void publishSensorData() {

    if (!mqtt_connected || mqtt_client == NULL) {

        return;
    }

    float humi = dht.readHumidity();
    float temp = dht.readTemperature();

    if (isnan(humi) || isnan(temp)) {

        Serial.println("[WARN] Failed to read from DHT22 sensor");

        return;
    }

    JSONVar payloadObj;
    payloadObj["humidity"] = humi;
    payloadObj["temperature"] = temp;
    String jsonStr = JSON.stringify(payloadObj);

    int msg_id = esp_mqtt_client_publish(
        mqtt_client,
        mqtt_publish_topic,
        jsonStr.c_str(),
        0,
        1,
        0
    );

    if (msg_id >= 0) {

        Serial.printf(
            "[PUBLISH] Topic: %s | Payload: %s | msg_id: %d\n",
            mqtt_publish_topic,
            jsonStr.c_str(),
            msg_id
        );
    } else {

        Serial.println("[FAIL] MQTT Publish failed");
    }
}

// ============================================================
// SETUP
// ============================================================

void setup() {

    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    dht.begin();

    delay(1000);

    Serial.println();
    Serial.println();
    esp_log_level_set("periph_ctrl", ESP_LOG_ERROR);
    Serial.println("############################################");
    Serial.println("#       ESP32 MQTTS DIAGNOSTIC TOOL         #");
    Serial.println("#       EASYFARM MQTT BROKER                #");
    Serial.println("#       ARDUINO ESP32 CORE 3.3.12           #");
    Serial.println("############################################");

    // TEST 1: WIFI

    if (!testWiFi()) {

        Serial.println("[STOP] WiFi Failed");

        return;
    }

    // TEST 2: NTP

    if (!testNTP()) {

        Serial.println("[STOP] NTP Failed");

        return;
    }

    // TEST 3: DNS

    testDNS();

    // TEST 4: TCP

    testTCP();

    // TEST 5: TLS

    testTLS();

    // TEST 6: MQTT

    testMQTT();

    Serial.println();
    Serial.println("############################################");
    Serial.println("#       DIAGNOSTIC SEQUENCE COMPLETED      #");
    Serial.println("#       MQTT EVENTS WILL CONTINUE          #");
    Serial.println("############################################");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

    if (WiFi.status() != WL_CONNECTED) {

        Serial.println("[WARNING] WiFi Disconnected");

        delay(1000);

        return;
    }

    // MQTT CONNECTION TIMEOUT REPORT

    if (
        mqtt_start_time > 0 &&
        !mqtt_connected &&
        !mqtt_timeout_reported &&
        millis() - mqtt_start_time > 20000
    ) {

        mqtt_timeout_reported = true;

        Serial.println();
        Serial.println("[TIMEOUT] MQTT did not connect within 20 seconds");

        Serial.println("Check MQTT ERROR details above.");
    }

    if (mqtt_connected && (millis() - lastPublishTime >= publishInterval)) {

        lastPublishTime = millis();
        publishSensorData();
    }

    delay(100);
}
