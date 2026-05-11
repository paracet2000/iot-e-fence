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
## Repository
- origin: https://github.com/paracet2000/iot-e-fence

## การ push git โดยใช้ PowerShell Script
.\push-git.ps1 "update battery divider and sample stats"

## OTA upload

โปรเจกต์นี้ตั้งค่า OTA ไว้แล้ว โดย PlatformIO จะอัปโหลดไปที่ `192.168.4.1` ผ่าน `espota`

ก่อนกด Upload:

1. ให้เครื่องคอมพิวเตอร์เชื่อมต่อกับ WiFi ของบอร์ด `FENCE_PRO`
2. ให้บอร์ดทำงานตามปกติและเปิด ArduinoOTA อยู่
3. กด Upload ใน PlatformIO ได้เลย

ถ้าต้องการเปลี่ยนปลายทาง OTA ให้แก้ `upload_port` ใน `platformio.ini`

