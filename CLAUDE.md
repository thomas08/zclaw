# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

zclaw is an AI personal assistant firmware for ESP32 microcontrollers, written in C using ESP-IDF/FreeRTOS. It targets a strict firmware budget of ≤ 888 KiB total. The agent receives messages via Telegram or USB serial, calls an LLM (Anthropic, OpenAI, OpenRouter, or Ollama), executes tool calls, and replies.

## Build & Development Commands

**Prerequisites:** ESP-IDF v5.4 installed at `~/esp/esp-idf/` or `~/esp/v5.4/esp-idf/`, with cJSON (`apt install libcjson-dev` on Ubuntu).

```bash
# Build firmware
./scripts/build.sh

# Flash to device
./scripts/flash.sh --kill-monitor /dev/cu.usbmodem1101

# Provision credentials (WiFi SSID/pass, LLM backend/key, Telegram token/chat IDs)
./scripts/provision.sh --port /dev/cu.usbmodem1101

# Serial monitor
./scripts/monitor.sh /dev/cu.usbmodem1101

# Quick local dev cycle (uses ~/.config/zclaw/dev.env profile)
./scripts/provision-dev.sh --write-template   # create template once
./scripts/provision-dev.sh                    # reprovision without retyping secrets

# Run host tests (no hardware required)
./scripts/test.sh host

# Run host + build device tests
./scripts/test.sh all

# Check firmware size
./scripts/size.sh

# QEMU emulator (stub LLM, no hardware)
./scripts/emulate.sh
```

## Architecture

### Startup sequence (`main/main.c` → `app_main`)
1. NVS init → OTA check → factory reset check → boot loop guard
2. WiFi connect → NTP sync (`cron_init`)
3. LLM init → rate limiter → Telegram init → tools init → channel init
4. Create FreeRTOS queues and start tasks: `channel_task`, `telegram_task`, `agent_task`, `cron_task`

### Message flow
- **Inputs:** USB serial (`channel.c`) and Telegram long-poll (`telegram.c`) both push `channel_msg_t` structs into a shared `input_queue`.
- **Agent task** (`agent.c`): dequeues messages, maintains rolling conversation history (`MAX_HISTORY_TURNS=12`), calls LLM in an agentic loop (up to `MAX_TOOL_ROUNDS=5`), dispatches tool calls, sends replies to `channel_output_queue` and `telegram_output_queue`.
- **Outputs:** channel task writes to USB serial; telegram task sends to Telegram API.

### LLM integration (`main/llm.c`, `main/llm_auth.c`)
- Supports Anthropic, OpenAI, OpenRouter, Ollama backends (selected at runtime via NVS key).
- JSON request/response serialization in `main/json_util.c`.
- Retries with exponential backoff; budget capped at `LLM_RETRY_BUDGET_MS=45s`.

### Tool system
- **Built-in tools** are registered via the X-macro pattern in `main/builtin_tools.def`. To add a new tool: implement handler in `tools_*.c`, declare in `tools_handlers.h`, add `TOOL_ENTRY(...)` line in `builtin_tools.def`.
- **User tools** (`main/user_tools.c`): runtime-created tools stored in NVS. When called, the agent receives an action string and executes it using built-in tools.
- Tool handler signature: `bool handler(const cJSON *input, char *result, size_t result_len)` — return `true` for success or benign not-found; `false` for validation/execution errors; always write a human-readable message to `result`.

### Persistent storage
- All credentials and user state stored in ESP NVS via `main/memory.c`.
- NVS key constants are in `main/nvs_keys.h`; user memory keys must start with `u_`.
- Namespaces: `zclaw` (general), `zc_cron` (schedules), `zc_tools` (user tools), `zc_config` (config).

### Scheduler (`main/cron.c`)
- Three schedule types: `periodic` (every N minutes), `daily` (local time HH:MM), `once` (one-shot after N minutes).
- Checks every `CRON_CHECK_INTERVAL_MS=10s`; max `CRON_MAX_ENTRIES=16` entries.
- Cron-triggered turns block `cron_set` to prevent re-scheduling loops.

### Security
- Telegram chat ID allowlist enforced in `main/telegram_chat_ids.c` / `main/security.c`.
- GPIO access restricted to a configurable pin range (`GPIO_MIN_PIN`–`GPIO_MAX_PIN`) via `main/gpio_policy.c`.
- Boot loop guard (`main/boot_guard.c`): enters safe mode after `MAX_BOOT_FAILURES=4` consecutive rapid boots.

## Key Configuration (`main/config.h`)

All compile-time tuning lives here: buffer sizes, task stack sizes, queue depths, rate limits (`RATELIMIT_MAX_PER_HOUR=100`, `RATELIMIT_MAX_PER_DAY=1000`), LLM timeout/retry parameters, and the `SYSTEM_PROMPT`.

## Testing

Host tests live in `test/host/`. They use GCC on the host with mocks for ESP-IDF, FreeRTOS, and NVS. Tests are compiled with `-Wall -Wextra -Werror -Wshadow -Wformat=2` and AddressSanitizer enabled by default. Python tests for scripts/relay/bridge are also run via `unittest`.

To run a single test file manually (example):
```bash
cd test/host
gcc -std=c99 -Wall -Wextra -Werror -DTEST_BUILD -I../../main -I. \
    test_agent.c mock_esp.c mock_memory.c mock_llm.c mock_freertos.c \
    mock_tools.c mock_user_tools.c mock_ratelimit.c \
    ../../main/agent.c ../../main/agent_commands.c ../../main/agent_prompt.c \
    ../../main/json_util.c ../../main/security.c ../../main/text_buffer.c \
    ../../main/gpio_policy.c ../../main/tools_gpio.c ../../main/tools_i2c.c \
    ../../main/tools_system.c ../../main/cron_utils.c \
    ../../main/telegram_update.c ../../main/telegram_token.c \
    ../../main/telegram_chat_ids.c ../../main/telegram_poll_policy.c \
    ../../main/telegram_http_diag.c ../../main/llm_auth.c \
    ../../main/wifi_credentials.c ../../main/memory_keys.c \
    ../../main/boot_guard.c mock_system_diag_deps.c mock_i2c.c \
    -lcjson -o build/test_agent && ./build/test_agent
```

Refer to `scripts/test.sh` for the authoritative compile commands for each test binary.
