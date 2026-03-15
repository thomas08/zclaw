#!/bin/bash
curl http://192.168.1.40:1234/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "qwen/qwen3-4b-thinking-2507",
    "messages": [
      {"role": "system", "content": "You are a helpful assistant."},
      {"role": "user", "content": "hi"}
    ],
    "tools": [
      {
        "type": "function",
        "function": {
          "name": "get_time",
          "description": "Get current time",
          "parameters": {"type": "object", "properties": {}}
        }
      }
    ],
    "max_tokens": 50
  }'
