#!/bin/bash
# aes67ctl add/list/remove against files in a temp directory. Never restarts
# coreaudiod and never sends: streams are only written, on lo0.
set -u
CTL="$1"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT
export AES67_CONFIG_PATH="$DIR/streams.json"
export AES67_PTP_CONFIG_PATH="$DIR/ptp.json"
failures=0
check() { if eval "$2"; then :; else echo "  FAIL: $1"; failures=$((failures + 1)); fi; }

"$CTL" add send 239.69.250.2 --interface lo0 --no-restart >/dev/null
check "a send stream is added" '[ $? -eq 0 ] && [ -f "$AES67_CONFIG_PATH" ]'
"$CTL" add receive 239.69.250.1 --interface lo0 --channels 1 --no-restart >/dev/null
check "a receive stream is added" '[ $? -eq 0 ]'

out="$("$CTL" list)"
check "list shows the send stream on outputs 1-2" 'grep -q "send     239.69.250.2:5004  2 ch  outputs 1-2  lo0" <<<"$out"'
check "list shows the receive stream on input 1" 'grep -q "receive  239.69.250.1:5004  1 ch  inputs 1  lo0" <<<"$out"'

"$CTL" add send 239.69.250.3 --interface lo0 --first-channel 1 --no-restart 2>"$DIR/err" >/dev/null
check "a clashing send stream is refused" '[ $? -eq 1 ] && grep -q "device channels 1-2" "$DIR/err"'
"$CTL" add send 239.69.250.3 --interface lo0 --no-restart >/dev/null
check "without --first-channel a stream goes on the first free channels" '[ $? -eq 0 ] && "$CTL" list | grep -q "send     239.69.250.3:5004  2 ch  outputs 3-4  lo0"'

"$CTL" add send 239.69.250.2 --interface lo0 --channels 1 --no-restart >/dev/null
check "the same group replaces its stream" '[ "$("$CTL" list | grep -c 239.69.250.2)" -eq 1 ] && "$CTL" list | grep -q "239.69.250.2:5004  1 ch"'

"$CTL" add send 239.69.250.4 --interface lo0 --channels 9 --no-restart 2>/dev/null >/dev/null
check "more than 8 channels is a usage error" '[ $? -eq 2 ]'
"$CTL" add send 239.69.250.4 --interface "lo0;x" --no-restart 2>/dev/null >/dev/null
check "an odd interface name is refused" '[ $? -eq 2 ]'

"$CTL" add send -inet --interface lo0 --no-restart 2>/dev/null >/dev/null
check "a group that isn't a multicast address is a usage error" '[ $? -eq 2 ]'
"$CTL" doctor --group example.com 2>/dev/null >/dev/null
check "doctor refuses a hostname as a group" '[ $? -eq 2 ]'

"$CTL" remove 239.69.250.3 --no-restart >/dev/null
check "a group is removed" '[ $? -eq 0 ] && ! "$CTL" list | grep -q 239.69.250.3'
"$CTL" remove 239.69.250.3 --no-restart 2>/dev/null >/dev/null
check "removing an unknown group fails" '[ $? -eq 1 ]'

echo "not json" >"$AES67_CONFIG_PATH"
"$CTL" add send 239.69.250.5 --interface lo0 --no-restart 2>/dev/null >/dev/null
check "a corrupt file is left alone" '[ $? -eq 1 ] && [ "$(cat "$AES67_CONFIG_PATH")" = "not json" ]'

check "no backup is made of a corrupt file" '[ ! -e "$AES67_CONFIG_PATH.bak" ]'
rm "$AES67_CONFIG_PATH"
echo '{"version": "1.0", "streams": []}' >"$DIR/elsewhere.json"
ln -s "$DIR/elsewhere.json" "$AES67_CONFIG_PATH"
"$CTL" list 2>/dev/null >/dev/null
check "a symlinked streams file is refused" '[ $? -eq 1 ]'

if [ "$failures" -ne 0 ]; then
    echo "aes67ctl CLI: $failures failed"
    exit 1
fi
echo "aes67ctl CLI: all passed"
