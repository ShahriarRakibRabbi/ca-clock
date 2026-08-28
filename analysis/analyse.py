#!/usr/bin/env python3
"""
Turn a CA-CLOCK sweep CSV into figures and a summary table.

Standard library only: no pandas, no matplotlib, no numpy. Figures are written
as SVG, which every browser and GitHub renders directly. This keeps the project
runnable on a machine with nothing but a plain Python install.

Usage:
    python analysis/analyse.py results/runs.csv [results/device.csv]

Why device.csv matters
----------------------
Operation counts (page-ins, write-backs) are exactly reproducible: the same
config and seed always produce the same counts. Per-run I/O *time* is not -
measured on the development laptop, two runs issuing identical operations
reported total I/O times differing by a factor of two, because SSD/HDD latency
drifts with background activity.

So cost is modelled, not timed: every run's counts are multiplied by ONE shared
pair of latencies from `ca-clock.exe --calibrate`. That keeps the comparison
about the policy instead of about the mood of the disk. Figures say so.
"""

import csv
import os
import sys
from collections import defaultdict

NUMERIC = (
    "total_pages frame_budget alpha max_dirty_skips write_ratio seed "
    "read_accesses write_accesses "
    "faults_total faults_pagein faults_write_protect faults_reference evictions "
    "pagein_reads writeback_writes dirty_spares scan_exhausted "
    "io_read_us_total io_write_us_total ewma_read_us ewma_write_us r_hat "
    "est_io_us wall_clock_us"
).split()


def arm_of(row):
    """Short name for the policy arm this row used: CLOCK, a=x, or k=n."""
    if row.get("policy") == "bounded" or row.get("max_dirty_skips", 0) > 0:
        return "k=%d" % int(row["max_dirty_skips"])
    if row["alpha"] > 0:
        return "a=%.2f" % row["alpha"]
    return "CLOCK"


def arm_sort_key(name):
    """CLOCK first, then probabilistic by alpha, then bounded by k."""
    if name == "CLOCK":
        return (0, 0.0)
    if name.startswith("a="):
        return (1, float(name[2:]))
    return (2, float(name[2:]))


def load_runs(path):
    rows = []
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            for key in NUMERIC:
                if key in row and row[key] not in (None, ""):
                    try:
                        row[key] = float(row[key])
                    except ValueError:
                        row[key] = 0.0
            rows.append(row)
    if not rows:
        raise SystemExit("no rows in %s" % path)
    return rows


def load_calibration(path):
    """Return (read_us, write_us, source_label)."""
    if path and os.path.exists(path):
        with open(path, newline="") as handle:
            for row in csv.DictReader(handle):
                return (
                    float(row["read_us_median"]),
                    float(row["write_us_median"]),
                    "measured (%s samples, durable=%s)" % (row["samples"], row["durable_writes"]),
                )
    return None


def mean(values):
    values = list(values)
    return sum(values) / len(values) if values else 0.0


def group_mean(rows, key_fields, value_field):
    """Average value_field over repetitions (seeds) for each key combination."""
    buckets = defaultdict(list)
    for row in rows:
        key = tuple(row[field] for field in key_fields)
        buckets[key].append(row[value_field])
    return {key: mean(values) for key, values in buckets.items()}


# --------------------------------------------------------------------------
# Minimal SVG line-chart writer
# --------------------------------------------------------------------------

PALETTE = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd", "#ff7f0e", "#17becf"]


