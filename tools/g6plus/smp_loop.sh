#!/bin/bash
# Repeat `fastboot boot IMAGE` on the Moto G6 Plus to measure how often SMP start-up completes.
#   smp_loop.sh IMAGE [COUNT]
# IMAGE must be built with boot-arg sweetsmptest=N (xnu patch 1034): after "8/8 CPUs initialized" the
# kernel resets the SoC, ABL boots Android again and adb comes back. A failed start (panic) stays on the
# screen, adb never comes back and the loop stops there. RAM only: nothing is written to the phone.
set -u
IMG=$1; COUNT=${2:-10}; WAIT=${SMP_LOOP_WAIT:-200}
wait_for() {  # wait_for adb|fastboot seconds
	local end=$((SECONDS + $2))
	while [ $SECONDS -lt $end ]; do
		if [ "$1" = adb ]; then adb devices | grep -qw device && return 0
		else fastboot devices | grep -q fastboot && return 0; fi
		sleep 2
	done
	return 1
}
pass=0
for i in $(seq 1 "$COUNT"); do
	wait_for adb 120 || { echo "run $i: no adb device, stop"; break; }
	sleep 5
	adb reboot bootloader
	wait_for fastboot 60 || { echo "run $i: no fastboot device, stop"; break; }
	t0=$SECONDS
	fastboot boot "$IMG" >/dev/null 2>&1 || echo "run $i: fastboot boot reported an error (the phone may have booted anyway)"
	sleep 15
	if wait_for adb "$WAIT"; then
		pass=$((pass + 1)); echo "run $i: PASS (adb back after $((SECONDS - t0)) s)"
	else
		echo "run $i: FAIL (no adb after $((SECONDS - t0)) s): the phone screen shows the failure"; break
	fi
done
echo "RESULT: $pass/$COUNT complete SMP starts"
