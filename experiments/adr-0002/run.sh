#!/bin/sh
# run.sh — build and run the ADR-0002 experiments; the raw log is kept
# (never overwritten) under experiments/adr-0002/logs/.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$root/dist/experiments"
mkdir -p "$out" "$here/logs"
clang -arch arm64 -std=c17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
	-Wshadow -Wvla -Wformat=2 -Wimplicit-fallthrough -Wuninitialized \
	-Wsometimes-uninitialized -Wreturn-stack-address -fbounds-safety -O1 \
	"$here/probe.c" -o "$out/probe"
log="$here/logs/$(date +%Y%m%dT%H%M%S).log"
{
	echo "# $(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(uname -m)"
	echo "# $(clang --version | head -1)"
	echo "# trials=${1:-300}"
} >"$log"
rc=0
"$out/probe" "${1:-300}" >>"$log" 2>&1 || rc=$?
cat "$log"
echo "log: ${log#"$root"/}"
exit "$rc"