def write_line_chart(path, series, x_label, y_label, title, subtitle=""):
    """series: list of (label, [(x, y), ...])."""
    width, height = 760, 460
    left, right, top, bottom = 78, 190, 62, 62
    plot_w = width - left - right
    plot_h = height - top - bottom

    points = [p for _, data in series for p in data]
    if not points:
        return
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    x_min, x_max = min(xs), max(xs)
    y_min, y_max = 0.0, max(ys)
    if x_max == x_min:
        x_max = x_min + 1.0
    if y_max <= 0:
        y_max = 1.0
    y_max *= 1.08

    def sx(x):
        return left + (x - x_min) / (x_max - x_min) * plot_w

    def sy(y):
        return top + plot_h - (y - y_min) / (y_max - y_min) * plot_h

    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="0 0 %d %d" font-family="Segoe UI, Arial, sans-serif">' % (width, height, width, height))
    out.append('<rect width="%d" height="%d" fill="white"/>' % (width, height))
    out.append('<text x="%d" y="26" font-size="16" font-weight="600" fill="#111">%s</text>' % (left, esc(title)))
    if subtitle:
        out.append('<text x="%d" y="46" font-size="11" fill="#666">%s</text>' % (left, esc(subtitle)))

    # gridlines + y ticks
    for i in range(6):
        y_value = y_max * i / 5.0
        y = sy(y_value)
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#e6e6e6"/>' % (left, y, left + plot_w, y))
        out.append('<text x="%.1f" y="%.1f" font-size="11" fill="#555" text-anchor="end">%s</text>'
                   % (left - 8, y + 4, fmt(y_value)))

    # x ticks
    x_values = sorted(set(xs))
    if len(x_values) > 9:
        step = len(x_values) // 8 + 1
        x_values = x_values[::step]
    for x_value in x_values:
        x = sx(x_value)
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#e6e6e6"/>' % (x, top, x, top + plot_h))
        out.append('<text x="%.1f" y="%.1f" font-size="11" fill="#555" text-anchor="middle">%s</text>'
                   % (x, top + plot_h + 18, fmt(x_value)))

    out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="none" stroke="#999"/>' % (left, top, plot_w, plot_h))
    out.append('<text x="%.1f" y="%.1f" font-size="12" fill="#333" text-anchor="middle">%s</text>'
               % (left + plot_w / 2.0, height - 18, esc(x_label)))
    out.append('<text x="18" y="%.1f" font-size="12" fill="#333" text-anchor="middle" '
               'transform="rotate(-90 18 %.1f)">%s</text>' % (top + plot_h / 2.0, top + plot_h / 2.0, esc(y_label)))

    for index, (label, data) in enumerate(series):
        colour = PALETTE[index % len(PALETTE)]
        data = sorted(data)
        path_d = " ".join(("M" if i == 0 else "L") + "%.1f %.1f" % (sx(x), sy(y)) for i, (x, y) in enumerate(data))
        out.append('<path d="%s" fill="none" stroke="%s" stroke-width="2"/>' % (path_d, colour))
        for x, y in data:
            out.append('<circle cx="%.1f" cy="%.1f" r="3" fill="%s"/>' % (sx(x), sy(y), colour))
        legend_y = top + 6 + index * 20
        out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" stroke-width="2"/>'
                   % (left + plot_w + 14, legend_y, left + plot_w + 40, legend_y, colour))
        out.append('<text x="%d" y="%.1f" font-size="11" fill="#333">%s</text>'
                   % (left + plot_w + 46, legend_y + 4, esc(label)))

    out.append("</svg>")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out))
    print("  wrote %s" % path)


