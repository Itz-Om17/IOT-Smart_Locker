#!/bin/bash
# Fake ESP32. Usage: KEY=<device key> ./test-device.sh scan AB12CD34 | state | pin 123456
B=${BASE:-http://localhost:5000/api/device}
H=(-H "Content-Type: application/json" -H "x-device-id: LOCKER-01" -H "x-device-key: $KEY")
case $1 in
  scan)  curl -s "${H[@]}" -X POST $B/scan -d "{\"uid\":\"$2\"}" ;;
  state) curl -s "${H[@]}" $B/state ;;
  pin)   curl -s "${H[@]}" -X POST $B/verify-pin -d "{\"pin\":\"$2\"}" ;;
esac; echo
