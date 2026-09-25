"""Renders the storage benchmark results as SVG bar charts for the docs site.

    python3 bench/storage/charts.py
reads  bench/results/storage-postgres18.txt and bench/results/storage-compression.txt
writes docs/assets/*.svg
"""

from __future__ import annotations

import re
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RESULTS = ROOT / "bench" / "results"
OUT = ROOT / "docs" / "assets"

TEXT = "#6e7781"
BAR = "#4493f8"
HIGHLIGHT = "#d29922"


def bar_chart(title: str, unit: str, rows: list[tuple[str, float, str]], highlight: set[str]) -> str:
    label_w, bar_w, row_h, top = 250, 380, 26, 44
    width = label_w + bar_w + 190
    height = top + row_h * len(rows) + 12
    peak = max(v for _, v, _ in rows) or 1
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
        f'font-family="-apple-system, Segoe UI, system-ui, sans-serif" font-size="13" role="img" '
        f'aria-label="{escape(title)}">',
        f'<text x="0" y="18" font-size="15" font-weight="600" fill="{TEXT}">{escape(title)}</text>',
    ]
    for i, (label, value, note) in enumerate(rows):
        y = top + i * row_h
        w = max(2.0, bar_w * value / peak)
        color = HIGHLIGHT if label in highlight else BAR
        parts.append(f'<text x="{label_w - 8}" y="{y + 16}" text-anchor="end" fill="{TEXT}">{escape(label)}</text>')
        parts.append(f'<rect x="{label_w}" y="{y + 3}" width="{w:.1f}" height="{row_h - 8}" rx="3" fill="{color}"/>')
        parts.append(
            f'<text x="{label_w + w + 6:.1f}" y="{y + 16}" fill="{TEXT}">{value:g} {escape(unit)}'
            f'{" · " + escape(note) if note else ""}</text>'
        )
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def postgres_rows() -> list[dict[str, str]]:
    lines = (RESULTS / "storage-postgres18.txt").read_text().splitlines()
    header = [h.strip() for h in lines[1].split("|")]
    return [dict(zip(header, (c.strip() for c in line.split("|")))) for line in lines[2:] if "|" in line]


def compression_rows(section: str) -> list[tuple[str, float, float]]:
    text = (RESULTS / "storage-compression.txt").read_text()
    block = text.split(f"## steady {section}")[1].split("##")[0]
    rows = []
    for line in block.strip().splitlines()[1:]:
        m = re.match(r"(.+?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)$", line)
        if m:
            rows.append((m.group(1).strip(), float(m.group(3)), float(m.group(4))))
    return rows


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    pg = postgres_rows()
    random_like = {r["case"] for r in pg if float(r["leaf_density"].rstrip("%")) < 80}

    (OUT / "pg-index-bytes.svg").write_text(bar_chart(
        "Primary-key index size per row, 5M rows (lower is better)", "B",
        [(r["case"], float(r["index_B/row"]), f'{float(r["leaf_density"].rstrip("%")):.0f}% full') for r in pg],
        random_like,
    ))
    (OUT / "pg-insert-time.svg").write_text(bar_chart(
        "Time to insert 5M rows (lower is better)", "s",
        [(r["case"], round(int(r["insert_ms"]) / 1000, 1), "") for r in pg],
        random_like,
    ))
    (OUT / "pg-wal.svg").write_text(bar_chart(
        "WAL written while inserting 5M rows (lower is better)", "MB",
        [(r["case"], float(r["wal_MB"]), "") for r in pg],
        random_like,
    ))
    for section, name in (("1,000 IDs/s", "compression-1k.svg"), ("100,000 IDs/s", "compression-100k.svg")):
        rows = compression_rows(section)
        (OUT / name).write_text(bar_chart(
            f"Compressed size per ID with zlib, steady {section} (lower is better)", "B",
            [(label, z, f"lzma {x:g} B") for label, z, x in rows],
            {label for label, z, _ in rows if z >= 15},
        ))
    print("wrote", ", ".join(sorted(p.name for p in OUT.glob("*.svg"))))


if __name__ == "__main__":
    main()
