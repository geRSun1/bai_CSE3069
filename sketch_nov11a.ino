#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// --- 1. CẤU HÌNH MẠNG & MQTT ---
const char* WIFI_SSID = "Minh";        // Thay tên Wi-Fi của bạn
const char* WIFI_PASS = "20062006";       // Thay mật khẩu Wi-Fi của bạn
const char* MQTT_SERVER = "192.168.100.7";      // Thay bằng IP LAN máy tính chạy Docker (xem bằng ipconfig)
const int   MQTT_PORT = 1883;                  // Cổng MQTT TCP mặc định của Mosquitto/HiveMQ
const char* MQTT_TOPIC = "nhom12/node01/telemetry"; // Topic đúng chuẩn theo yêu cầu đề bài

// --- 2. CẤU HÌNH PHẦN CỨNG (Kế thừa Bài 2 & Bài 3) ---
#define DHTPIN 14
#define DHTTYPE DHT11
#define BUTTON_PIN 27
#define MOTOR_PWM_PIN 26

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- 3. CẤU TRÚC DỮ LIỆU & HÀNG ĐỢI FREERTOS (QUEUE) ---
struct SensorPacket {
    unsigned long timestamp;
    float temperature;
    float humidity;
    bool valid;
};

QueueHandle_t telemetryQueue;
const int QUEUE_SIZE = 30; // Đã tăng kích thước Queue lên 30 bản ghi đệm khi mất mạng

// Biến trạng thái FSM An toàn (Bài 3)
enum SystemState { STATE_DUNG, STATE_CHAY, STATE_LOI };
volatile SystemState currentState = STATE_DUNG;
volatile bool emergencyStopFlag = false;
volatile unsigned long lastButtonPress = 0;

// Ngắt ngoài nút bấm dừng khẩn cấp (ISR)
void IRAM_ATTR onEmergencyButton() {
    unsigned long now = millis();
    if (now - lastButtonPress > 250) {
        emergencyStopFlag = true;
        analogWrite(MOTOR_PWM_PIN, 0); // Ngắt tải tức thì
        lastButtonPress = now;
    }
}

