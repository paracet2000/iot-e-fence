# Electric Fence ESP32 (PlatformIO)

## วิธีใช้งาน

1. เปิดโฟลเดอร์นี้ด้วย VS Code (ติดตั้ง PlatformIO Extension แล้ว)
2. แก้ไข WiFi ชื่อและรหัสผ่านใน src/main.cpp
3. กดปุ่ม Upload เพื่ออัปโหลดโค้ดเข้า ESP32
4. Serial Monitor ใช้ความเร็ว 115200

## ไลบรารีที่ใช้
- ESPmDNS
- arduino-timer

## หมายเหตุ
- หากต้องการ OTA ให้เชื่อมต่อ WiFi เดียวกับคอมพิวเตอร์
- ค่า Pulse จำกัดไม่เกิน 5000us เพื่อความปลอดภัย
