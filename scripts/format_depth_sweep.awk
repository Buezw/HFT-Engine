# Formats the SWEEP_ROW lines emitted by bench/benchmark.c (one per
# MAX_ORDERS_PER_LVL compiled by `make depth-sweep`) into a table of
# baseline-vs-optimized speedup by book depth. Invoked by the Makefile;
# not meant to be run standalone, but takes SWEEP_ROW lines on stdin/argv
# either way.
BEGIN {
    printf "%-7s %14s %14s %9s %9s %9s %14s\n",
           "depth", "mean_ns_base", "mean_ns_opt", "mean_x", "p50_x", "p99_x", "rescan_ns/call"
    printf "%-7s %14s %14s %9s %9s %9s %14s\n",
           "-----", "------------", "-----------", "------", "-----", "-----", "--------------"
}
{
    delete kv
    for (i = 2; i <= NF; i++) {
        n = split($i, pair, "=")
        if (n == 2) kv[pair[1]] = pair[2]
    }
    depth = kv["depth"]
    mb = kv["mean_ns_base"]; mo = kv["mean_ns_opt"]
    p50b = kv["p50_ns_base"]; p50o = kv["p50_ns_opt"]
    p99b = kv["p99_ns_base"]; p99o = kv["p99_ns_opt"]
    rescan = kv["rescan_ns_per_call_base"]
    if (depth == "" || mo == 0 || p50o == 0 || p99o == 0) next
    printf "%-7s %14.1f %14.1f %8.2fx %8.2fx %8.2fx %14.1f\n",
           depth, mb, mo, mb / mo, p50b / p50o, p99b / p99o, rescan
}
