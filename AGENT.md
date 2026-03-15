# AGENT.md — IoT Agents Development Guide
> สำหรับ Claude Code / Claude CLI
> ต่อจากงานเดิม: zclaw + LM Studio + SH-BC3 + ESP32-S3 ทำงานได้แล้ว
> เป้าหมายใหม่: พัฒนาเป็น IoT Agents ที่รู้จัก sensor หลายตัว + autonomous heartbeat

---

## สถานะปัจจุบัน (ทำได้แล้ว)

```
✅ ESP32-S3: flash zclaw + provision + Telegram + LM Studio ทำงานได้
✅ LM Studio: Qwen3-4B (qwen/qwen3-4b-thinking-2507) ที่ 192.168.1.40:1234
✅ Telegram: @thomas_zclaw_bot, Chat ID: YOUR_CHAT_ID
✅ WiFi: MakerHub_2.4G
✅ GPIO tool: ทำงานได้
✅ Schedule tool: ทำงานได้
✅ Memory (NVS): ทำงานได้
✅ BME280 sensor layer: เขียนแล้ว (sensor_registry + driver)
❌ BME280: ยังไม่ได้ทดสอบ hardware — ต่อสาย SDA=GPIO8, SCL=GPIO9
✅ Heartbeat loop: เขียนแล้ว (heartbeat.c + tools_heartbeat.c)
✅ Cloud logging: เขียนแล้ว (cloud_log.c + tools_cloud.c)
```

---

## Hardware

| บอร์ด | Chip | Serial Port | สถานะ |
|---|---|---|---|
| ESP32-S3 DevKit | ESP32-S3 | /dev/ttyACM0 | ✅ primary |
| ESP32-C3 (ทั่วไป) | ESP32-C3 | /dev/ttyACM0 | ✅ รองรับ (set-target esp32c3) |

**Sensors (ต่อ external):**
- BME280 → I2C 0x76, SDA=GPIO8, SCL=GPIO9 (default, แก้ได้ใน sensor_bme280.h)

---

## ปัญหาที่พบจริงและวิธีแก้ (อย่าลืม)

### WiFi AUTH_EXPIRE
```c
// main/main.c — เพิ่มหลัง threshold.authmode
wifi_config.sta.pmf_cfg.capable = true;
wifi_config.sta.pmf_cfg.required = false;
```

### LM Studio URL ต้องครบ
```
# ผิด:  http://192.168.1.40:1234/v1
# ถูก:  http://192.168.1.40:1234/v1/chat/completions
```

### Model ID ต้องตรงทุกตัวอักษร
```bash
curl http://192.168.1.40:1234/v1/models  # ดู "id" field
# ใช้: qwen/qwen3-4b-thinking-2507
```

### Boot Loop → SAFE MODE
```bash
# กด BOOT ค้าง + RESET รอ 5 วินาที
# หรือ: ./scripts/erase.sh --nvs --port /dev/ttyACM0
```

### ESP-IDF Submodules ไม่ครบ (CMake error mbedcrypto)
```bash
cd ~/esp/esp-idf
git submodule update --init \
  components/mbedtls/mbedtls \
  components/lwip/lwip \
  components/esp_wifi/lib \
  components/esp_phy/lib \
  components/bt/host/nimble/nimble \
  components/spiffs/spiffs \
  components/bt/controller/lib_esp32c3_family   # เฉพาะตอน build esp32c3
```

---

## สถาปัตยกรรมใหม่: zclaw-iot

### หลักการ
1. **ไม่แตะ zclaw core** — เพิ่มเป็น layer ใหม่ทั้งหมด
2. **Sensor registry pattern** — X-macro เหมือน builtin_tools.def
3. **LLM เรียกเฉพาะตอนจำเป็น** — heartbeat ไม่ผ่าน LLM ทุก cycle
4. **เพิ่ม sensor ใหม่ใช้เวลาไม่เกิน 1 ชั่วโมง**

### โครงสร้างไฟล์เป้าหมาย
```
main/
├── (zclaw core — ไม่แตะ)
│   ├── main.c               ← เพิ่มแค่ heartbeat_init() และ sensor_registry_init()
│   ├── agent.c / llm.c / telegram.c / cron.c / memory.c
│   ├── tools_gpio.c / tools_i2c.c / tools_system.c / user_tools.c
│   └── builtin_tools.def    ← เพิ่ม TOOL_ENTRY เท่านั้น
│
├── sensors/                 ← NEW Layer
│   ├── sensor_registry.h    ← interface กลาง + X-macro SENSOR_TABLE
│   ├── sensor_registry.c    ← init ทุกตัว + read by name
│   ├── sensor_bme280.c      ← BME280 driver (Step 1)
│   ├── sensor_mpu6050.c     ← MPU-6050 driver (Step 5)
│   └── sensor_dht11.c       ← DHT11 driver (ถ้าต้องการ)
│
├── heartbeat.c / heartbeat.h    ← NEW: FreeRTOS task + threshold rules
├── tools_sensors.c              ← NEW: read_sensor + list_sensors tools
└── cloud_log.c / cloud_log.h    ← NEW: HTTP POST to endpoint
```

