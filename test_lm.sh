#!/bin/bash
curl http://192.168.1.40:1234/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen/qwen3-4b-thinking-2507","messages":[{"role":"user","content":"hi"}],"max_tokens":10}'
