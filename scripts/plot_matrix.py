#!/usr/bin/env python3
import argparse
import csv
import math
from pathlib import Path


CASE_ORDER = ["semtier_local", "semtier_remote", "malloc_local", "malloc_remote"]
CASE_LABELS = {
    "semtier_local": "SemTier\nlocal",
    "semtier_remote": "SemTier\nremote",
    "malloc_local": "malloc\nlocal",
    "malloc_remote": "malloc\nremote",
}
CASE_COLORS = {
    "semtier_local": "#2f80ed",
    "semtier_remote": "#8ab4f8",
    "malloc_local": "#36a269",
    "malloc_remote": "#d45b5b",
}
NODE0_COLOR = "#2f80ed"
NODE1_COLOR = "#f2994a"


def esc(value):
    return (
        str(value)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def load_rows(path):
    rows = []
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            if row.get("returncode") not in ("0", 0):
                continue
            try:
                rows.append(
                    {
                        "case": row["case"],
                        "rep": int(row["rep"]),
                        "run_s": float(row["run_s"]),
                        "build_s": float(row["build_s"]),
                        "total_s": float(row["total_s"]),
                        "node0_mb": float(row["node0_mb"]),
                        "node1_mb": float(row["node1_mb"]),
                        "placement_ok": str(row.get("placement_ok", "")).lower()
                        == "true",
                    }
                )
            except (KeyError, TypeError, ValueError):
                continue
    if not rows:
        raise SystemExit(f"no valid rows found in {path}")
    return rows


def mean(values):
    return sum(values) / len(values)


def stdev(values):
    if len(values) < 2:
        return 0.0
    avg = mean(values)
    return math.sqrt(sum((value - avg) ** 2 for value in values) / (len(values) - 1))


def summarize(rows):
    by_case = {}
    for row in rows:
        by_case.setdefault(row["case"], []).append(row)

    summaries = []
    for case in CASE_ORDER:
        items = by_case.get(case, [])
        if not items:
            continue
        summaries.append(
            {
                "case": case,
                "label": CASE_LABELS.get(case, case).split("\n"),
                "reps": len(items),
                "run_mean": mean([item["run_s"] for item in items]),
                "run_sd": stdev([item["run_s"] for item in items]),
                "build_mean": mean([item["build_s"] for item in items]),
                "node0_mean": mean([item["node0_mb"] for item in items]),
                "node1_mean": mean([item["node1_mb"] for item in items]),
                "placement_all_ok": all(item["placement_ok"] for item in items),
            }
        )
    return summaries


def multiline_text(x, y, lines, size=12, fill="#202124", weight="400"):
    out = [
        f'<text x="{x}" y="{y}" text-anchor="middle" font-size="{size}" '
        f'font-weight="{weight}" fill="{fill}">'
    ]
    for i, line in enumerate(lines):
        dy = 0 if i == 0 else size * 1.15
        out.append(f'<tspan x="{x}" dy="{dy}">{esc(line)}</tspan>')
    out.append("</text>")
    return "".join(out)


def runtime_panel(items):
    left, top, width, height = 78, 105, 500, 300
    base = top + height
    max_val = max(item["run_mean"] + item["run_sd"] for item in items) * 1.22
    tick_step = 5.0
    while max_val / tick_step > 5:
        tick_step += 5.0
    max_axis = math.ceil(max_val / tick_step) * tick_step
    bar_w, gap = 68, 44
    svg = []

    svg.append(f'<text x="{left}" y="{top - 50}" font-size="18" font-weight="700">Traversal runtime</text>')
    svg.append(f'<text x="{left}" y="{top - 28}" font-size="13" fill="#5f6368">Mean run_s across repetitions; error bars are sample standard deviation.</text>')

    tick = 0.0
    while tick <= max_axis + 1e-9:
        y = base - (tick / max_axis) * height
        svg.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + width}" y2="{y:.1f}" stroke="#e7e9ee"/>')
        svg.append(f'<text x="{left - 12}" y="{y + 4:.1f}" text-anchor="end" font-size="12" fill="#5f6368">{tick:.0f}s</text>')
        tick += tick_step

    for i, item in enumerate(items):
        x = left + 42 + i * (bar_w + gap)
        h = (item["run_mean"] / max_axis) * height
        y = base - h
        color = CASE_COLORS.get(item["case"], "#777")
        svg.append(f'<rect x="{x}" y="{y:.1f}" width="{bar_w}" height="{h:.1f}" fill="{color}" rx="4"/>')

        if item["run_sd"] > 0:
            err = (item["run_sd"] / max_axis) * height
            cx = x + bar_w / 2
            svg.append(f'<line x1="{cx}" y1="{y - err:.1f}" x2="{cx}" y2="{y + err:.1f}" stroke="#333" stroke-width="1.4"/>')
            svg.append(f'<line x1="{cx - 8}" y1="{y - err:.1f}" x2="{cx + 8}" y2="{y - err:.1f}" stroke="#333" stroke-width="1.4"/>')
            svg.append(f'<line x1="{cx - 8}" y1="{y + err:.1f}" x2="{cx + 8}" y2="{y + err:.1f}" stroke="#333" stroke-width="1.4"/>')

        svg.append(f'<text x="{x + bar_w / 2}" y="{y - 11:.1f}" text-anchor="middle" font-size="13" font-weight="700">{item["run_mean"]:.2f}s</text>')
        svg.append(multiline_text(x + bar_w / 2, base + 24, item["label"]))

    ratios = ratio_text(items)
    if ratios:
        svg.append(f'<text x="{left + width - 3}" y="{top - 50}" text-anchor="end" font-size="13" fill="#333">{esc(ratios[0])}</text>')
        if len(ratios) > 1:
            svg.append(f'<text x="{left + width - 3}" y="{top - 31}" text-anchor="end" font-size="13" fill="#333">{esc(ratios[1])}</text>')

    return "\n".join(svg)


