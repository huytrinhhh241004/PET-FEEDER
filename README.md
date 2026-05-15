Smart Pet Feeder (ESP32)
Hệ thống cho thú cưng ăn thông minh dựa trên khối lượng thực tế, tích hợp FreeRTOS và Web Dashboard.

1. Tổng quan (Overview)
Hệ thống này được thiết kế để tự động hóa việc chăm sóc thú cưng với độ chính xác cao. Khác với các hệ thống đổ hạt theo thời gian thuần túy dễ dẫn đến tình trạng thừa hoặc thiếu thức ăn, hệ thống này sử dụng cảm biến lực (Loadcell) để kiểm soát khối lượng thực tế hiện có trong bát.

Toàn bộ hệ thống vận hành trên nền tảng FreeRTOS, giúp quản lý đa nhiệm mượt mà giữa việc điều khiển phần cứng bao gồm Servo, Cân, màn hình LCD và duy trì kết nối không dây thông qua Web Server và Wi-Fi.

2. Các tính năng nổi bật
Định lượng thông minh (Smart Rationing)
Hệ thống tự động đọc khối lượng hiện tại trước khi xả hạt. Nếu bát còn thừa thức ăn từ bữa trước, máy sẽ tự động tính toán và chỉ đổ thêm phần còn thiếu để đạt đúng mục tiêu (Target) đã cài đặt, giúp thực phẩm luôn tươi mới và tránh lãng phí.

Đa nhiệm thời gian thực (Real-time Multitasking)
Sử dụng hệ điều hành thời gian thực FreeRTOS với các Task độc lập và cơ chế Semaphore. Điều này đảm bảo dữ liệu cân nặng từ Loadcell và thời gian từ module RTC luôn được cập nhật chính xác, không xảy ra xung đột dữ liệu khi thực hiện nhiều tác vụ đồng thời.

Điều khiển mượt mà (Smooth Discharge)
Áp dụng cơ chế "Slow Zone" trong việc điều khiển Servo:

Xả nhanh: Thực hiện khi khối lượng trong bát còn cách xa mục tiêu.

Mở hé (Nhỏ giọt): Thực hiện khi khối lượng gần đạt đến ngưỡng mục tiêu để triệt tiêu sai số do quán tính rơi của hạt, đảm bảo độ chính xác đến từng gram.

Dashboard Web tích hợp
Giao diện quản lý trực quan trên trình duyệt web cho phép người dùng:

Theo dõi cân nặng thực tế và trạng thái hoạt động của hệ thống qua mạng Wi-Fi.

Cài đặt lịch ăn cố định (Sáng/Chiều) và định lượng cụ thể trực tiếp từ thiết bị di động.

Xem biểu đồ thống kê trực quan hiển thị lượng thức ăn tiêu thụ và lượng thức ăn thừa theo từng ngày.

Lưu trữ dữ liệu ngoại tuyến (Data Logging)
Toàn bộ lịch sử hoạt động và dữ liệu ăn uống được ghi lại vào thẻ nhớ SD dưới dạng tệp tin .csv. Tính năng này hỗ trợ chủ nuôi trong việc phân tích thói quen ăn uống và theo dõi sức khỏe của thú cưng trong thời gian dài.

Chế độ dự phòng và An toàn
Nút nhấn vật lý: Cho phép người dùng ra lệnh cho ăn thủ công ngay lập tức mà không cần thông qua giao diện web.

Màn hình LCD: Hiển thị địa chỉ IP kết nối và thông số cân nặng tức thời, đảm bảo việc giám sát vẫn khả thi ngay cả khi không có điện thoại hoặc máy tính.

3. Thành phần phần cứng chính
Vi điều khiển: ESP32.

Cảm biến lực: Loadcell kết hợp module giải mã HX711.

Thời gian thực: Module RTC DS3231.

Cơ cấu chấp hành: Động cơ Servo.

Lưu trữ: Module đọc ghi thẻ nhớ SD.

Hiển thị: Màn hình LCD 16x2 giao tiếp I2C.
