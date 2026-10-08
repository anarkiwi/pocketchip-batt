#!/bin/sh
# Exercise pocketchip-batt against a fake power_supply tree.
# Usage: test/run-tests.sh path/to/pocketchip-batt
set -u
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
T=$(mktemp -d)
PID=
trap '[ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$T"' EXIT
export POCKETCHIP_BATT_SYSFS="$T/sys" POCKETCHIP_BATT_DIR="$T/out"
BAT="$T/sys/axp20x-battery"
pass=0
fail=0

check() {
  if [ "$2" = "$3" ]; then
    pass=$((pass + 1))
    echo "ok   $1"
  else
    fail=$((fail + 1))
    echo "FAIL $1: expected [$3] got [$2]"
  fi
}

reset() {
  rm -rf "$T/sys" "$T/out"
  mkdir -p "$BAT" "$T/out"
}

put() { printf '%s\n' "$2" >"$BAT/.$1" && mv "$BAT/.$1" "$BAT/$1"; }

once() {
  rm -f "$T/out/voltage" "$T/out/charging"
  "$BIN" --once
  check "$1 voltage" "$(od -An -c "$T/out/voltage" | tr -d ' ')" "$2\n"
  check "$1 charging" "$(od -An -c "$T/out/charging" | tr -d ' ')" "$3\n"
}

await() {
  i=0
  while [ "$(cat "$T/out/$1" 2>/dev/null)" != "$2" ] && [ $i -lt 500 ]; do
    sleep 0.01
    i=$((i + 1))
  done
  check "$3" "$(cat "$T/out/$1" 2>/dev/null)" "$2"
}

hardlinked() { [ "$(stat -c %h "$T/$1.keep")" = 2 ] && echo same; }

reset
put voltage_now 4188000
put status Charging
once "charging" 4188 1
put voltage_now 4188999
for s in Discharging Full 'Not charging' Unknown; do
  put status "$s"
  once "status $s, uV truncated" 4188 0
done
printf 'Charging' >"$BAT/status"
printf '3500000' >"$BAT/voltage_now"
once "no trailing newline" 3500 1

rm "$BAT/voltage_now" "$BAT/status"
once "missing attributes" 3700 0
for v in '' abc 0 -4000000 99999999999999999999; do
  put voltage_now "$v"
  once "bad voltage [$v]" 3700 0
done
: >"$BAT/voltage_now"
once "empty voltage file" 3700 0
rm -rf "$T/sys"
once "missing supply" 3700 0

rm -rf "$T/out"
"$BIN" --once 2>/dev/null
check "missing output dir creates nothing" "$([ -e "$T/out" ] && echo exists)" ""
reset
mkdir "$T/out/voltage"
"$BIN" --once 2>/dev/null
check "failed rename leaves no temp file" "$(find "$T/out" -name '.*' | wc -l)" 0
check "failed voltage still publishes charging" "$(cat "$T/out/charging")" 0
"$BIN" --bogus 2>/dev/null
check "usage exit status" $? 2

reset
put voltage_now 4000000
put status Discharging
POCKETCHIP_BATT_PERIOD_MS=200 "$BIN" &
PID=$!
await voltage 4000 "first sample seeds average"
await charging 0 "daemon charging"
ln "$T/out/voltage" "$T/voltage.keep"
ln "$T/out/charging" "$T/charging.keep"
sleep 1
check "unchanged voltage not rewritten" "$(hardlinked voltage)" same
check "unchanged charging not rewritten" "$(hardlinked charging)" same
put voltage_now 3800000
for want in 3900 3850 3825 3812 3806 3803 3801 3800; do
  await voltage "$want" "average step to $want"
done
check "changed voltage rewritten" "$(hardlinked voltage)" ""
check "charging untouched by voltage change" "$(hardlinked charging)" same
rm "$BAT/voltage_now"
sleep 0.5
check "missing sample keeps average" "$(cat "$T/out/voltage")" 3800
put status Charging
await charging 1 "charging change published"
check "no temp files left" "$(find "$T/out" -name '.*' | wc -l)" 0
kill "$PID"
wait "$PID"
check "clean exit on SIGTERM" $? 0
PID=

uevent() {
  python3 -c 'import socket, sys
socket.socket(socket.AF_NETLINK, socket.SOCK_DGRAM, 15).sendto(
    sys.argv[1].replace(",", "\0").encode() + b"\0", (0, 1))' "$1" 2>/dev/null
}
if command -v python3 >/dev/null && uevent add@/probe; then
  reset
  put voltage_now 4000000
  put status Discharging
  POCKETCHIP_BATT_PERIOD_MS=600000 "$BIN" &
  PID=$!
  await charging 0 "uevent daemon started"
  put status Charging
  uevent change@/devices/kbd,SUBSYSTEM=input
  sleep 0.3
  check "uevent from other subsystem ignored" "$(cat "$T/out/charging")" 0
  uevent change@/devices/usb,ACTION=change,SUBSYSTEM=power_supply
  await charging 1 "power_supply uevent resamples charging"
  kill "$PID"
  wait "$PID"
  PID=
else
  echo "skip uevent tests: sending uevents needs python3 and CAP_NET_ADMIN"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