def memory_panel(items):
    left, top, width, height = 675, 105, 470, 300
    base = top + height
    max_axis = max(item["node0_mean"] + item["node1_mean"] for item in items) * 1.22
    max_axis = max(50.0, math.ceil(max_axis / 50.0) * 50.0)
    bar_w, gap = 68, 44
    svg = []

    svg.append(f'<text x="{left}" y="{top - 50}" font-size="18" font-weight="700">NUMA residency</text>')
    svg.append(f'<text x="{left}" y="{top - 28}" font-size="13" fill="#5f6368">Mean resident MB from numastat sampled during benchmark sleep.</text>')

    tick = 0.0
    while tick <= max_axis + 1e-9:
        y = base - (tick / max_axis) * height
        svg.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + width}" y2="{y:.1f}" stroke="#e7e9ee"/>')
        svg.append(f'<text x="{left - 12}" y="{y + 4:.1f}" text-anchor="end" font-size="12" fill="#5f6368">{tick:.0f} MB</text>')
        tick += 50.0

    for i, item in enumerate(items):
        x = left + 35 + i * (bar_w + gap)
        h0 = (item["node0_mean"] / max_axis) * height
        h1 = (item["node1_mean"] / max_axis) * height
        y0 = base - h0
        y1 = y0 - h1
        if h0 > 0:
            svg.append(f'<rect x="{x}" y="{y0:.1f}" width="{bar_w}" height="{h0:.1f}" fill="{NODE0_COLOR}" rx="4"/>')
        if h1 > 0:
            svg.append(f'<rect x="{x}" y="{y1:.1f}" width="{bar_w}" height="{h1:.1f}" fill="{NODE1_COLOR}" rx="4"/>')
        total = item["node0_mean"] + item["node1_mean"]
        label_y = base - (total / max_axis) * height - 10
        ok = "ok" if item["placement_all_ok"] else "check"
        svg.append(f'<text x="{x + bar_w / 2}" y="{label_y:.1f}" text-anchor="middle" font-size="13" font-weight="700">{total:.1f} MB</text>')
        svg.append(f'<text x="{x + bar_w / 2}" y="{base + 60}" text-anchor="middle" font-size="11" fill="#5f6368">{ok}</text>')
        svg.append(multiline_text(x + bar_w / 2, base + 24, item["label"]))

    svg.append(f'<rect x="{left + width - 132}" y="{top - 53}" width="13" height="13" fill="{NODE0_COLOR}" rx="2"/>')
    svg.append(f'<text x="{left + width - 113}" y="{top - 42}" font-size="12" fill="#333">Node 0</text>')
    svg.append(f'<rect x="{left + width - 62}" y="{top - 53}" width="13" height="13" fill="{NODE1_COLOR}" rx="2"/>')
    svg.append(f'<text x="{left + width - 43}" y="{top - 42}" font-size="12" fill="#333">Node 1</text>')
    return "\n".join(svg)


def ratio_text(items):
    by_case = {item["case"]: item for item in items}
    out = []
    if "malloc_local" in by_case and "malloc_remote" in by_case:
        ratio = by_case["malloc_remote"]["run_mean"] / by_case["malloc_local"]["run_mean"]
        out.append(f"malloc remote/local: {ratio:.3f}x")
    if "semtier_local" in by_case and "semtier_remote" in by_case:
        ratio = by_case["semtier_remote"]["run_mean"] / by_case["semtier_local"]["run_mean"]
        out.append(f"SemTier remote/local: {ratio:.3f}x")
    return out


def render_svg(items, csv_path, out_path):
    ratios = ratio_text(items)
    footer = " | ".join(ratios) if ratios else "Generated from matrix_results.csv"
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="1220" height="535" viewBox="0 0 1220 535" role="img" aria-label="SemTier NUMA matrix results">
<rect width="1220" height="535" fill="#ffffff"/>
<text x="610" y="38" text-anchor="middle" font-size="23" font-weight="800" fill="#202124">SemTier NUMA Matrix</text>
<text x="610" y="61" text-anchor="middle" font-size="12" fill="#5f6368">source: {esc(csv_path)}</text>
{runtime_panel(items)}
{memory_panel(items)}
<text x="610" y="506" text-anchor="middle" font-size="13" fill="#333">{esc(footer)}</text>
</svg>
'''
    out_path.write_text(svg)


def main():
    parser = argparse.ArgumentParser(description="Plot SemTier matrix_results.csv as SVG.")
    parser.add_argument(
        "csv",
        nargs="?",
        type=Path,
        default=Path("results/collection/matrix_results.csv"),
        help="path to matrix_results.csv",
    )
    parser.add_argument(
        "-o",
        "--out",
        type=Path,
        default=None,
        help="output SVG path",
    )
    args = parser.parse_args()

    csv_path = args.csv.resolve()
    out_path = args.out or csv_path.with_suffix(".svg")
    rows = load_rows(csv_path)
    items = summarize(rows)
    render_svg(items, csv_path, out_path)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
