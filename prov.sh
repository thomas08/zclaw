#!/bin/bash
# Local provisioning script — fill in YOUR credentials before running.
# This file is a template; do NOT commit real tokens.
./scripts/provision.sh \
  --port /dev/ttyACM0 \
  --backend openai \
  --api-key lm-studio \
  --api-url http://192.168.1.40:1234/v1/chat/completions \
  --tg-token "YOUR_TELEGRAM_BOT_TOKEN" \
  --tg-chat-id YOUR_CHAT_ID \
  --skip-api-check \
  --yes \
  --pass YOUR_WIFI_PASSWORD
  # --cloud-url https://example.com/sensor   # optional: enable cloud logging at provision time
  # --cloud-key "mytoken"                    # optional: Bearer auth for cloud endpoint
