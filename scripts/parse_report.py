#!/usr/bin/env python3
"""Extract estimated II / fMAX / area from an ahls `report` flow output tree.

The exact report layout is compiler-version specific. This walks the output
directory for JSON/HTML/RPT artifacts and pulls the fields the Phase 2 loop
cares about. Writes a compact JSON summary next to the report (or to -o).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


INTERESTING_KEY_RE = re.compile(
    r"(ii|initiation.?interval|fmax|fmax_clock|clock_fmax|frequency|"
    r"alm|alut|aluts|les?|dsp|m20k|ram|bram|area|latency|"
    r"bottleneck|schedule)",
    re.IGNORECASE,
)


def _looks_numeric(v: Any) -> bool:
    return isinstance(v, (int, float)) or (
        isinstance(v, str) and bool(re.fullmatch(r"[-+]?\d+(\.\d+)?(e[-+]?\d+)?", v.strip()))
    )


def _walk_json(obj: Any, prefix: str = "") -> list[tuple[str, Any]]:
    hits: list[tuple[str, Any]] = []
    if isinstance(obj, dict):
        for k, v in obj.items():
            path = f"{prefix}.{k}" if prefix else str(k)
            if INTERESTING_KEY_RE.search(str(k)) and not isinstance(v, (dict, list)):
                hits.append((path, v))
            hits.extend(_walk_json(v, path))
    elif isinstance(obj, list):
        # Keep list-of-dicts (loop tables) but don't explode huge numeric arrays.
        if obj and isinstance(obj[0], dict):
            for i, v in enumerate(obj):
                hits.extend(_walk_json(v, f"{prefix}[{i}]"))
        elif prefix and INTERESTING_KEY_RE.search(prefix.split(".")[-1]):
            hits.append((prefix, obj[:20] if len(obj) > 20 else obj))
    return hits


def collect_json(root: Path) -> dict[str, Any]:
    files = sorted(root.rglob("*.json"))
    # Skip our own summaries if re-run.
    files = [p for p in files if p.name not in {"parsed_report.json", "phase2_summary.json"}]
    by_file: dict[str, Any] = {}
    for p in files:
        try:
            data = json.loads(p.read_text(errors="replace"))
        except (OSError, json.JSONDecodeError):
            continue
        hits = _walk_json(data)
        if hits:
            rel = str(p.relative_to(root))
            by_file[rel] = [{"path": k, "value": v} for k, v in hits]
    return by_file


def collect_text_metrics(root: Path) -> list[dict[str, str]]:
    pats = [
        re.compile(r"Initiation Interval\s*[:=]\s*(\S+)", re.I),
        re.compile(r"\bII\s*[:=]\s*(\d+)", re.I),
        re.compile(r"fMAX[^\n]{0,40}?(\d+(?:\.\d+)?)\s*MHz", re.I),
        re.compile(r"Estimated\s+fMAX\s*[:=]\s*(\S+)", re.I),
        re.compile(r"ALUTs?\s*[:=]\s*([\d,]+)", re.I),
        re.compile(r"ALMs?\s*[:=]\s*([\d,]+)", re.I),
        re.compile(r"DSPs?\s*[:=]\s*([\d,]+)", re.I),
        re.compile(r"M20Ks?\s*[:=]\s*([\d,]+)", re.I),
        re.compile(r"Latency\s*[:=]\s*(\S+)", re.I),
    ]
    hits: list[dict[str, str]] = []
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        if p.suffix.lower() not in {".txt", ".rpt", ".log", ".html", ".csv"}:
            continue
        if p.stat().st_size > 8_000_000:
            continue
        try:
            text = p.read_text(errors="replace")
        except OSError:
            continue
        for pat in pats:
            for m in pat.finditer(text):
                hits.append(
                    {
                        "file": str(p.relative_to(root)),
                        "pattern": pat.pattern,
                        "value": m.group(1),
                    }
                )
    return hits


def list_tree(root: Path, max_entries: int = 200) -> list[str]:
    entries: list[str] = []
    for p in sorted(root.rglob("*")):
        if p.is_file():
            entries.append(str(p.relative_to(root)))
        if len(entries) >= max_entries:
            entries.append("... truncated ...")
            break
    return entries


def summarize(json_hits: dict[str, Any], text_hits: list[dict[str, str]]) -> dict[str, Any]:
    """Pull a few headline numbers if present."""
    headline: dict[str, Any] = {}

    def consider(key: str, value: Any) -> None:
        kl = key.lower()
        if "ii" in kl.split(".")[-1] or "initiation" in kl:
            headline.setdefault("ii_candidates", []).append({key: value})
        if "fmax" in kl or (kl.endswith("frequency") and _looks_numeric(value)):
            headline.setdefault("fmax_candidates", []).append({key: value})
        if re.search(r"alm|alut|dsp|m20k", kl):
            headline.setdefault("area_candidates", []).append({key: value})
        if "latency" in kl:
            headline.setdefault("latency_candidates", []).append({key: value})

    for fname, rows in json_hits.items():
        for row in rows:
            consider(f"{fname}:{row['path']}", row["value"])
    for h in text_hits:
        consider(f"{h['file']}:{h['pattern']}", h["value"])
    return headline


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report_dir", type=Path, help="Directory containing ahls report output")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Where to write parsed_report.json (default: <report_dir>/parsed_report.json)",
    )
    args = parser.parse_args()
    root = args.report_dir.resolve()
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        raise SystemExit(2)

    json_hits = collect_json(root)
    text_hits = collect_text_metrics(root)
    payload = {
        "report_dir": str(root),
        "tree": list_tree(root),
        "headline": summarize(json_hits, text_hits),
        "json_hits": json_hits,
        "text_hits": text_hits[:200],
    }
    out = args.output.resolve() if args.output else root / "parsed_report.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, default=str) + "\n")
    print(f"Wrote {out}")
    print(f"Files under report dir: {len(payload['tree'])}")
    print("Headline:")
    print(json.dumps(payload["headline"], indent=2, default=str))


if __name__ == "__main__":
    main()
