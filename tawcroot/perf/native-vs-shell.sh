#!/system/bin/sh
# Push to the device next to run-macro-shell.sh's rootfs and run with sh:
# the same GNU binaries over the same tree, natively through the rootfs
# ld.so (paths prefixed) vs under tawcroot-cand, 3 rounds on cpu9.
# Device-side: same GNU binaries over the same tree, native (via the
# rootfs ld.so, paths prefixed) vs under tawcroot. Pinned to cpu9.
R=/data/local/tmp/tawc-dev/macro/rootfs
T=/data/local/tmp/tawc-dev/macro/tawcroot-cand
L="$R/usr/lib/ld-linux-aarch64.so.1 --library-path $R/usr/lib"
cat > $R/tmp/nv-native.sh <<NAT
find_usr|$L $R/usr/bin/find $R/usr -type f | $L $R/usr/bin/wc -l
ls_lR_usr_lib|$L $R/usr/bin/ls -lR $R/usr/lib | $L $R/usr/bin/wc -l
grep_tree|$L $R/usr/bin/grep -rl nonexistent-needle $R/usr/share $R/etc 2>/dev/null | $L $R/usr/bin/wc -l
tar_usr_share|$L $R/usr/bin/tar cf - $R/usr/share 2>/dev/null | $L $R/usr/bin/wc -c
NAT
cat > $R/tmp/nv-guest.sh <<'GST'
find_usr|find /usr -type f | wc -l
ls_lR_usr_lib|ls -lR /usr/lib | wc -l
grep_tree|grep -rl nonexistent-needle /usr/share /etc 2>/dev/null | wc -l
tar_usr_share|tar cf - /usr/share 2>/dev/null | wc -c
GST
cat > $R/tmp/nv-time.sh <<'TIM'
now() { echo "${EPOCHREALTIME/./}"; }
while IFS="|" read -r name cmd; do
  $SH -c "$cmd" >/dev/null 2>&1 </dev/null || echo "WARN $MODE $name failed" >&2
  ts=()
  for i in 1 2 3 4 5 6 7; do t0=$(now); $SH -c "$cmd" >/dev/null 2>&1 </dev/null; t1=$(now); ts+=($(( (t1-t0)/1000 ))); done
  s=($(printf "%s\n" "${ts[@]}" | sort -n)); echo "NV $MODE $name ${s[3]}"
done < "$1"
TIM
cd $R
echo "CHECK native find count: $($L $R/usr/bin/find $R/usr -type f | $L $R/usr/bin/wc -l)"
for r in 1 2 3; do
  MODE=native SH="$L $R/usr/bin/bash" taskset 200 $L $R/usr/bin/bash $R/tmp/nv-time.sh $R/tmp/nv-native.sh
  taskset 200 $T -r $R -b /dev:/dev -b /proc:/proc -b /sys:/sys -- /usr/bin/env -i PATH=/usr/bin:/bin HOME=/root LANG=C MODE=tawcroot SH=bash /usr/bin/bash /tmp/nv-time.sh /tmp/nv-guest.sh
done
