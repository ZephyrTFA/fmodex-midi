#!/usr/bin/env bash
# Coverage-guided fuzzing of the SMF parser + player with libFuzzer
# (ASan/UBSan). usage: test/fuzz.sh [seconds] [jobs]
set -euo pipefail
cd "$(dirname "$0")/.."
SECS=${1:-300}; JOBS=${2:-8}
B=build/fuzz; mkdir -p "$B/corpus" "$B/artifacts"
cp -n test/midi/*.mid test/midi-bad/*.mid "$B/corpus/" 2>/dev/null || true
nix shell nixpkgs#clang -c clang -std=gnu11 -O1 -g -fsanitize=fuzzer,address,undefined \
    -fno-sanitize-recover=all -Isrc -o "$B/fuzz_smf" test/fuzz_smf.c src/smf.c src/player.c -lm
# libFuzzer writes fuzz-N.log per job into the working directory.
cd "$B"
./fuzz_smf corpus -max_total_time="$SECS" -jobs="$JOBS" -workers="$JOBS" \
    -max_len=4096 -artifact_prefix=artifacts/ -print_final_stats=1 2>&1 | tail -n 5
grep -h 'stat::number_of_executed_units' fuzz-*.log | awk '{s += $2} END {print "total execs:", s}'
ls artifacts
