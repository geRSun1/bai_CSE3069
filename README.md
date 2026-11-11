## File trong repo này
- `docker-compose.yml` — Cấu hình dựng HiveMQ MQTT Broker bằng Docker Compose (đã tách khóa bí mật sang `.env` và đưa vào `.gitignore`)
- `sketch_nov11a.ino` — Mã nguồn FreeRTOS cho ESP32 tách 3 Task độc lập và Queue giới hạn 30 bản ghi (`sketch_nov11a.ino`)a.onp
- `task_queue.png` — Sơ đồ kiến trúc triển khai hệ thống, phân bổ Dual-Core, luồng Task và Queue trên ESP32
- `mqtt_telemetry_log.json` — Log nhận dữ liệu MQTT liên tục trong 10 phút từ thiết bị ESP32 thật
---

## 4.1 Payload thực tế
Dưới đây là bản tin JSON thực tế thu thập được từ cảm biến DHT11 trên node ESP32 (`node01`) gửi lên HiveMQ Broker qua topic `nhom12/node01/telemetry`:

- **Bản tin khi cảm biến đo hợp lệ (`valid: true`):**

<img width="551" height="15" alt="image" src="https://github.com/user-attachments/assets/86c2888a-c7f7-4d4a-a2b9-f2ee09637a33" />


- **Bản tin khi cảm biến bị ngắt kết nối hoặc đọc lỗi `NaN` (`valid: false`):**


<img width="805" height="12" alt="image" src="https://github.com/user-attachments/assets/67fd0540-766a-4cb7-b7ef-f0613513b019" />


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
<img width="1080" height="1920" alt="image" src="https://github.com/user-attachments/assets/4daf01b4-96aa-43db-b296-b4d9b36a6d15" />


## 1. Phân bổ tác vụ trên 2 nhân CPU của ESP32 (Dual-Core Architecture)
ESP32 sở hữu 2 nhân xử lý (`Core 0` và `Core 1`). Hệ thống tận dụng hàm `xTaskCreatePinnedToCore()` để ghim các tác vụ độc lập vào từng nhân nhằm triệt tiêu hiện tượng xung đột tài nguyên và loại bỏ độ trễ do mạng:

* **Trên `Core 1` (Nhân xử lý ứng dụng & thời gian thực):**
  * **`TaskSafetyControl` (Mức ưu tiên = 3, Chu kỳ $100\text{ ms}$):**
    * Có mức ưu tiên cao nhất trong toàn bộ hệ thống (`Priority 3`).
   
      
  * **`TaskSensorRead` (Mức ưu tiên = 2, Chu kỳ $2000\text{ ms}$):**
    * Tiến hành đọc số đo nhiệt độ và độ ẩm từ cảm biến **DHT11 (`GPIO 14`)** đúng định kỳ mỗi 2 giây bằng hàm `vTaskDelayUntil()`.
    * Thực hiện kiểm tra tính hợp lệ của dữ liệu (phát hiện lỗi `NaN` để gán cờ `valid: true/false`), đóng gói thành cấu trúc `SensorPacket` rồi đẩy xuống hàng đợi `telemetryQueue`.

* **Trên `Core 0` (Nhân xử lý giao thức mạng Wi-Fi & MQTT):**
  * **`TaskMQTTPublish` (Mức ưu tiên = 1):**
    * Có mức ưu tiên thấp nhất (`Priority 1`) và được ghim sang `Core 0` (nhân vốn phụ trách ngăn xếp Wi-Fi/TCP ngầm của ESP32).
    * Đảm nhận nhiệm vụ duy trì liên kết mạng Wi-Fi/MQTT, lấy các gói dữ liệu từ `telemetryQueue`, định dạng thành chuỗi JSON và truyền (publish) qua giao thức TCP lên Broker. Mọi độ trễ kết nối hoặc thời gian chờ kết nối lại (`reconnect`) chỉ ảnh hưởng đến Task này trên `Core 0`, hoàn toàn không gây tác động hay chặn đứng `Core 1`.

---

## 2. Cơ chế đệm dữ liệu qua Hàng đợi `telemetryQueue`
Giữa `TaskSensorRead` (bên sản xuất dữ liệu - Producer) và `TaskMQTTPublish` (bên tiêu thụ dữ liệu - Consumer) không thực hiện truyền biến trực tiếp mà trao đổi thông qua hàng đợi FreeRTOS có giới hạn nghiêm ngặt:

* **Kích thước cố định (`Size: 30`):** Sức chứa tối đa lên đến 30 bản ghi `SensorPacket` (tương đương 60 giây lưu đệm dữ liệu trong trường hợp mất kết nối Broker).
* **Chính sách khi đầy (`Drop Oldest Policy` — Bỏ bản ghi cũ nhất):**
  * Khi đường truyền bị ngắt hoặc Broker sập quá 60 giây, hàng đợi sẽ đạt ngưỡng tối đa (`30/30`).
  * Lúc này, trước khi chèn mẫu đo mới vào, `TaskSensorRead` sẽ tự động rút bỏ 1 bản ghi cũ nhất ở đầu hàng đợi ra bằng hàm `xQueueReceive()`, sau đó mới chèn bản ghi mới nhất vào cuối hàng đợi bằng `xQueueSend()` với thời gian chờ bằng `0` (không gây block).
  * Cơ chế này giúp `TaskSensorRead` không bao giờ bị treo khi Queue đầy. Ngay khi mạng phục hồi, hệ thống sẽ xả đệm và gửi ngay các mẫu đo bám sát thời điểm hiện tại nhất.

---

## 3. Khối Máy chủ Broker & Giám sát trên PCf
* **HiveMQ Broker (`192.168.100.7:1883`):**
  * Được dựng và vận hành trên máy tính cá nhân thông qua **Docker Compose**.
  * Mở cổng TCP `1883` trong mạng LAN để tiếp nhận các bản tin JSON từ ESP32 gửi tới theo chủ đề `nhom12/node01/telemetry`.
* **Subscriber / Web Client (`Port 8080`):**
  * Giao diện điều khiển, Web Client của HiveMQ hoặc các tiến trình nhận tin độc lập (`mosquitto_sub` / Python Subscriber trên máy tính thứ 2) tiến hành đăng ký (`subscribe`) đúng topic `nhom12/node01/telemetry`.
  * Nhận dữ liệu liên tục theo thời gian thực, phục vụ cho việc giám sát trực quan và ghi nhận log hệ thống trong 10 phút (`log_10min.txt`).

---

## 4. Khối máy Subcribe 
* Khối máy nhận JSON bằng cách mở một công cụ/phần mềm ở trạng thái sẵn sàng Subscriber đúng cái Topic mà ESP32 đang truyền đi. Cứ mỗi lần ESP32 gửi lên, Broker sẽ lập tức "bơm" gói JSON đó đến và hiển thị trên màn hình

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
