#!/bin/bash
# Count instructions newer than ARMv8.0 in a kernel (or any arm64 Mach-O): it must be 0 for SDM630.
#   tools/lseemu/kernel_v80_check.sh path/to/kernel.development.sweet
# Lists the classes the A53 lacks (LSE, RCpc, PAN/UAO system registers, FP16/DotProd/RDM/FCMA/JSCVT,
# PAC, v8.4+ flag ops, SB) with the functions that hold them. Exit 1 if any is found.
# Data in __TEXT that only disassembles as such (not code) is skipped: K256 (SHA-256 round constants).
set -euo pipefail
K="$1"
DIS="$(mktemp)"
trap 'rm -f "$DIS"' EXIT
xcrun llvm-objdump -d --no-show-raw-insn --mattr=+v8.5a,+lse,+rcpc,+rcpc-immo,+fullfp16,+dotprod,+rdm,+complxnum,+jsconv,+pauth,+flagm,+sb "$K" > "$DIS"
RE='^((ld|st)(add|clr|eor|set|smax|smin|umax|umin)|swp|cas|casp)(al|a|l)?(b|h)?$|^ldapr|^ldapur|^stlur|^sqrdml[as]h|^[us]dot$|^fcmla|^fcadd|^fjcvtzs|^(pac|aut|xpac|reta|bra|blra|ldra|ereta)[a-z]*$|^(cfinv|rmif|setf8|setf16|axflag|xaflag|sb)$'
awk -v re="$RE" '/^[0-9a-f]+ <.*>:$/ {fn=$2; next}
  fn == "<K256>:" { next }
  NF >= 2 { m=$2; if (m ~ re) { c[m]++; f[fn]++; n++ } }
  /(msr|mrs).*(PAN|UAO|DIT|SSBS)/ { c["msr/mrs PAN|UAO|DIT|SSBS"]++; f[fn]++; n++ }
  END { for (k in c) printf "  %6d %s\n", c[k], k; nf=0; for (k in f) nf++;
        printf "total %d post-v8.0 instructions in %d functions\n", n+0, nf; exit (n > 0) }' "$DIS"
