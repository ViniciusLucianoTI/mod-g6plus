#!/bin/bash
# Read, under the phone's own Android (root through Magisk), what its kernel uses for each CPU frequency of both
# clusters: the cluster supply regulators (names, voltages, corners) and the clock state. Ground truth for raising
# the CPU clocks from XNU on the SDM630 (the bootloader leaves cpu0-3 at 1670.4 MHz and cpu4-7 at 614.4 MHz).
#
#   tools/g6plus/collect-cpuclk.sh [OUT_DIR]        (default backup/evert/cpuclk-<date>, git-ignored)
#
# Nothing is written to storage on the phone. For each frequency the script pins the cluster to it for two seconds
# (scaling_min_freq = scaling_max_freq, the cpufreq sysfs knobs) and puts the original limits and governor back
# at the end; a reboot would also restore them. Needs adb authorised and `su`.
set -u
cd "$(dirname "$0")/../.."
out=${1:-backup/evert/cpuclk-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out"
sh_() { adb shell "su -c '$1'" 2>&1 | tr -d '\r'; }
echo "out: $out"
sh_ 'ls /sys/kernel/debug/regulator | tr "\n" " "' > "$out/regulators.txt"
regs='for r in /sys/kernel/debug/regulator/*apc* /sys/kernel/debug/regulator/*pwrcl* /sys/kernel/debug/regulator/*perfcl* /sys/kernel/debug/regulator/*s2* /sys/kernel/debug/regulator/*s3* /sys/kernel/debug/regulator/*s1* /sys/kernel/debug/regulator/*apm* /sys/kernel/debug/regulator/*mem_acc*; do [ -d $r ] && echo "$(basename $r): voltage $(cat $r/voltage 2>/dev/null) enable $(cat $r/enable 2>/dev/null) mode $(cat $r/mode 2>/dev/null)"; done 2>/dev/null'
for c in 0 4; do
	d=/sys/devices/system/cpu/cpu$c/cpufreq
	ogov=$(sh_ "cat $d/scaling_governor"); omin=$(sh_ "cat $d/scaling_min_freq"); omax=$(sh_ "cat $d/scaling_max_freq")
	echo "cpu$c: governor $ogov min $omin max $omax" | tee -a "$out/levels.txt"
	for f in $(sh_ "cat $d/scaling_available_frequencies"); do
		sh_ "echo $f > $d/scaling_min_freq; echo $f > $d/scaling_max_freq; echo $f > $d/scaling_min_freq" >/dev/null
		sleep 2
		{
			echo "### cpu$c at $f: cur $(sh_ "cat $d/scaling_cur_freq") clk $(sh_ 'cat /sys/kernel/debug/clk/perfcl_clk/clk_rate /sys/kernel/debug/clk/pwrcl_clk/clk_rate' | tr '\n' ' ')"
			sh_ "$regs"
		} >> "$out/levels.txt"
	done
	sh_ "echo $omax > $d/scaling_max_freq; echo $omin > $d/scaling_min_freq; echo $omax > $d/scaling_max_freq; echo $ogov > $d/scaling_governor" >/dev/null
	echo "cpu$c restored: governor $(sh_ "cat $d/scaling_governor") min $(sh_ "cat $d/scaling_min_freq") max $(sh_ "cat $d/scaling_max_freq")"
done
sh_ 'for d in /sys/kernel/debug/cpr3-regulator/*/*; do [ -d $d ] && echo "== $d" && ls $d | tr "\n" " " && echo; done 2>/dev/null | head -60; ls /sys/kernel/debug/osm /sys/kernel/debug/msm-apm 2>/dev/null' > "$out/cpr-debugfs.txt"
wc -l "$out"/*.txt
