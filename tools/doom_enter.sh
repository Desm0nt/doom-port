#!/bin/sh
# From a fresh launch to gameplay (save "The UAC"): brightness A -> slot A -> CONTINUE GAME A -> loading -> "Press B to continue".
cd "$(dirname "$0")/.."
sh tools/phone_pad.sh wait:55 A wait:10 A wait:8 A wait:${1:-40} B wait:10 B wait:10 B wait:10 B wait:10 B wait:10 B wait:10 shot:entered
