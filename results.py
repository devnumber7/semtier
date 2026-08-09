#!/usr/bin/env python3
from pathlib import Path


CASES = [
    {
        "name": "SemTier fast=0",
        "short": "SemTier\nfast=0",
        "run_s": 9.717679003,
        "build_s": 0.971143782,
        "node0_mb": 78.25,
        "node1_mb": 0.00,
        "note": "placed on node0",
        "color": "#2f80ed",
    },
    {
        "name": "SemTier fast=1",
        "short": "SemTier\nfast=1",
        "run_s": 9.716504748,
        "build_s": 0.970924751,
        "node0_mb": 78.25,
        "node1_mb": 0.00,
        "note": "remote attempt stayed node0",
        "color": "#8ab4f8",
    },
    {
        "name": "malloc local",
        "short": "malloc\nlocal",
        "run_s": 10.594243770,
        "build_s": 0.989812898,
        "node0_mb": 154.03,
        "node1_mb": 0.00,
        "note": "membind=0",
        "color": "#36a269",
    },
    {
        "name": "malloc remote",
        "short": "malloc\nremote",
        "run_s": 17.265207915,
        "build_s": 1.311370396,
        "node0_mb": 1.35,
        "node1_mb": 152.68,
        "note": "membind=1",
        "color": "#d45b5b",
    },
]


def esc(text):
    return (
        str(text)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def multiline_text(x, y, text, size=14, anchor="middle", weight="400", fill="#222"):
    lines = text.split("\n")
    out = [f'<text x="{x}" y="{y}" text-anchor="{anchor}" font-size="{size}" font-weight="{weight}" fill="{fill}">']
    for i, line in enumerate(lines):
        dy = 0 if i == 0 else size * 1.15
        out.append(f'<tspan x="{x}" dy="{dy}">{esc(line)}</tspan>')
    out.append("</text>")
    return "".join(out)


def bar_panel_runtime():
    left, top = 70, 95
    width, height = 495, 300
    max_val = 20.0
    base = top + height
    bar_w = 66
    gap = 43

    svg = []
    svg.append(f'<text x="{left}" y="{top - 48}" font-size="18" font-weight="700" fill="#202124">Pointer-chase traversal time</text>')
    svg.append(f'<text x="{left}" y="{top - 24}" font-size="13" fill="#5f6368">Use run_ns: allocation/build time is excluded.</text>')

    for tick in range(0, 21, 5):
        y = base - (tick / max_val) * height
        svg.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + width}" y2="{y:.1f}" stroke="#e7e9ee"/>')
        svg.append(f'<text x="{left - 12}" y="{y + 4:.1f}" text-anchor="end" font-size="12" fill="#5f6368">{tick}s</text>')

    for i, case in enumerate(CASES):
        x = left + 40 + i * (bar_w + gap)
        h = (case["run_s"] / max_val) * height
        y = base - h
        svg.append(f'<rect x="{x}" y="{y:.1f}" width="{bar_w}" height="{h:.1f}" fill="{case["color"]}" rx="4"/>')
        svg.append(f'<text x="{x + bar_w / 2}" y="{y - 9:.1f}" text-anchor="middle" font-size="13" font-weight="700" fill="#202124">{case["run_s"]:.2f}s</text>')
        svg.append(multiline_text(x + bar_w / 2, base + 24, case["short"], size=12))

    svg.append('<path d="M456 136 C440 118, 396 117, 372 184" fill="none" stroke="#5f6368" stroke-width="1.5" marker-end="url(#arrow)"/>')
    svg.append('<text x="389" y="108" font-size="12" fill="#333">malloc remote is 1.63x slower</text>')
    svg.append('<text x="389" y="124" font-size="12" fill="#333">than malloc local</text>')

    svg.append('<path d="M244 202 C229 178, 194 177, 180 240" fill="none" stroke="#5f6368" stroke-width="1.5" marker-end="url(#arrow)"/>')
    svg.append('<text x="195" y="167" font-size="12" fill="#333">SemTier fast=1 did not</text>')
    svg.append('<text x="195" y="183" font-size="12" fill="#333">move memory to node1</text>')
    return "\n".join(svg)


def bar_panel_memory():
    left, top = 650, 95
    width, height = 495, 300
    max_val = 170.0
    base = top + height
    bar_w = 66
    gap = 43
    node0_color = "#2f80ed"
    node1_color = "#f2994a"

    svg = []
    svg.append(f'<text x="{left}" y="{top - 48}" font-size="18" font-weight="700" fill="#202124">NUMA residency from numastat</text>')
    svg.append(f'<text x="{left}" y="{top - 24}" font-size="13" fill="#5f6368">Blue is node0, orange is node1.</text>')

    for tick in range(0, 171, 50):
        y = base - (tick / max_val) * height
        svg.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + width}" y2="{y:.1f}" stroke="#e7e9ee"/>')
        svg.append(f'<text x="{left - 12}" y="{y + 4:.1f}" text-anchor="end" font-size="12" fill="#5f6368">{tick} MB</text>')

    for i, case in enumerate(CASES):
        x = left + 40 + i * (bar_w + gap)
        h0 = (case["node0_mb"] / max_val) * height
        h1 = (case["node1_mb"] / max_val) * height
        y0 = base - h0
        y1 = y0 - h1
        if h0 > 0:
            svg.append(f'<rect x="{x}" y="{y0:.1f}" width="{bar_w}" height="{h0:.1f}" fill="{node0_color}" rx="4"/>')
        if h1 > 0:
            svg.append(f'<rect x="{x}" y="{y1:.1f}" width="{bar_w}" height="{h1:.1f}" fill="{node1_color}" rx="4"/>')
        total = case["node0_mb"] + case["node1_mb"]
        svg.append(f'<text x="{x + bar_w / 2}" y="{base - ((total / max_val) * height) - 9:.1f}" text-anchor="middle" font-size="13" font-weight="700" fill="#202124">{total:.1f}</text>')
        svg.append(multiline_text(x + bar_w / 2, base + 24, case["short"], size=12))

    svg.append(f'<rect x="{left + 312}" y="{top - 52}" width="14" height="14" fill="{node0_color}" rx="2"/>')
    svg.append(f'<text x="{left + 332}" y="{top - 40}" font-size="12" fill="#333">Node 0</text>')
    svg.append(f'<rect x="{left + 385}" y="{top - 52}" width="14" height="14" fill="{node1_color}" rx="2"/>')
    svg.append(f'<text x="{left + 405}" y="{top - 40}" font-size="12" fill="#333">Node 1</text>')
    return "\n".join(svg)


def main():
    out = Path("cloudlab_initial_results.svg")
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="1220" height="535" viewBox="0 0 1220 535" role="img" aria-label="CloudLab SemTier initial NUMA results">
<defs>
  <marker id="arrow" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto" markerUnits="strokeWidth">
    <path d="M0,0 L0,6 L8,3 z" fill="#5f6368" />
  </marker>
</defs>
<rect width="1220" height="535" fill="#ffffff"/>
<text x="610" y="36" text-anchor="middle" font-size="23" font-weight="800" fill="#202124">CloudLab SemTier Initial NUMA Results</text>
{bar_panel_runtime()}
{bar_panel_memory()}
<text x="610" y="500" text-anchor="middle" font-size="13" fill="#333">Main read: malloc local vs remote proves the node has a real NUMA penalty; SemTier currently places serial arenas on node0, but fast=1 did not move them to node1.</text>
</svg>
'''
    out.write_text(svg)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
    