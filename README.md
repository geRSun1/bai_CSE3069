## File trong repo này
- `docker-compose.yml` — Cấu hình dựng HiveMQ MQTT Broker bằng Docker Compose (đã tách khóa bí mật sang `.env` và đưa vào `.gitignore`)
- `firmware/` — Mã nguồn FreeRTOS cho ESP32 tách 3 Task độc lập và Queue giới hạn 30 bản ghi (`sketch_nov11a.ino`)
- `topic_payload.md` — Tài liệu mô tả quy ước đặt tên Topic và cấu trúc bản tin JSON Payload
- `task_queue.png` — Sơ đồ kiến trúc triển khai hệ thống, phân bổ Dual-Core, luồng Task và Queue trên ESP32
- `log_10min.txt` — Log nhận dữ liệu MQTT liên tục trong 10 phút từ thiết bị ESP32 thật
- `log_disconnect.txt` — Log thực nghiệm ngắt kết nối Broker có chủ đích và kiểm chứng hàng đợi Queue + vòng điều khiển an toàn

---

## 4.1 Payload thực tế
Dưới đây là bản tin JSON thực tế thu thập được từ cảm biến DHT11 trên node ESP32 (`node01`) gửi lên HiveMQ Broker qua topic `nhom12/node01/telemetry`:

- **Bản tin khi cảm biến đo hợp lệ (`valid: true`):**

<img width="551" height="15" alt="image" src="https://github.com/user-attachments/assets/86c2888a-c7f7-4d4a-a2b9-f2ee09637a33" />


- **Bản tin khi cảm biến bị ngắt kết nối hoặc đọc lỗi `NaN` (`valid: false`):**


<img width="520" height="35" alt="image" src="https://github.com/user-attachments/assets/fde757e6-ae9b-40c6-80b1-b001c86b0b46" />


**Giải thích chi tiết từng trường trong Payload:**
| Trường dữ liệu | Kiểu dữ liệu | Ý nghĩa kỹ thuật |
| :--- | :--- | :--- |
| **`device_id`** | `String` | Định danh duy nhất của node cảm biến trong mô hình nhà kính (`"node01"`), giúp Broker và hệ thống giám sát phân biệt dữ liệu khi có nhiều node cùng hoạt động. |
| **`ts`** | `Number` | Dấu thời gian (timestamp) tính bằng mili-giây (`millis()`) kể từ lúc ESP32 khởi động, dùng để đối chiếu chu kỳ lấy mẫu $2000\text{ ms}$ và kiểm tra thứ tự gói tin. |
| **`temp`** | `Number / null` | Giá trị nhiệt độ thực tế đọc từ cảm biến DHT11 (`GPIO 14`). Khi phần cứng cảm biến gặp sự cố (hàm đọc trả về `NaN`), trường này bắt buộc gán bằng **`null`** thay vì `0` để phân biệt rõ ràng giữa lỗi đứt dây cảm biến với mức nhiệt độ $0^\circ\text{C}$ thực tế ngoài môi trường. |
| **`unit`** | `String` | Đơn vị đo của đại lượng nhiệt độ (`"C"` — độ Celsius). |
| **`valid`** | `Boolean` | Cờ báo chất lượng dữ liệu (kế thừa từ Bài 2): nhận giá trị `true` nếu số đo hợp lệ, và `false` nếu cảm biến lỗi/timeout. Các tác vụ điều khiển tự động chỉ sử dụng số đo khi `valid: true`. |

---

## 4.2 Task và queue
![Sơ đồ Task và Queue FreeRTOS trên ESP32](task_queue.png)

1. **Phân công nhiệm vụ các Task và nơi triển khai:**
   - **`TaskSafetyControl` (Ghim trên `Core 1`, Mức ưu tiên cao nhất = `3`, Chu kỳ `100 ms`):** Chuyên trách vận hành máy trạng thái an toàn (FSM kế thừa Bài 3: `DỪNG` / `CHẠY` / `LỖI`), lắng nghe cờ ngắt ngoài (ISR) từ nút nhấn dừng khẩn cấp (`GPIO 27`) và điều khiển trực tiếp xung PWM động cơ/máy bơm (`GPIO 26`).
   - **`TaskSensorRead` (Ghim trên `Core 1`, Mức ưu tiên = `2`, Chu kỳ `2000 ms`):** Đọc nhiệt độ/độ ẩm từ cảm biến DHT11 (`GPIO 14`) đúng chu kỳ cố định $2000\text{ ms}$ bằng `vTaskDelayUntil()`, kiểm tra `isnan()`, đóng gói vào cấu trúc `SensorPacket` và đẩy vào hàng đợi `telemetryQueue`.
   - **`TaskMQTTPublish` (Ghim trên `Core 0`, Mức ưu tiên = `1`):** Chuyên trách duy trì kết nối Wi-Fi và MQTT TCP tới **HiveMQ Broker** (triển khai trên PC qua Docker Compose tại địa chỉ `192.168.100.7:1883`), lấy từng gói tin từ `telemetryQueue`, đóng gói thành chuỗi JSON và publish lên topic `nhom12/node01/telemetry`.