// =========================================================================
// TASK 1: ĐỌC CẢM BIẾN (Chu kỳ 2000ms) -> Đẩy vào Queue
// =========================================================================
void TaskSensorRead(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(2000);

    for (;;) {
        SensorPacket pkt;
        pkt.timestamp = millis();
        pkt.temperature = dht.readTemperature();
        pkt.humidity = dht.readHumidity();

        // Kiểm tra lỗi cảm biến (NaN) theo chuẩn Bài 2
        if (isnan(pkt.temperature) || isnan(pkt.humidity)) {
            pkt.valid = false;
        } else {
            pkt.valid = true;
        }

        // Chính sách khi Queue đầy: BỎ BẢN GHI CŨ NHẤT (Drop Oldest), ghi bản mới nhất
        if (uxQueueSpacesAvailable(telemetryQueue) == 0) {
            SensorPacket droppedPkt;
            xQueueReceive(telemetryQueue, &droppedPkt, 0); 
            Serial.printf("[QUEUE-FULL] [t=%lums] Queue day (%d/%d)! Da bo ban ghi cu (ts=%lu) de ghi ban moi.\n", 
                          millis(), QUEUE_SIZE, QUEUE_SIZE, droppedPkt.timestamp);
        }

        // Đẩy gói tin vào Queue (không chặn)
        xQueueSend(telemetryQueue, &pkt, 0);

        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

// =========================================================================
// TASK 2: GỬI MQTT (Lấy từ Queue -> Publish lên HiveMQ Broker)
// =========================================================================
void TaskMQTTPublish(void *pvParameters) {
    SensorPacket rxPkt;
    char payload[160];

    for (;;) {
        // Tự động kết nối lại Wi-Fi nếu bị rớt
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Tự động kết nối lại MQTT Broker nếu mất kết nối
        if (!mqttClient.connected()) {
            Serial.printf("[MQTT] [t=%lums] Disconnected! Reconnecting to HiveMQ broker...\n", millis());
            if (mqttClient.connect("ESP32_Node01_Nhom12")) {
                Serial.printf("[MQTT] [t=%lums] Reconnected successfully!\n", millis());
            } else {
                Serial.printf("[MQTT] [t=%lums] Connect failed (rc=%d). Retrying in 2s...\n", millis(), mqttClient.state());
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }
        mqttClient.loop();

        // Lấy dữ liệu từ Queue (chờ tối đa 500ms)
        if (xQueueReceive(telemetryQueue, &rxPkt, pdMS_TO_TICKS(500)) == pdPASS) {
            // Đóng gói JSON đúng mẫu yêu cầu: phân biệt null khi lỗi cảm biến
            if (rxPkt.valid) {
                snprintf(payload, sizeof(payload),
                         "{\"device_id\":\"node01\",\"ts\":%lu,\"temp\":%.1f,\"unit\":\"C\",\"valid\":true}",
                         rxPkt.timestamp, rxPkt.temperature);
            } else {
                snprintf(payload, sizeof(payload),
                         "{\"device_id\":\"node01\",\"ts\":%lu,\"temp\":null,\"unit\":\"C\",\"valid\":false}",
                         rxPkt.timestamp);
            }

            if (mqttClient.publish(MQTT_TOPIC, payload)) {
                Serial.printf("[MQTT-PUB] [t=%lums] Sent: %s (Queue left: %u)\n", 
                              millis(), payload, uxQueueMessagesWaiting(telemetryQueue));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// =========================================================================
// TASK 3: GIỮ AN TOÀN (Safety FSM - Chu kỳ 100ms, Ưu tiên cao nhất)
// =========================================================================
void TaskSafetyControl(void *pvParameters) {
    unsigned long lastHeartbeat = 0;

    for (;;) {
        // Xử lý sự kiện nút bấm khẩn cấp ngay lập tức không phụ thuộc mạng
        if (emergencyStopFlag) {
            currentState = STATE_DUNG;
            analogWrite(MOTOR_PWM_PIN, 0);
            emergencyStopFlag = false;
            Serial.printf("[SAFETY-FSM] [t=%lums] Emergency Button -> State: DUNG (PWM=0%%)\n", millis());
        }

        // In nhịp tim (Heartbeat) chứng minh Task an toàn không bị chặn khi mất mạng
        if (millis() - lastHeartbeat >= 2000) {
            lastHeartbeat = millis();
            const char* stateStr = (currentState == STATE_DUNG) ? "DUNG" : (currentState == STATE_CHAY ? "CHAY" : "LOI");
            Serial.printf("[SAFETY-HEARTBEAT] [t=%lums] FSM_State: %s | Safety loop running on schedule.\n", 
                          millis(), stateStr);
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // Chu kỳ cố định 100ms
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(MOTOR_PWM_PIN, OUTPUT);
    analogWrite(MOTOR_PWM_PIN, 0);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onEmergencyButton, FALLING);

    dht.begin();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

    // Khởi tạo Queue với kích thước mới (30 phần tử)
    telemetryQueue = xQueueCreate(QUEUE_SIZE, sizeof(SensorPacket));

    // Tạo các Task FreeRTOS (Ghim Task an toàn mức ưu tiên cao nhất = 3)
    xTaskCreatePinnedToCore(TaskSafetyControl, "SafetyTask", 2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(TaskSensorRead,    "SensorTask", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(TaskMQTTPublish,   "MQTTTask",   4096, NULL, 1, NULL, 0);

    Serial.println("[INIT] FreeRTOS Tasks & Queue initialized with size 30. Ready for HiveMQ.");
}

void loop() {
    // Không dùng loop() khi chạy hệ điều hành thời gian thực FreeRTOS
    vTaskDelete(NULL);
}