---

## Step 1: Sensor Abstraction Layer

### `main/sensors/sensor_registry.h`
```c
#pragma once
#include <stdbool.h>

typedef struct {
    float   values[4];
    const char *labels[4];
    int     count;
    bool    valid;
} sensor_data_t;

typedef struct {
    const char *name;
    const char *description;
    bool (*init)(void);
    bool (*read)(sensor_data_t *out);
    bool enabled;
} sensor_driver_t;

// เพิ่ม sensor ใหม่ที่นี่บรรทัดเดียว
#define SENSOR_TABLE \
    SENSOR_ENTRY("bme280",  "Temp/Humidity/Pressure I2C 0x76", \
                  bme280_init,  bme280_read)  \
    SENSOR_ENTRY("mpu6050", "Accelerometer/Gyroscope I2C 0x68", \
                  mpu6050_init, mpu6050_read) \

bool sensor_registry_init(void);
bool sensor_read_by_name(const char *name, sensor_data_t *out);
void sensor_list_all(char *buf, size_t len);
```

### `main/sensors/sensor_bme280.c`
```c
// BME280: I2C 0x76, SDA=GPIO8, SCL=GPIO9
// Chip ID register 0xD0 → ควรคืน 0x60
// ต้องทำ:
// 1. i2c_master_bus_add_device() ด้วย address 0x76
// 2. ตรวจ chip ID
// 3. ตั้งค่า forced mode + oversampling
// 4. อ่าน raw ADC แล้ว compensate ตาม BME280 datasheet

bool bme280_init(void);
bool bme280_read(sensor_data_t *out);
// out->values[0] = temp (°C)
// out->values[1] = humidity (%)
// out->values[2] = pressure (hPa)
// out->count = 3
```

### `main/tools_sensors.c`
```c
// Tool: read_sensor — AI เรียก {"name": "bme280"}
// Tool: list_sensors — แสดง sensor ทั้งหมดที่มี
// ลงทะเบียนใน builtin_tools.def:
//   TOOL_ENTRY("read_sensor", tool_read_sensor, "Read sensor by name")
//   TOOL_ENTRY("list_sensors", tool_list_sensors, "List all sensors")
```

### ทดสอบ Step 1
```
พิมพ์ใน Telegram: "list sensors"
คาดหวัง: แสดง bme280, mpu6050

พิมพ์ใน Telegram: "read sensor bme280"
คาดหวัง: temp=28.5°C, humidity=72%, pressure=1013hPa
```

---

## Step 2: ลงทะเบียน Tool ใน builtin_tools.def

```c
// เพิ่มใน main/builtin_tools.def
TOOL_ENTRY("read_sensor",
    tool_read_sensor,
    "Read sensor data. Input: {\"name\": \"bme280\"}")

TOOL_ENTRY("list_sensors",
    tool_list_sensors,
    "List all available sensors and their status")
```

---

## Step 3: Heartbeat Loop

### `main/heartbeat.c`
```c
// FreeRTOS task ใหม่ — ไม่ขึ้นกับ cron.c เดิม
// interval: 5 นาที (ปรับได้ผ่าน NVS key "hb_interval_min")
// threshold rules เก็บใน NVS: "hb_rule_0" ... "hb_rule_15"
// format: "bme280.temp_c > 35 : ห้องร้อนเกิน 35°C"

// Logic แต่ละ cycle:
// 1. อ่าน sensor ทุกตัวที่ enabled
// 2. เปรียบเทียบกับ rules ทุกข้อ
// 3. ถ้าปกติ → cloud_log_write() เงียบๆ
// 4. ถ้าผิดปกติ → inject message เข้า input_queue
//    → agent ตื่นขึ้นมาส่ง Telegram alert อัตโนมัติ

// เพิ่มใน main/main.c หลัง tools_init():
//   heartbeat_init();

// Tools สำหรับ AI ตั้งค่าผ่าน Telegram:
//   heartbeat_add_rule    → "แจ้งถ้าอุณหภูมิเกิน 35°C"
//   heartbeat_list_rules  → แสดง rules ทั้งหมด
//   heartbeat_delete_rule → ลบ rule ตาม index
```

