#!/bin/bash
# Read, under the phone's own Android (root through Magisk), what its kernel programs for each GPU frequency:
# the GPUCC registers of both GPU PLLs and of the gfx3d clock (0x05065000..), the kgsl power levels, and the
# regulator / clock debug views. Ground truth for programming the PLLs from XNU (Adreno 508, SDM630).
#
#   tools/g6plus/collect-gpuclk.sh [OUT_DIR]        (default backup/evert/gpuclk-<date>, git-ignored)
#
# Nothing is written to storage on the phone. For each power level the script pins kgsl to that level for two
# seconds (min_pwrlevel = max_pwrlevel = N, the same sysfs knobs Android's own thermal code uses) and puts the
# original values back at the end; a reboot would also restore them. Needs adb authorised and `su`.
set -u
cd "$(dirname "$0")/../.."
out=${1:-backup/evert/gpuclk-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out"
sh_() { adb shell "su -c '$1'" 2>&1 | tr -d '\r'; }
K=/sys/class/kgsl/kgsl-3d0
echo "out: $out"
{
	echo "### kgsl"; for f in gpu_model gpu_available_frequencies gpuclk max_gpuclk num_pwrlevels min_pwrlevel max_pwrlevel default_pwrlevel thermal_pwrlevel speed_bin; do printf '%s: ' $f; sh_ "cat $K/$f 2>/dev/null"; done
	echo "### devfreq"; sh_ "cat $K/devfreq/available_frequencies $K/devfreq/governor $K/devfreq/cur_freq 2>/dev/null"
	echo "### which register tool"; sh_ "which devmem devmem2 busybox toybox 2>/dev/null; toybox --list 2>/dev/null | tr \" \" \"\\n\" | grep -x devmem"
} > "$out/kgsl.txt"
cat "$out/kgsl.txt"
omin=$(sh_ "cat $K/min_pwrlevel"); omax=$(sh_ "cat $K/max_pwrlevel")
n=$(sh_ "cat $K/num_pwrlevels"); n=${n:-0}
# register dump: PLL0 at +0x00..0x3c, PLL1 at +0x40..0x7c, gfx3d RCG +0x1070/0x1074, CBCR +0x1098, GDSCs
dump='for a in 0 4 8 c 10 14 18 1c 20 24 28 2c 30 34 38 3c 40 44 48 4c 50 54 58 5c 60 64 68 6c 70 74 78 7c 1004 1020 1070 1074 1090 1094 1098 10b0 10b4 10d0; do printf "%s " 0x050650$(printf %04s $a | tr " " 0 | cut -c3-); devmem 0x0506$(printf %04x $((0x5000 + 0x$a))) 4 2>&1; done'
i=0
while [ "$i" -lt "$n" ]; do
	sh_ "echo $i > $K/max_pwrlevel; echo $i > $K/min_pwrlevel; echo $i > $K/max_pwrlevel" >/dev/null
	sleep 2
	{
		echo "### level $i: gpuclk $(sh_ "cat $K/gpuclk") min $(sh_ "cat $K/min_pwrlevel") max $(sh_ "cat $K/max_pwrlevel")"
		sh_ "$dump"
		echo "# clk debugfs"; sh_ 'for c in gfx3d_clk_src gpu_pll0_pll_out_main gpu_pll1_pll_out_main gpucc_gfx3d_clk; do d=/sys/kernel/debug/clk/$c; [ -d $d ] && echo "$c rate $(cat $d/clk_rate 2>/dev/null) enable $(cat $d/clk_enable_count 2>/dev/null) $(cat $d/clk_measure 2>/dev/null)"; done'
		echo "# regulators"; sh_ 'for r in /sys/kernel/debug/regulator/*s3*level* /sys/kernel/debug/regulator/*gfx* /sys/kernel/debug/regulator/*cx*; do [ -d $r ] && echo "$r: $(cat $r/voltage 2>/dev/null) uv / level $(cat $r/consumers 2>/dev/null | head -3 | tr "\n" ";")"; done 2>/dev/null | head -12'
	} >> "$out/levels.txt"
	i=$((i + 1))
done
sh_ "echo $omax > $K/max_pwrlevel; echo $omin > $K/min_pwrlevel; echo $omax > $K/max_pwrlevel" >/dev/null
echo "restored min_pwrlevel $(sh_ "cat $K/min_pwrlevel") max_pwrlevel $(sh_ "cat $K/max_pwrlevel") (were $omin / $omax)"
sh_ 'd=/sys/kernel/debug/clk/gfx3d_clk_src; ls $d 2>/dev/null; cat $d/clk_list_rate_vdd_level $d/fmax_rates $d/clk_parent 2>/dev/null' > "$out/gfx3d-debugfs.txt"
sh_ 'cat /sys/class/thermal/thermal_zone*/type 2>/dev/null | head -40; echo; cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | head -40' > "$out/thermal.txt"
wc -l "$out"/*.txt