def esc(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def fmt(value):
    if abs(value) >= 1000:
        return "%.0f" % value
    if abs(value) >= 10:
        return "%.1f" % value
    return "%.2f" % value


# --------------------------------------------------------------------------


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: python analysis/analyse.py results/runs.csv [results/device.csv]")

    runs_path = sys.argv[1]
    device_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(runs_path), "device.csv")
    out_dir = os.path.dirname(os.path.abspath(__file__))

    rows = load_runs(runs_path)
    calibration = load_calibration(device_path)

    if calibration is None:
        read_us = mean(r["ewma_read_us"] for r in rows if r["ewma_read_us"] > 0)
        write_us = mean(r["ewma_write_us"] for r in rows if r["ewma_write_us"] > 0)
        source = "FALLBACK: mean of per-run EWMA - run 'ca-clock.exe --calibrate' for a proper calibration"
        print("WARNING: %s not found; cost model is approximate." % device_path)
    else:
        read_us, write_us, source = calibration

    r_hat = (write_us / read_us) if read_us > 0 else 0.0
    cost_note = "cost model: 1 page-in = %.0f us, 1 write-back = %.0f us, r_hat = %.2f  [%s]" % (
        read_us, write_us, r_hat, source)
    print("\n%s\n" % cost_note)

    for row in rows:
        row["model_io_ms"] = (row["pagein_reads"] * read_us + row["writeback_writes"] * write_us) / 1000.0
        row["arm"] = arm_of(row)

    arms = sorted({row["arm"] for row in rows}, key=arm_sort_key)
    write_ratios = sorted({row["write_ratio"] for row in rows})

    def rows_for(arm):
        return [r for r in rows if r["arm"] == arm]

    # Figure 1: write-backs vs write ratio, one line per policy arm.
    series = []
    for arm in arms:
        means = group_mean(rows_for(arm), ["write_ratio"], "writeback_writes")
        series.append((arm, [(k[0], v) for k, v in means.items()]))
    write_line_chart(os.path.join(out_dir, "fig1_writebacks.svg"), series,
                     "workload write ratio", "4 KB write-backs issued to disk",
                     "Fig 1  Write-backs vs write ratio",
                     "Lower is better. Measured operation counts, averaged over seeds.")

    # Figure 2: the cost/collapse trade, one point per arm at the heaviest write
    # ratio - the operating point where the two policy families actually diverge.
    heaviest = write_ratios[-1]
    series = []
    for arm in arms:
        subset = [r for r in rows_for(arm) if r["write_ratio"] == heaviest]
        if not subset:
            continue
        evictions = mean(r["evictions"] for r in subset)
        exhausted = mean(r["scan_exhausted"] for r in subset)
        share = 100.0 * exhausted / evictions if evictions > 0 else 0.0
        series.append((arm, [(mean(r["model_io_ms"] for r in subset), share)]))
    write_line_chart(os.path.join(out_dir, "fig2_cost_vs_collapse.svg"), series,
                     "modelled device time (ms) - lower is better",
                     "% of evictions with no eligible victim",
                     "Fig 2  Cost against collapse, at write ratio %.2f" % heaviest,
                     "Bottom-left is the goal: cheap AND still behaving like CLOCK. " + cost_note)

    # Figure 3: how often the scan gave up - the failure mode bounded-k removes.
    series = []
    for write_ratio in write_ratios:
        data = []
        for arm in arms:
            subset = [r for r in rows_for(arm) if r["write_ratio"] == write_ratio]
            if not subset:
                continue
            evictions = mean(r["evictions"] for r in subset)
            exhausted = mean(r["scan_exhausted"] for r in subset)
            data.append((arm_sort_key(arm)[0] * 10 + arm_sort_key(arm)[1],
                         100.0 * exhausted / evictions if evictions > 0 else 0.0))
        series.append(("write_ratio=%.2f" % write_ratio, data))
    write_line_chart(os.path.join(out_dir, "fig3_scan_exhaustion.svg"), series,
                     "policy arm (CLOCK -> alpha -> bounded k)",
                     "% of evictions with no eligible victim",
                     "Fig 3  Where CA-CLOCK stops behaving like CLOCK",
                     "Probabilistic arms collapse under write pressure; bounded-k cannot, by construction.")

    # Summary table
    print("%-8s %-7s %10s %10s %12s %10s %9s" %
          ("arm", "w_ratio", "page-ins", "wr-backs", "spares", "exhaust%", "model_ms"))
    print("-" * 74)
    baseline = {}
    for write_ratio in write_ratios:
        for arm in arms:
            subset = [r for r in rows_for(arm) if r["write_ratio"] == write_ratio]
            if not subset:
                continue
            evictions = mean(r["evictions"] for r in subset)
            exhausted = mean(r["scan_exhausted"] for r in subset)
            model_ms = mean(r["model_io_ms"] for r in subset)
            if arm == "CLOCK":
                baseline[write_ratio] = model_ms
            print("%-8s %-7.2f %10.0f %10.0f %12.0f %9.1f%% %9.0f" % (
                arm, write_ratio,
                mean(r["pagein_reads"] for r in subset),
                mean(r["writeback_writes"] for r in subset),
                mean(r["dirty_spares"] for r in subset),
                (100.0 * exhausted / evictions) if evictions > 0 else 0.0,
                model_ms))
        print()

    print("Change in modelled device time vs CLOCK, negative = better:")
    for write_ratio in write_ratios:
        base = baseline.get(write_ratio)
        if not base:
            continue
        print("  write_ratio=%.2f" % write_ratio)
        for arm in arms:
            if arm == "CLOCK":
                continue
            subset = [r for r in rows_for(arm) if r["write_ratio"] == write_ratio]
            if not subset:
                continue
            model_ms = mean(r["model_io_ms"] for r in subset)
            evictions = mean(r["evictions"] for r in subset)
            exhausted = mean(r["scan_exhausted"] for r in subset)
            share = 100.0 * exhausted / evictions if evictions > 0 else 0.0
            flag = "   <-- COLLAPSED, result not trustworthy" if share > 1.0 else ""
            print("    %-8s %+6.1f%%%s" % (arm, 100.0 * (model_ms - base) / base, flag))

    print("\nanalysis done")


if __name__ == "__main__":
    main()