### ทดสอบ Step 3
```
พิมพ์ใน Telegram: "แจ้งฉันถ้าอุณหภูมิเกิน 30°C"
คาดหวัง: AI สร้าง heartbeat rule

รอ 5 นาที (หรือลด interval ชั่วคราวเป็น 1 นาที)
คาดหวัง: Telegram ได้รับ alert อัตโนมัติโดยไม่ต้องส่งคำสั่ง
```

---

## Step 4: Cloud Logging

### Cloud Logging (Optional Feature)

Cloud logging เป็น **optional** — heartbeat และ sensor tools ทำงานได้ปกติโดยไม่ต้องตั้งค่า

**เปิดใช้งานผ่าน Telegram:**
```
"ตั้ง cloud endpoint เป็น https://example.com/sensor"
→ AI เรียก cloud_set_endpoint

"push sensor data ไป cloud เดี๋ยวนี้"
→ AI เรียก cloud_push_now

"ปิด cloud logging"
→ AI เรียก cloud_clear_endpoint
```

**เปิดใช้งานตอน provision (ถ้าต้องการ):**
```bash
./scripts/provision.sh ... \
  --cloud-url https://example.com/sensor \
  --cloud-key "mytoken"   # optional Bearer token
```

**JSON payload:**
```json
{"sensor":"bme280","ts":1720000000,"temp_c":28.5,"humidity_pct":72.1,"pressure_hpa":1013.2}
```

ถ้า POST ล้มเหลว → เก็บใน ring buffer (8 entries) แล้ว retry รอบถัดไป

---

## Step 5: เพิ่ม Sensor ใหม่ (pattern สำหรับอนาคต)

```c
// ทำแค่ 3 อย่าง:

// 1. เขียน driver (~100 บรรทัด)
// main/sensors/sensor_xxx.c
bool xxx_init(void) { /* init I2C device */ }
bool xxx_read(sensor_data_t *out) { /* read + fill out */ }

// 2. ลงทะเบียน — 1 บรรทัด
// main/sensors/sensor_registry.h ใน SENSOR_TABLE:
SENSOR_ENTRY("xxx", "Description", xxx_init, xxx_read)

// 3. ทดสอบ
// พิมพ์ใน Telegram: "read sensor xxx"
```

---

## คำสั่ง Build

```bash
# Export IDF (ทำทุก terminal ใหม่)
. ~/esp/esp-idf/export.sh

# Build ESP32-S3 (primary)
rm sdkconfig && idf.py set-target esp32s3 && idf.py build

# Build ESP32-C3
rm sdkconfig && idf.py set-target esp32c3 && idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash

# Flash + Monitor
idf.py -p /dev/ttyACM0 flash monitor

# Monitor เฉยๆ
./scripts/monitor.sh /dev/ttyACM0

# Provision (ใช้ prov.sh)
bash prov.sh

# Erase credentials เท่านั้น (คง firmware)
./scripts/erase.sh --nvs --port /dev/ttyACM0

# ตรวจขนาด firmware
idf.py size

# Run host tests (ไม่ต้องมี hardware)
./scripts/test.sh host
```

---

## Provision Script (prov.sh)

```bash
#!/bin/bash
# ไม่ต้องกำหนด --model: firmware จะ query /v1/models จาก LM Studio
# แล้วใช้โมเดลที่ load อยู่ตอน boot อัตโนมัติ
./scripts/provision.sh \
  --port /dev/ttyACM0 \
  --backend openai \
  --api-key lm-studio \
  --api-url http://192.168.1.40:1234/v1/chat/completions \
  --tg-token "YOUR_BOT_TOKEN_HERE" \
  --tg-chat-id YOUR_CHAT_ID \
  --skip-api-check \
  --yes \
  --pass YOUR_WIFI_PASSWORD
```

> **หมายเหตุ:** ถ้าต้องการ pin โมเดลเฉพาะ ให้เพิ่ม `--model "ชื่อโมเดล"` กลับมา

---

## ถ้าติดปัญหา — ข้อมูลที่ต้องส่งให้ Claude

1. กำลัง implement Step ไหน (1–5)
2. Error message เต็มๆ (copy ทั้งหมด)
3. `idf.py -p /dev/ttyACM0 monitor` output
4. LM Studio Server log (ถ้าปัญหา tool calling)
5. `idf.py size` (ถ้า firmware เกิน limit)
6. `dmesg | tail -20` (ถ้าปัญหา USB/serial)

---

*zclaw base: github.com/tnm/zclaw | docs: zclaw.dev*
*IoT Agents: sensor layer + heartbeat + cloud log*
*Board: ESP32-S3 DevKit*
*LLM: LM Studio Qwen3-4B local*
