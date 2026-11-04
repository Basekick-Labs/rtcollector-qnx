#!/usr/bin/env python3
"""Pools rtc_loop histograms per scenario and prints the overhead table.

    rtc-bench-report.py RESULTS_DIR [--csv out.csv]
    rtc-bench-report.py --self-test

RESULTS_DIR holds NN-SCENARIO/ directories from rtc-batch.sh (one or more
batches). Histograms of the same scenario are summed bucket by bucket, so the
percentiles are over every sample, not medians of runs. The acceptance rule
(plan v2, fixed before measuring): misses and overruns equal S0's; pooled
response p99.9 and max within the S0a/S0b difference + 5 us; the positive
control must exceed that threshold or the verdict is void.
"""
import glob
import math
import os
import re
import sys

FINE, COARSE = 2000, 980
NBUCKETS = FINE + COARSE + 1
ORDER = ["S0a", "S0b", "Cplus", "S1", "S2", "S2d", "S3", "S3m", "S3h", "S4", "S4o", "S3q", "S6r", "S6b", "Sneg"]
JUDGED = ["S1", "S2", "S2d", "S3", "S3m", "S3h", "S4", "S4o", "S3q", "S6r", "S6b"]


def lower(i):
    if i < FINE:
        return i * 1000
    if i < FINE + COARSE:
        return (FINE + (i - FINE) * 100) * 1000
    return (FINE + COARSE * 100) * 1000


def upper(i):
    return lower(i + 1) if i + 1 < NBUCKETS else None


def bucket_of(ns):
    """Index of the bucket whose lower edge is ns (as written by rtc_loop)."""
    us = ns // 1000
    if us < FINE:
        return us
    c = (us - FINE) // 100
    return FINE + c if c < COARSE else NBUCKETS - 1


class Hist:
    def __init__(self):
        self.b = [0] * NBUCKETS
        self.n = 0
        self.max = 0

    def merge(self, o):
        for i, v in enumerate(o.b):
            self.b[i] += v
        self.n += o.n
        self.max = max(self.max, o.max)

    def quantile(self, q):
        """Upper edge of the bucket holding the ceil(q*n)-th value, capped at max."""
        if not self.n:
            return 0
        rank = max(1, math.ceil(q * self.n - 1e-9))
        seen = 0
        for i, v in enumerate(self.b):
            seen += v
            if seen >= rank:
                up = upper(i)
                return self.max if up is None or up > self.max else up
        return self.max

    def at_or_above(self, ns):
        return sum(v for i, v in enumerate(self.b) if lower(i) >= ns)


def read_hist(path):
    hs = {}
    for line in open(path):
        name, a, b = line.strip().split(",")
        if name.endswith("_max"):
            h = hs.setdefault(name[:-4], Hist())
            h.max, h.n = int(a), int(b)
        else:
            hs.setdefault(name, Hist()).b[bucket_of(int(a))] += int(b)
    return hs


def read_summary(path):
    kv = {}
    for tok in open(path).read().split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            kv[k] = v
    return kv


def collect(root):
    runs = {}
    for d in sorted(glob.glob(os.path.join(root, "**", "[0-9][0-9]-*"), recursive=True)):
        m = re.match(r"\d+-(\w+)$", os.path.basename(d))
        if not m or not os.path.isfile(os.path.join(d, "hist.csv")):
            continue
        s = m.group(1)
        summ = read_summary(os.path.join(d, "summary"))
        if summ.get("rc") != "0":
            print(f"warning: {d}: rtc_loop rc={summ.get('rc')}", file=sys.stderr)
        r = runs.setdefault(s, {"hist": {}, "misses": 0, "overruns": 0, "skipped": 0, "pub_err": 0, "runs": 0,
                               "run_max": []})
        hs = read_hist(os.path.join(d, "hist.csv"))
        for k, h in hs.items():
            r["hist"].setdefault(k, Hist()).merge(h)
        r["misses"] += int(summ.get("misses", 0))
        r["overruns"] += int(summ.get("overruns", 0))
        r["skipped"] += int(summ.get("skipped", 0))
        r["pub_err"] += int(summ.get("publish_errors", 0))
        r["run_max"].append(hs.get("resp", Hist()).max)
        r["runs"] += 1
    return runs


def us(ns):
    return f"{ns / 1000:.1f}"


