Smart Pet Feeder (Hệ thống Máy cho ăn thông minh ESP32)
Hệ thống điều khiển máy cho thú cưng ăn tự động và thủ công bằng ESP32, tích hợp cân điện tử kiểm soát khối lượng thức ăn chính xác, lưu trữ lịch sử qua thẻ nhớ SD và cung cấp giao diện Web Responsive hiển thị biểu đồ phân tích xu hướng ăn uống của thú cưng.

Tính năng nổi bật

Định lượng thông minh (Loadcell HX11): Tự động điều chỉnh lượng thức ăn đổ thêm dựa trên khối lượng thức ăn còn thừa trong bát để đạt đúng mục tiêu khẩu phần.

Điều khiển Servo chống giật: Sử dụng 3 cấp độ góc mở (Mở hoàn toàn - Mở hé giảm tốc-Đóng hẳn) giúp thức ăn chảy mượt, không bị kẹt hay giật cơ học.

Đa nhiệm thời gian thực (FreeRTOS): Quản lý đồng thời 6 tác vụ (Task) độc lập (Đọc cân, cập nhật thời gian RTC, quét nút bấm, quét Web Server, hiển thị LCD, điều khiển Servo) thông qua cơ chế đồng bộ Mutex và Semaphore.

Trình quản lý WiFi (WiFiManager): Tự động phát AP (PET_FEEDER) để cấu hình WiFi khi không kết nối được mạng cũ. Nhấn giữ nút cứng 3 giây để reset WiFi.

Phân tích & Thống kê nâng cao: Theo dõi lượng thức ăn thừa, tính toán lượng thực tế thú cưng đã ăn, đưa ra các cảnh báo thông minh (Insight) như gợi ý giảm khẩu phần ăn nếu thú cưng bỏ thừa nhiều.
