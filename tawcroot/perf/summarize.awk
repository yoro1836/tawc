# Summarize run-perf TSV rows (backend round benchmark elapsed ops ns_per_op)
# into per-backend mean/median/min/max. Shared by run-perf.sh and
# run-perf-device.sh.
BEGIN { FS = "\t" }
/^#/ || $1 == "backend" { next }
{
    key = $1 SUBSEP $3
    if (!(key in seen)) {
        seen[key] = 1
        keys[++nkeys] = key
    }
    sum[key] += $6
    n = ++count[key]
    vals[key, n] = $6 + 0
}
function sort_vals(key, n,    i, j, tmp) {
    for (i = 1; i <= n; i++) {
        for (j = i + 1; j <= n; j++) {
            if (vals[key, j] < vals[key, i]) {
                tmp = vals[key, i]
                vals[key, i] = vals[key, j]
                vals[key, j] = tmp
            }
        }
    }
}
END {
    print "summary_ns_per_op:"
    print "backend\tbenchmark\trounds\tmean\tmedian\tmin\tmax"
    for (ki = 1; ki <= nkeys; ki++) {
        key = keys[ki]
        n = count[key]
        sort_vals(key, n)
        if (n % 2) {
            median = vals[key, (n + 1) / 2]
        } else {
            median = (vals[key, n / 2] + vals[key, n / 2 + 1]) / 2
        }
        split(key, parts, SUBSEP)
        printf "%s\t%s\t%d\t%.2f\t%.2f\t%.2f\t%.2f\n",
               parts[1], parts[2], n, sum[key] / n, median,
               vals[key, 1], vals[key, n]
    }
}
