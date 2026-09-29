#!/bin/bash
# measure.sh — the Phase 1 sleep measurement fixed in ADR-0005.
#
# Runs `timeout 120 sleep 600` and records when it ends. Meanwhile, put the
# Mac to sleep (Apple menu > Sleep) and wake it after at least 3 minutes.
# Nothing is changed on the machine: the sleep and wake times are only read
# from `pmset -g log`.
#
# Pass: the run does not end on waking, and ends with 124 once the awake
# time (wall time minus time asleep; a DarkWake counts as awake) adds up to
# about 120 seconds.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
bin="$root/dist/timeout"
[ -x "$bin" ] || { echo "build first: make build" >&2; exit 2; }
mkdir -p "$here/logs"
log="$here/logs/$(date +%Y%m%dT%H%M%S).log"

epoch() { date -j -f "%Y-%m-%d %H:%M:%S" "$1" +%s; }

{
	echo "# $(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(uname -m)"
	echo "# $("$bin" --version)"
} >"$log"

start=$(date "+%Y-%m-%d %H:%M:%S")
echo "start: $start — put the Mac to sleep now, and wake it after at least 3 minutes." | tee -a "$log"
"$bin" 120 sleep 600
rc=$?
end=$(date "+%Y-%m-%d %H:%M:%S")
echo "end:   $end, exit status $rc" | tee -a "$log"

# Sleep intervals inside [start, end]: from each Sleep to the next Wake or DarkWake.
s0=$(epoch "$start")
s1=$(epoch "$end")
asleep=0
last_wake=""
sleep_at=""
echo "# pmset -g log, Sleep / Wake / DarkWake between start and end:" >>"$log"
while IFS= read -r line; do
	ts=$(printf '%s' "$line" | cut -c1-19)
	t=$(epoch "$ts") || continue
	[ "$t" -lt "$s0" ] && continue
	[ "$t" -gt "$s1" ] && break
	echo "  $line" | cut -c1-120 >>"$log"
	case "$(printf '%s' "$line" | awk '{print $4}')" in
	Sleep) sleep_at=$t ;;
	Wake | DarkWake)
		if [ -n "$sleep_at" ]; then
			asleep=$((asleep + t - sleep_at))
			sleep_at=""
		fi
		last_wake=$t
		;;
	esac
# The type column is padded: "Wake    " but "Wake Requests" (a schedule, not a wake).
done < <(pmset -g log | grep -E '^[0-9-]+ [0-9:]+ [+-][0-9]+ (Sleep|Wake|DarkWake)[[:space:]]{2,}')

wall=$((s1 - s0))
awake=$((wall - asleep))
after_wake=""
[ -n "$last_wake" ] && after_wake=$((s1 - last_wake))
{
	echo "wall ${wall}s, asleep ${asleep}s, awake ${awake}s, ended ${after_wake:-?}s after the last wake"
	if [ "$rc" -eq 124 ] && [ "$asleep" -ge 180 ] && [ "$awake" -ge 110 ] && [ "$awake" -le 140 ] &&
		[ -n "$after_wake" ] && [ "$after_wake" -ge 5 ]; then
		echo "PASS: time asleep was not counted"
	else
		echo "FAIL (or not measured: asleep must be at least 180s)"
	fi
} | tee -a "$log"
echo "log: ${log#"$root"/}"
