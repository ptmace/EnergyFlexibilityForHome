# Smart Home Energy Flexibility: IoT & AI-Driven Management System

![C/C++](https://img.shields.io/badge/C%2FC%2B%2B-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white)
![Python](https://img.shields.io/badge/Python-3776AB?style=for-the-badge&logo=python&logoColor=white)
![AWS](https://img.shields.io/badge/AWS_EC2-232F3E?style=for-the-badge&logo=amazon-aws&logoColor=white)
![MQTT](https://img.shields.io/badge/MQTT-660066?style=for-the-badge&logo=mqtt&logoColor=white)
![Tasmota](https://img.shields.io/badge/Tasmota-1DA1F2?style=for-the-badge)

> Đồ án tốt nghiệp Kỹ thuật Máy tính - ĐH Bách Khoa ĐHQG TP.HCM (HCMUT). 
> Hệ thống quản lý năng lượng thông minh kết hợp thiết bị biên (Edge) và trí tuệ nhân tạo (Cloud AI) nhằm tối ưu hóa chi phí, bảo vệ lưới điện và cá nhân hóa trải nghiệm người dùng.

---

## System Preview
<img width="1619" height="747" alt="dashboardchinh" src="https://github.com/user-attachments/assets/61e1d861-fee7-41ea-9f95-ba24a75ba2f2" />

_Main Dashboard_
<img width="1632" height="764" alt="chitietnha1" src="https://github.com/user-attachments/assets/b95db5ae-58da-4654-b562-1101c8974143" />
<img width="1676" height="831" alt="chitietnha2" src="https://github.com/user-attachments/assets/d800775d-d945-4b68-a941-558d19eb1496" />
<img width="1676" height="836" alt="chitietnha3" src="https://github.com/user-attachments/assets/e640b6a3-51e7-45f5-b34c-5023164defc2" />
<img width="231" height="240" alt="nha1" src="https://github.com/user-attachments/assets/681b5800-899e-4e37-adc8-1bab74edc08a" />



---

## Key Features

Hệ thống biến đổi ngôi nhà từ trạng thái tiêu thụ thụ động sang quản lý năng lượng chủ động với các tính năng cốt lõi:

*   **Adaptive AI Decision Engine:** Tự động học thói quen người dùng và lập lịch thiết bị dựa trên dự báo năng lượng tái tạo (Random Forest).
*   **Multi-Objective Optimization (NSGA-II):** Cung cấp 5 tiêu chí tùy chọn cho người dùng: 
    *   `Min Cost` (Tiết kiệm chi phí biểu giá EVN)
    *   `Min CO2` (Bảo vệ môi trường)
    *   `Max Comfort` (Ưu tiên sự thoải mái)
    *   `Peak Shaving` (Cắt đỉnh tải bảo vệ Aptomat)
    *   `Grid Balance` (Cân bằng nguồn điện)
*   **Safety FSM (Finite State Machine):** Chuyển đổi mượt mà giữa chế độ Tự động/Thủ công. Tự động ngắt khẩn cấp (DEACTIVATE) khi quá tải hoặc quá nhiệt.
*   **Real-time IoT Communication:** Đo lường V, A, W, kWh liên tục với độ trễ thấp thông qua giao thức MQTT.

---

## System Architecture

Hệ thống được thiết kế theo mô hình phân lớp Edge-to-Cloud:
1.  **Edge Layer (Tầng thiết bị):** Sử dụng các Smart Plugs chạy firmware Tasmota (C/C++) để đo lường công suất và điều khiển đóng/ngắt rơ-le vật lý.
2.  **Network Layer:** Giao tiếp hai chiều bằng MQTT (Publish/Subscribe) thông qua Broker.
3.  **Cloud Layer (AWS EC2 Linux):** Chạy nền tảng xử lý Python gồm 2 luồng song song:
    *   *Offline Layer:* NSGA-II sinh 100 kịch bản Pareto và AI Random Forest cập nhật dự báo.
    *   *Online Layer:* Đánh giá kịch bản thời gian thực và xuất lệnh điều khiển.



* Sơ đồ khối hệ thống Edge-to-Cloud
<img width="680" height="400" alt="block-diagram" src="https://github.com/user-attachments/assets/9378031b-3f28-4966-9b6f-f9bad231d024" />

* Sơ đồ Module
<img width="1629" height="859" alt="modulev3" src="https://github.com/user-attachments/assets/3a808526-2422-41f2-a21f-c9ef02cfcb72" />

* Flowchart
<img width="402" height="1224" alt="Flowchart" src="https://github.com/user-attachments/assets/ea2459eb-fa45-4268-9e2f-74a14069ca48" />

* Sơ đồ FSM
<img width="1489" height="321" alt="FSM" src="https://github.com/user-attachments/assets/26bb4fa1-af1c-4bdf-96da-87b6bdc54a6f" />


---

## Tech Stack

*   **Hardware/Firmware:** Smart Plugs, ESP8266/ESP32, Tasmota, C/C++
*   **IoT & Cloud:** MQTT Protocol, AWS EC2 (Ubuntu), CoreIoT Platform
*   **Algorithms & AI:** Python, Scikit-learn (Random Forest), Pymoo (NSGA-II)

---

## Dashboard Walkthrough

### 1. AI Optimization Panel & FSM Control
Cho phép người dùng chọn tiêu chí tối ưu và theo dõi trạng thái hệ thống theo thời gian thực (Optimization / Manual / Recovery).

_Bảng chọn 5 tiêu chí_

<img width="369" height="410" alt="bangtieuchi" src="https://github.com/user-attachments/assets/af025b61-5aa0-443a-a6ff-e69839275281" />

_Bảng điều khiển trạng thái_

<img width="311" height="208" alt="fsm2" src="https://github.com/user-attachments/assets/2e963474-9447-4b8a-a57d-cfae8855eebf" /> 
<img width="310" height="202" alt="fsm-deactivate" src="https://github.com/user-attachments/assets/a98d16a3-fda3-438e-933e-94795f750fa9" /> 
<img width="313" height="209" alt="fsm-recovery" src="https://github.com/user-attachments/assets/730bface-c3f4-4b8d-962e-e47ea2f0c23e" /> 
<img width="303" height="211" alt="fsm-reset" src="https://github.com/user-attachments/assets/5ecc128f-2f10-4751-83ac-2a6a1f5df0c9" /> 
<img width="312" height="203" alt="ManualMode" src="https://github.com/user-attachments/assets/93e570db-f049-4a27-8d5f-ba1f17d2caca" />

### 2. Auto-Pilot Scheduler
Hiển thị lịch biểu bật tắt thiết bị tự động trong 24 giờ tới sau khi hệ thống đã tính toán Pareto và chốt kịch bản.
<img width="860" height="375" alt="scheduler" src="https://github.com/user-attachments/assets/e9ebf69b-54bf-4923-9b32-5f8d8b17bcd4" />


### 3. Environment & Power Telemetry
Giám sát nhiệt độ, bức xạ mặt trời, lượng CO2, chi phí ước tính và trạng thái từng tải cục bộ.
<img width="1613" height="818" alt="chitietchisothietbi" src="https://github.com/user-attachments/assets/f5149641-cd7a-4f00-8957-942aa1a34cfb" />

<img width="375" height="154" alt="thongso" src="https://github.com/user-attachments/assets/1991834b-5c2f-4ee0-aa94-df8e0b93ab9a" />

_Sơ đồ bố trí thiết bị_

<img width="500" height="283" alt="sodobotrithietbi" src="https://github.com/user-attachments/assets/ca061623-215f-40cd-845f-9078748c19c2" /> <img width="283" height="295" alt="chitiet1thietbi" src="https://github.com/user-attachments/assets/b324f4a2-3501-4f95-9184-eba7b9219f66" />

_Giám sát chi tiết_

<img width="422" height="307" alt="tongdiennang" src="https://github.com/user-attachments/assets/c758185e-ed3c-442d-bd17-269d062b2663" />   <img width="485" height="351" alt="dienap" src="https://github.com/user-attachments/assets/992d6481-cb12-46aa-987b-267b1247d439" />

_Thông báo_
<img width="1401" height="391" alt="bangthongbao" src="https://github.com/user-attachments/assets/5cdc4d9c-756c-432b-a702-d4535e4f1c48" />

---

## Team Members
*   **Nguyễn Hữu An Lợi** (Computer Engineering - HCMUT)
*   **Phạm Thị Mai Anh** (Computer Engineering - HCMUT)
*   **Instructor:** TS. Lê Trọng Nhân

[Đọc báo cáo đồ án chi tiết (PDF) tại đây](https://drive.google.com/file/d/1oGO15nROYrnm8KpokoeONRbM9cbjRag5/view?usp=drive_link)
