Smart Pet Feeder (ESP32) - Hệ thống cho thú cưng ăn thông minh
1. Tổng quan (Overview)
Hệ thống này được thiết kế để tự động hóa việc chăm sóc thú cưng một cách chính xác nhất. Thay vì chỉ đổ hạt theo thời gian (dễ dẫn đến thừa/thiếu), hệ thống sử dụng cảm biến lực (Loadcell) để kiểm soát khối lượng thực tế trong bát.

Toàn bộ hệ thống vận hành trên nền tảng FreeRTOS, giúp quản lý đa nhiệm mượt mà giữa việc điều khiển phần cứng (Servo, Cân, LCD) và duy trì kết nối không dây (Web Server, Wi-Fi).

2. Các tính năng nổi bật
Định lượng: Hệ thống đọc khối lượng hiện tại trước khi xả. Nếu bát còn thừa thức ăn, máy sẽ tự động tính toán và chỉ đổ thêm phần còn thiếu để đạt đúng mục tiêu (Target).

Đa nhiệm thời gian thực: Sử dụng FreeRTOS với các Task độc lập và Semaphore để đảm bảo dữ liệu cân nặng và thời gian RTC luôn chính xác, không bị xung đột.

Điều khiển mượt mà: Áp dụng cơ chế "Slow Zone" cho Servo: xả nhanh khi còn ít và chuyển sang mở hé (nhỏ giọt) khi gần đạt mục tiêu để tránh sai số khối lượng.

Dashboard Web tích hợp:

Theo dõi cân nặng và trạng thái hệ thống qua Wi-Fi.

Cài đặt lịch ăn (Sáng/Chiều) và định lượng trực tiếp từ điện thoại.

Biểu đồ thống kê hiển thị lượng ăn và lượng thức ăn thừa theo ngày.

Lưu trữ dữ liệu ngoại tuyến: Toàn bộ lịch sử ăn uống được ghi log vào thẻ nhớ SD dưới dạng file CSV để phân tích thói quen ăn uống của thú cưng lâu dài.

Chế độ dự phòng: Có nút nhấn vật lý để cho ăn thủ công và màn hình LCD hiển thị IP/Cân nặng khi không có điện thoại/máy tính.
