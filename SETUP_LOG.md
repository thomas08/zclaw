# zclaw Setup Log

## Hardware
- **Board:** ESP32-S3 (current) / SH-BC3 ESP32-C3 (secondary, has BME280)
- **Serial port:** `/dev/ttyACM0`
- **LM Studio:** `192.168.1.40:1234` (Qwen3-4B)
- **WiFi:** MakerHub_2.4G

---

## สิ่งที่แก้ระหว่าง Setup

### 1. WiFi AUTH_EXPIRE (ESP32-C3)
**อาการ:** บอร์ดเห็น AP แต่ connect ไม่ได้ `reason=2 (AUTH_EXPIRE)`
**สาเหตุ:** ESP-IDF 5.x เปลี่ยน default PMF ทำให้ conflict กับ router
**แก้ที่:** `main/main.c` หลังบรรทัด `threshold.authmode`:
```c
wifi_config.sta.pmf_cfg.capable = true;
wifi_config.sta.pmf_cfg.required = false;
```

### 2. LM Studio API URL ผิด
**อาการ:** LLM response 53 bytes, ขึ้น `API Error (unknown)`
**สาเหตุ:** URL ที่ provision ขาด `/chat/completions`
**แก้:** ใช้ URL เต็ม `http://192.168.1.40:1234/v1/chat/completions`

### 3. Model ID ต้องตรงทุกตัวอักษร
**สาเหตุ:** ใส่ `qwen3-4b` แต่ LM Studio ใช้ `qwen/qwen3-4b-thinking-2507`
**ตรวจด้วย:** `curl http://192.168.1.40:1234/v1/models`

### 4. Boot Loop Guard
**อาการ:** `SAFE MODE - Too many boot failures`
**สาเหตุ:** WiFi fail → restart เร็ว → boot_count เพิ่ม → safe mode
**แก้:** กด BOOT ค้าง + กด RESET รอ 5 วินาที (factory reset)

### 5. ESP-IDF Submodule
**อาการ:** CMake error: `Cannot specify link libraries for target "mbedcrypto"`
**แก้:** Update เฉพาะ submodules ที่จำเป็น (ข้าม openthread):
```bash
cd ~/esp/esp-idf
git submodule update --init components/mbedtls/mbedtls components/lwip/lwip \
  components/heap/tlsf components/esp_wifi/lib components/esp_phy/lib \
  components/esp_coex/lib components/bt/host/nimble/nimble \
  components/spiffs/spiffs components/bt/controller/lib_esp32c3_family
```

---

## Build & Flash Commands

```bash
# Export IDF (ทำทุก terminal ใหม่)
. ~/esp/esp-idf/export.sh

# Build ESP32-S3
rm sdkconfig
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash

# Build ESP32-C3
rm sdkconfig
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash

# Monitor
./scripts/monitor.sh /dev/ttyACM0

# Provision (ใช้ prov.sh ที่สร้างไว้)
bash prov.sh
```

---

## Provision Parameters

| Key | Value |
|---|---|
| WiFi SSID | MakerHub_2.4G |
| WiFi Pass | YOUR_WIFI_PASSWORD |
| Backend | openai |
| API URL | http://192.168.1.40:1234/v1/chat/completions |
| API Key | lm-studio |
| Model | qwen/qwen3-4b-thinking-2507 |
| Telegram Token | (ใน prov.sh) |
| Telegram Chat ID | YOUR_CHAT_ID |

---

## Status ปัจจุบัน

- [x] ESP32-S3: flash + provision + Telegram + LLM ทำงาน
- [ ] ESP32-S3: เชื่อม BME280 (ยังไม่ได้ต่อ sensor)
- [ ] ESP32-C3 (SH-BC3): ต้อง factory reset (boot_count=17) แล้ว flash ใหม่