def report(runs, csv_path=None):
    base = Hist()
    for s in ("S0a", "S0b"):
        if s in runs:
            base.merge(runs[s]["hist"]["resp"])
    noise_p999 = noise_max = None
    if "S0a" in runs and "S0b" in runs:
        a, b = runs["S0a"]["hist"]["resp"], runs["S0b"]["hist"]["resp"]
        noise_p999 = abs(a.quantile(0.999) - b.quantile(0.999))
        noise_max = abs(a.max - b.max)
    lines = ["| Scenario | runs | cycles | resp p50 | p99 | p99.9 | p99.99 | max | ≥50 µs | misses | overruns | Δp99.9 | Δmax | publish p99 |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    rows = []
    base_runs = sum(runs[s]["runs"] for s in ("S0a", "S0b") if s in runs)
    base_misses = sum(runs[s]["misses"] for s in ("S0a", "S0b") if s in runs)
    base_overruns = sum(runs[s]["overruns"] for s in ("S0a", "S0b") if s in runs)
    verdicts = []
    for s in [x for x in ORDER if x in runs] + sorted(x for x in runs if x not in ORDER):
        r = runs[s]
        h = r["hist"].get("resp", Hist())
        p = r["hist"].get("pub", Hist())
        d999 = h.quantile(0.999) - base.quantile(0.999) if base.n else 0
        dmax = h.max - base.max if base.n else 0
        lines.append(f"| {s} | {r['runs']} | {h.n} | {us(h.quantile(.5))} | {us(h.quantile(.99))} | "
                     f"{us(h.quantile(.999))} | {us(h.quantile(.9999))} | {us(h.max)} | {h.at_or_above(50000)} | "
                     f"{r['misses']} | {r['overruns']} | {us(d999)} | {us(dmax)} | {us(p.quantile(.99)) if p.n else '-'} |")
        rows.append([s, r["runs"], h.n, h.quantile(.5), h.quantile(.99), h.quantile(.999), h.quantile(.9999), h.max,
                     h.at_or_above(50000), r["misses"], r["overruns"], d999, dmax, p.quantile(.99) if p.n else ""])
        if s in JUDGED and noise_p999 is not None:
            # misses and overruns no more often than in S0 (per run), tails within the noise floor
            per_run = r["runs"] / base_runs if base_runs else 0
            ok = (r["misses"] <= base_misses * per_run and r["overruns"] <= base_overruns * per_run and
                  d999 <= noise_p999 + 5000 and dmax <= noise_max + 5000)
            verdicts.append((s, ok, d999, dmax))
    out = "\n".join(lines) + "\n"
    if noise_p999 is not None:
        thr999, thrmax = noise_p999 + 5000, noise_max + 5000
        out += f"\nNoise floor (S0a vs S0b): p99.9 {us(noise_p999)} µs, max {us(noise_max)} µs; thresholds p99.9 {us(thr999)} µs, max {us(thrmax)} µs.\n"
        if "Cplus" in runs:
            c = runs["Cplus"]["hist"]["resp"]
            sens = c.max - base.max > thrmax or c.quantile(0.999) - base.quantile(0.999) > thr999
            out += f"Positive control: Δmax {us(c.max - base.max)} µs → {'detected' if sens else 'NOT detected: the verdicts below are void'}.\n"
        for s, ok, d999, dmax in verdicts:
            out += f"- {s}: {'pass' if ok else 'FAIL'} (Δp99.9 {us(d999)} µs, Δmax {us(dmax)} µs)\n"
    if csv_path:
        with open(csv_path, "w") as f:
            f.write("scenario,runs,cycles,p50_ns,p99_ns,p999_ns,p9999_ns,max_ns,ge50us,misses,overruns,d_p999_ns,d_max_ns,pub_p99_ns\n")
            for row in rows:
                f.write(",".join(str(x) for x in row) + "\n")
    return out


def self_test():
    assert lower(0) == 0 and upper(0) == 1000 and lower(2000) == 2000000 and upper(2000) == 2100000
    for ns in (0, 999, 1000, 1999999, 2000000, 2099999, 99999999, 100000000, 10**12):
        i = bucket_of(ns)
        assert lower(i) <= ns and (upper(i) is None or ns < upper(i)), ns
    h = Hist()
    for i in range(1, 1001):
        h.b[bucket_of(i * 1000 + 500)] += 1
    h.n, h.max = 1000, 1000500
    assert h.quantile(0.5) == 501000, h.quantile(0.5)
    assert h.quantile(0.99) == 991000
    assert h.quantile(1.0) == 1000500
    assert h.at_or_above(991000) == 10
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        def run(name, resp, misses=0):
            os.makedirs(os.path.join(d, name))
            with open(os.path.join(d, name, "hist.csv"), "w") as f:
                for v, c in resp:
                    f.write(f"resp,{lower(bucket_of(v))},{c}\n")
                f.write(f"resp_max,{max(v for v, _ in resp)},{sum(c for _, c in resp)}\n")
            with open(os.path.join(d, name, "summary"), "w") as f:
                f.write(f"rtc_loop misses={misses} overruns=0 skipped=0 publish_errors=0\nrc=0\n")
        run("01-S0a", [(10000, 999), (12000, 1)])
        run("02-S0b", [(10000, 999), (13000, 1)])
        run("03-S1", [(10000, 999), (14000, 1)])
        run("04-S3", [(10000, 998), (90000, 2)])
        run("05-Cplus", [(10000, 990), (30000, 10)])
        run("06-S1", [(10000, 1000)])
        r = collect(d)
        assert r["S1"]["runs"] == 2 and r["S1"]["hist"]["resp"].n == 2000
        out = report(r)
        assert "- S1: pass" in out, out
        assert "- S3: FAIL" in out, out
        assert "Positive control" in out and "detected" in out and "NOT" not in out, out
    print("rtc-bench-report self-test ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        sys.exit(0)
    if not sys.argv[1:]:
        print(__doc__)
        sys.exit(2)
    csv = sys.argv[sys.argv.index("--csv") + 1] if "--csv" in sys.argv else None
    runs = collect(sys.argv[1])
    if not runs:
        print("no results under " + sys.argv[1], file=sys.stderr)
        sys.exit(1)
    sys.stdout.write(report(runs, csv))