2. **Cơ chế giao tiếp qua Queue và Chính sách khi Queue đầy:**
   - **Kích thước giới hạn:** Hàng đợi `telemetryQueue` được khởi tạo bằng `xQueueCreate(30, sizeof(SensorPacket))` với sức chứa tối đa **30 bản ghi** (tương đương 60 giây lưu đệm dữ liệu khi mất kết nối tới Broker).
   - **Chính sách khi đầy (`Drop Oldest` — Bỏ bản ghi cũ nhất):** Trước khi đẩy dữ liệu mới vào Queue, `TaskSensorRead` kiểm tra số ô trống bằng `uxQueueSpacesAvailable(telemetryQueue)`. Nếu hàng đợi đã đầy (`30/30`), Task sẽ gọi `xQueueReceive(telemetryQueue, &droppedPkt, 0)` để rút bỏ 1 bản ghi cũ nhất ở đầu hàng đợi ra, in cảnh báo `[QUEUE-FULL]` qua Serial, rồi mới đẩy bản ghi mới nhất vào cuối hàng đợi (`xQueueSend(..., 0)`). Chính sách này giúp `TaskSensorRead` không bao giờ bị chặn (non-blocking) và đảm bảo khi mạng có lại, Broker nhận được 30 mẫu đo gần với thời điểm hiện tại nhất.

3. **Vì sao thời gian chờ của Task an toàn không được phụ thuộc vô hạn vào MQTT?**
   - Giao thức MQTT hoạt động qua mạng Wi-Fi và TCP/IP nên luôn tiềm ẩn độ trễ không xác định (non-deterministic): khi sập Broker hoặc nhiễu sóng Wi-Fi, hàm `mqttClient.connect()` có thể chặn (block) luồng thực thi từ vài giây đến hàng chục giây.
   - Nếu gộp chung vòng kiểm tra an toàn vào cùng một `loop()` với MQTT hoặc để `TaskSafetyControl` chờ kết nối mạng, toàn bộ hệ thống bảo vệ sẽ bị "đóng băng" khi đứt mạng: người vận hành nhấn nút dừng khẩn cấp hoặc xảy ra lỗi quá thời gian (Timeout) nhưng cơ cấu chấp hành vẫn không thể ngắt PWM, dẫn đến nguy cơ tràn nước hoặc hư hại phần cứng nhà kính. Nhờ tách riêng `TaskSafetyControl` lên `Core 1` với mức ưu tiên cao nhất (`Priority 3`) và không phụ thuộc bất kỳ lệnh mạng nào, vòng điều khiển an toàn luôn phản hồi tức thời ($\le 100\text{ ms}$) trong mọi sự cố mạng.

---

## 4.3 Thí nghiệm ngắt kết nối
Thực hiện thí nghiệm kiểm chứng khả năng chịu lỗi bằng cách dừng chủ động container HiveMQ trên PC (`docker compose stop broker`) và làm nghẽn đường truyền để quan sát hành vi của hệ thống:

- **Thời điểm ngắt kết nối Broker:**
  - Khi dừng container HiveMQ Broker, `TaskMQTTPublish` trên Core 0 lập tức phát hiện mất kết nối và chuyển sang chu trình thử kết nối lại mỗi 2 giây:

    <img width="672" height="51" alt="image" src="https://github.com/user-attachments/assets/4a6ba13f-a534-4975-b935-e04e09a1ff69" />

- **Bằng chứng tác vụ an toàn và hàng đợi vẫn hoạt động chuẩn xác trong lúc mất kết nối:**
  - Trong suốt thời gian `TaskMQTTPublish` bị lỗi kết nối (`rc=-2`), `TaskSafetyControl` trên Core 1 vẫn duy trì nhịp tim (`[SAFETY-HEARTBEAT]`) đều đặn đúng chu kỳ mỗi $2000\text{ ms}$ (tốc độ quét nội bộ $100\text{ ms}$) và sẵn sàng đáp ứng ngắt nút bấm khẩn cấp ngay lập tức:


    <img width="687" height="70" alt="image" src="https://github.com/user-attachments/assets/52d47dfc-02f2-4333-93da-84688863df5c" />

  - Đồng thời, `TaskSensorRead` vẫn tiếp tục lấy mẫu mỗi 2 giây và đẩy vào `telemetryQueue`. Khi hàng đợi đạt ngưỡng giới hạn, cơ chế `Drop Oldest` tự động kích hoạt để loại bỏ mẫu cũ và giữ lại mẫu mới nhất mà không làm treo hệ thống:
    ```text
    [QUEUE-FULL] [t=106674ms] Queue day (30/30)! Da bo ban ghi cu (ts=46649) de ghi ban moi.
    [QUEUE-FULL] [t=108674ms] Queue day (30/30)! Da bo ban ghi cu (ts=48649) de ghi ban moi.
    ```
- **Kết quả phục hồi sau khi bật lại Broker (`docker compose start broker`):**
  - Ngay khi HiveMQ Broker hoạt động trở lại, ESP32 tự động thiết lập lại phiên kết nối thành công (`[MQTT] Reconnected successfully!`).
 
    <img width="593" height="90" alt="ket_noi_thanh_cong" src="https://github.com/user-attachments/assets/3d4396eb-253b-48a5-abbc-2b7b94953645" />

 
  - Toàn bộ các bản ghi đang lưu đệm trong `telemetryQueue` được xả (flush) lần lượt lên topic `nhom12/node01/telemetry` cho tới khi `Queue left: 0`, sau đó hệ thống quay về nhịp gửi ổn định $2000\text{ ms}$/bản tin mà không cần reset mạch.


    có vid **demo_ngat_broker.mp4**
--- 
