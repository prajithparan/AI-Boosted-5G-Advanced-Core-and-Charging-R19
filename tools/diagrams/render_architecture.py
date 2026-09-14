#!/usr/bin/env python3
"""Render docs/diagrams/architecture.json to docs/diagrams/architecture{,-dark}.svg.

The JSON is the source of truth: every tile, every service line, every reference point on the
diagram is an entry there, and the entries are meant to be checked against the code (a served
API root, a *_base_url in config, a datastore in compose). The renderer is deliberately dumb --
it lays tiles on a grid the JSON dictates and draws the edges the JSON lists. It has no
knowledge of 5G; it cannot add a box.

Deterministic on purpose: tools/diagrams/check_readme_sync.py re-renders in CI and fails if the
committed SVGs differ, which is what makes a rendered image acceptable as the README's diagram
(ADR-0361 -- the render step is a check, not a memory).

No third-party dependency: stdlib only, so the CI lint job needs nothing installed.
"""
from __future__ import annotations

import json
import pathlib
import sys
from xml.sax.saxutils import escape

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "docs/diagrams/architecture.json"

TILE = 60  # icon tile side
TILE_R = 13
FONT = "Inter, 'Segoe UI', Helvetica, Arial, sans-serif"

THEMES = {
    "light": {
        "bg": "#f7f6f2", "title_text": "#1f1a12",
        "area_fill": "#ffffff", "area_stroke": "#d6d3cd", "area_head": "#6b6560",
        "acronym": "#1f2328", "name": "#3d4249", "ts": "#1f5fbf", "svc": "#6b7280",
        "edge": "#d9742b", "edge_label": "#9a4a12", "halo": "#f7f6f2",
        "planned_fill_a": "#ffffff", "planned_fill_b": "#eeeceb", "planned_stroke": "#b45309",
        "planned_text": "#b45309", "chip_bg": "#e7f0ff", "chip_fg": "#1f5fbf",
        "legend": "#4b5563", "band": "#f0ede8",
    },
    "dark": {
        "bg": "#0f1216", "title_text": "#1f1a12",
        "area_fill": "#161a20", "area_stroke": "#2c333d", "area_head": "#9aa3ad",
        "acronym": "#e6edf3", "name": "#b7c0ca", "ts": "#7fb0ff", "svc": "#8b949e",
        "edge": "#f0904a", "edge_label": "#f6b27a", "halo": "#0f1216",
        "planned_fill_a": "#1d2129", "planned_fill_b": "#161a20", "planned_stroke": "#f59e0b",
        "planned_text": "#f5b544", "chip_bg": "#1c2a44", "chip_fg": "#9cc3ff",
        "legend": "#9aa3ad", "band": "#12161b",
    },
}

# Glyphs: 24x24 viewBox, white strokes/fills. Simple shapes, drawn once, referenced by id.
GLYPHS = {
    "user": '<circle cx="12" cy="8.5" r="3.6" fill="none" stroke="#fff" stroke-width="1.9"/>'
            '<path d="M4.5 20c0-4 3.4-6.3 7.5-6.3s7.5 2.3 7.5 6.3" fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/>',
    "tree": '<rect x="9" y="3" width="6" height="5" rx="1.2" fill="#fff"/><rect x="2.5" y="15" width="6" height="5" rx="1.2" fill="#fff"/>'
            '<rect x="15.5" y="15" width="6" height="5" rx="1.2" fill="#fff"/>'
            '<path d="M12 8v4M5.5 15v-3h13v3" fill="none" stroke="#fff" stroke-width="1.8"/>',
    "db": '<ellipse cx="12" cy="6" rx="7.5" ry="3" fill="none" stroke="#fff" stroke-width="1.9"/>'
          '<path d="M4.5 6v12c0 1.7 3.4 3 7.5 3s7.5-1.3 7.5-3V6M4.5 12c0 1.7 3.4 3 7.5 3s7.5-1.3 7.5-3" fill="none" stroke="#fff" stroke-width="1.9"/>',
    "shield": '<path d="M12 2.8l7.2 2.7v6.2c0 4.6-3.1 8-7.2 9.5-4.1-1.5-7.2-4.9-7.2-9.5V5.5z" fill="none" stroke="#fff" stroke-width="1.9" stroke-linejoin="round"/>'
              '<path d="M8.6 12.2l2.3 2.3 4.6-4.8" fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"/>',
    "sliders": '<path d="M4 7h16M4 12h16M4 17h16" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/>'
               '<circle cx="9" cy="7" r="2.1" fill="#fff"/><circle cx="15" cy="12" r="2.1" fill="#fff"/><circle cx="8" cy="17" r="2.1" fill="#fff"/>',
    "registry": '<rect x="3.5" y="4" width="17" height="16" rx="2" fill="none" stroke="#fff" stroke-width="1.9"/>'
                '<path d="M7 9h10M7 12.5h10M7 16h6" stroke="#fff" stroke-width="1.8" stroke-linecap="round"/>',
    "asterisk": '<path d="M12 3v18M4.2 7.5l15.6 9M4.2 16.5l15.6-9" stroke="#fff" stroke-width="2.1" stroke-linecap="round"/>',
    "api": '<path d="M8 6l-5 6 5 6M16 6l5 6-5 6M14 4l-4 16" fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"/>',
    "hub": '<circle cx="12" cy="12" r="2.6" fill="#fff"/><circle cx="4.5" cy="6" r="2" fill="#fff"/><circle cx="19.5" cy="6" r="2" fill="#fff"/>'
           '<circle cx="4.5" cy="18" r="2" fill="#fff"/><circle cx="19.5" cy="18" r="2" fill="#fff"/>'
           '<path d="M6 7.3l4.2 3.2M18 7.3l-4.2 3.2M6 16.7l4.2-3.2M18 16.7l-4.2-3.2" stroke="#fff" stroke-width="1.6"/>',
    "link": '<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1.2 1.2M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1.2-1.2" fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/>',
    "chat": '<path d="M4 5.5h16v10H9l-4.5 3.5v-3.5H4z" fill="none" stroke="#fff" stroke-width="1.9" stroke-linejoin="round"/>'
            '<path d="M8 9h8M8 12h5" stroke="#fff" stroke-width="1.7" stroke-linecap="round"/>',
    "idcard": '<rect x="3" y="5" width="18" height="14" rx="2" fill="none" stroke="#fff" stroke-width="1.9"/>'
              '<circle cx="8.5" cy="11" r="2.2" fill="#fff"/><path d="M13 9.5h5M13 13h5M6 16h5" stroke="#fff" stroke-width="1.6" stroke-linecap="round"/>',
    "scales": '<path d="M12 4v16M6 20h12M4 9l8-2 8 2M4 9l-2.5 6h5zM20 9l-2.5 6h5z" fill="none" stroke="#fff" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>',
    "pin": '<path d="M12 21s6.5-6.2 6.5-11.2A6.5 6.5 0 0 0 5.5 9.8C5.5 14.8 12 21 12 21z" fill="none" stroke="#fff" stroke-width="1.9" stroke-linejoin="round"/>'
           '<circle cx="12" cy="9.8" r="2.4" fill="#fff"/>',
    "antenna": '<path d="M12 21v-9M8.5 8.5a5 5 0 0 1 7 0M6 6a8.5 8.5 0 0 1 12 0M9 21h6" fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/>'
               '<circle cx="12" cy="11" r="1.8" fill="#fff"/>',
    "phone": '<rect x="7" y="2.5" width="10" height="19" rx="2.2" fill="none" stroke="#fff" stroke-width="1.9"/><circle cx="12" cy="18" r="1.1" fill="#fff"/>',
    "coin": '<circle cx="12" cy="12" r="8.5" fill="none" stroke="#fff" stroke-width="1.9"/>'
            '<path d="M14.6 9.2c-.5-1-1.5-1.5-2.6-1.5-1.6 0-2.7.9-2.7 2 0 2.7 5.6 1.3 5.6 4.1 0 1.2-1.2 2-2.9 2-1.3 0-2.4-.6-2.8-1.6M12 6v12" fill="none" stroke="#fff" stroke-width="1.7" stroke-linecap="round"/>',
    "card": '<rect x="2.5" y="5.5" width="19" height="13" rx="2" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M2.5 10h19" stroke="#fff" stroke-width="2.2"/><path d="M6 15h5" stroke="#fff" stroke-width="1.6" stroke-linecap="round"/>',
    "handset": '<path d="M5.5 3.5h3.6l1.6 4.2-2.1 1.4a11 11 0 0 0 6.3 6.3l1.4-2.1 4.2 1.6v3.6c0 1-.8 1.8-1.8 1.8A15.2 15.2 0 0 1 3.7 5.3c0-1 .8-1.8 1.8-1.8z" fill="none" stroke="#fff" stroke-width="1.8" stroke-linejoin="round"/>',
    "doc": '<path d="M6 3h8l4 4v14H6z" fill="none" stroke="#fff" stroke-width="1.9" stroke-linejoin="round"/><path d="M14 3v4h4M9 12h6M9 15.5h6" stroke="#fff" stroke-width="1.6" stroke-linecap="round"/>',
    "tag": '<path d="M3.5 12.5V4h8.5l8.5 8.5-8.5 8.5z" fill="none" stroke="#fff" stroke-width="1.9" stroke-linejoin="round"/><circle cx="8" cy="8.5" r="1.6" fill="#fff"/>',
    "wallet": '<rect x="3" y="6" width="18" height="13" rx="2" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M3 10h18M15 14.5h3" stroke="#fff" stroke-width="1.8" stroke-linecap="round"/>',
    "users": '<circle cx="9" cy="8.5" r="3" fill="none" stroke="#fff" stroke-width="1.8"/><circle cx="16.5" cy="9.5" r="2.4" fill="none" stroke="#fff" stroke-width="1.8"/>'
             '<path d="M3 19c0-3.3 2.7-5.5 6-5.5s6 2.2 6 5.5M15.5 14.2c2.9 0 5.5 1.8 5.5 4.8" fill="none" stroke="#fff" stroke-width="1.8" stroke-linecap="round"/>',
    "globe": '<circle cx="12" cy="12" r="8.5" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M3.5 12h17M12 3.5c3 3 3 14 0 17M12 3.5c-3 3-3 14 0 17" fill="none" stroke="#fff" stroke-width="1.6"/>',
    "bolt": '<path d="M13.5 2.5L5 13.5h6l-1 8 8.5-11h-6z" fill="#fff" stroke="#fff" stroke-width="1.2" stroke-linejoin="round"/>',
    "bars": '<path d="M4 20h16" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/><rect x="5.5" y="11" width="3.5" height="7" fill="#fff"/><rect x="10.5" y="6" width="3.5" height="12" fill="#fff"/><rect x="15.5" y="9" width="3.5" height="9" fill="#fff"/>',
    "stream": '<path d="M3 7h8M3 12h13M3 17h10" stroke="#fff" stroke-width="2.1" stroke-linecap="round"/><path d="M14 4l6 3-6 3M19 9l2 3-2 3M16 14l5 3-5 3" fill="none" stroke="#fff" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>',
    "grid": '<rect x="3.5" y="3.5" width="17" height="17" rx="1.5" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M3.5 9.5h17M3.5 15h17M9.5 3.5v17M15 3.5v17" stroke="#fff" stroke-width="1.5"/>',
    "flask": '<path d="M9.5 3h5M10 3v6l-5.5 9.5A1.5 1.5 0 0 0 5.8 21h12.4a1.5 1.5 0 0 0 1.3-2.5L14 9V3" fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"/><path d="M8 15h8" stroke="#fff" stroke-width="1.8"/>',
    "chip": '<rect x="6" y="6" width="12" height="12" rx="2" fill="none" stroke="#fff" stroke-width="1.9"/><rect x="9.5" y="9.5" width="5" height="5" fill="#fff"/>'
            '<path d="M9 2.5v3.5M12 2.5v3.5M15 2.5v3.5M9 18v3.5M12 18v3.5M15 18v3.5M2.5 9h3.5M2.5 12h3.5M2.5 15h3.5M18 9h3.5M18 12h3.5M18 15h3.5" stroke="#fff" stroke-width="1.5" stroke-linecap="round"/>',
    "robot": '<rect x="4.5" y="8" width="15" height="11" rx="2.5" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M12 8V4.5M9.5 4.5h5" stroke="#fff" stroke-width="1.8" stroke-linecap="round"/>'
             '<circle cx="9" cy="13" r="1.5" fill="#fff"/><circle cx="15" cy="13" r="1.5" fill="#fff"/><path d="M9 16.5h6" stroke="#fff" stroke-width="1.6" stroke-linecap="round"/>',
    "chart": '<path d="M3.5 20h17M3.5 20V4" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/><path d="M6 15l4-5 3.5 3L19 6" fill="none" stroke="#fff" stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round"/><circle cx="19" cy="6" r="1.6" fill="#fff"/>',
    "funnel": '<path d="M3.5 4.5h17L14 12.5v6.5l-4 2v-8.5z" fill="none" stroke="#fff" stroke-width="1.9" stroke-linejoin="round"/>',
    "archive": '<rect x="3" y="4" width="18" height="5" rx="1.2" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M4.5 9v10.5h15V9M10 13h4" fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/>',
    "swap": '<path d="M4 8h13M13 4l4 4-4 4M20 16H7M11 12l-4 4 4 4" fill="none" stroke="#fff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>',
    "gauge": '<path d="M4 16a8 8 0 1 1 16 0" fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/><path d="M12 16l4.5-6" stroke="#fff" stroke-width="2.1" stroke-linecap="round"/><circle cx="12" cy="16" r="1.8" fill="#fff"/><path d="M6 19.5h12" stroke="#fff" stroke-width="1.6" stroke-linecap="round"/>',
    "eye": '<path d="M2.5 12s3.5-6 9.5-6 9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6z" fill="none" stroke="#fff" stroke-width="1.9" stroke-linejoin="round"/><circle cx="12" cy="12" r="3" fill="none" stroke="#fff" stroke-width="1.9"/>',
    "monitor": '<rect x="3" y="4" width="18" height="12" rx="2" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M9 20h6M12 16v4" stroke="#fff" stroke-width="1.8" stroke-linecap="round"/><path d="M6.5 12l3-3 2.5 2 4-4" fill="none" stroke="#fff" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>',
    "lock": '<rect x="5" y="10.5" width="14" height="10" rx="2" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M8 10.5V7.5a4 4 0 0 1 8 0v3" fill="none" stroke="#fff" stroke-width="1.9"/><circle cx="12" cy="15.5" r="1.5" fill="#fff"/>',
    "key": '<circle cx="8" cy="14" r="4.5" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M11.5 11.5L20 3M17 6l2.5 2.5M14.5 8.5l2.5 2.5" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/>',
    "route": '<circle cx="6" cy="18" r="2.5" fill="none" stroke="#fff" stroke-width="1.9"/><circle cx="18" cy="6" r="2.5" fill="none" stroke="#fff" stroke-width="1.9"/><path d="M8.5 18H14a3 3 0 0 0 3-3v-2a3 3 0 0 0-3-3h-4a3 3 0 0 1-3-3V6" fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/>',
}


def text(x, y, s, size, fill, weight="normal", anchor="middle", italic=False, halo=None, extra=""):
    style = f"font-family:{FONT};font-size:{size}px;font-weight:{weight};"
    if italic:
        style += "font-style:italic;"
    halo_attr = f' paint-order="stroke" stroke="{halo}" stroke-width="3" stroke-linejoin="round"' if halo else ""
    return (f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" fill="{fill}" style="{style}"'
            f'{halo_attr}{extra}>{escape(s)}</text>')


def darken(hex_color: str, factor: float) -> str:
    h = hex_color.lstrip("#")
    r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
    return "#%02x%02x%02x" % tuple(max(0, min(255, int(c * factor))) for c in (r, g, b))


class Layout:
    """Resolves every node's tile centre from its area + (col, row)."""

    def __init__(self, data):
        self.data = data
        self.areas = {a["id"]: a for a in data["areas"]}
        self.nodes = {n["id"]: n for n in data["nodes"]}
        self.pos = {}
        for n in data["nodes"]:
            a = self.areas[n["area"]]
            px, py = a["pitch"]
            ox = a["x"] + a.get("pad_x", 14) + px / 2
            oy = a["y"] + a.get("head", 34) + TILE / 2 + 6
            self.pos[n["id"]] = (ox + n["col"] * px + n.get("dx", 0), oy + n["row"] * py + n.get("dy", 0))

    def anchor(self, nid, side, offset=0.0):
        x, y = self.pos[nid]
        h = TILE / 2
        return {
            "top": (x + offset, y - h), "bottom": (x + offset, y + h),
            "left": (x - h, y + offset), "right": (x + h, y + offset),
        }[side]


def route(layout: Layout, e):
    """Orthogonal polyline from `from` to `to`. Explicit `via` points win; otherwise an elbow."""
    a, b = e["from"], e["to"]
    (ax, ay), (bx, by) = layout.pos[a], layout.pos[b]
    if "via" in e:
        pts = [tuple(p) for p in e["via"]]
        first, last = pts[0], pts[-1]
        s0 = e.get("from_side") or ("bottom" if first[1] > ay else "top" if first[1] < ay else "right" if first[0] > ax else "left")
        s1 = e.get("to_side") or ("top" if last[1] < by else "bottom" if last[1] > by else "left" if last[0] < bx else "right")
        p0 = layout.anchor(a, s0, e.get("from_off", 0))
        p1 = layout.anchor(b, s1, e.get("to_off", 0))
        return [p0] + pts + [p1]
    dx, dy = bx - ax, by - ay
    style = e.get("route", "auto")
    if style == "auto":
        style = "h" if abs(dx) >= abs(dy) else "v"
    if style == "s":  # straight, same row or column
        if abs(dy) < 1:
            return [layout.anchor(a, "right" if dx > 0 else "left", e.get("from_off", 0)),
                    layout.anchor(b, "left" if dx > 0 else "right", e.get("to_off", 0))]
        return [layout.anchor(a, "bottom" if dy > 0 else "top", e.get("from_off", 0)),
                layout.anchor(b, "top" if dy > 0 else "bottom", e.get("to_off", 0))]
    if style == "h":  # leave horizontally, arrive vertically (or straight if aligned)
        p0 = layout.anchor(a, "right" if dx > 0 else "left", e.get("from_off", 0))
        if abs(dy) < TILE:
            p1 = layout.anchor(b, "left" if dx > 0 else "right", e.get("to_off", 0))
            return [p0, (p1[0], p0[1]), p1] if abs(p1[1] - p0[1]) > 0.5 else [p0, p1]
        p1 = layout.anchor(b, "top" if dy > 0 else "bottom", e.get("to_off", 0))
        return [p0, (p1[0], p0[1]), p1]
    # "v": leave vertically, arrive horizontally (or straight if aligned)
    p0 = layout.anchor(a, "bottom" if dy > 0 else "top", e.get("from_off", 0))
    if abs(dx) < TILE:
        p1 = layout.anchor(b, "top" if dy > 0 else "bottom", e.get("to_off", 0))
        return [p0, (p0[0], p1[1]), p1] if abs(p1[0] - p0[0]) > 0.5 else [p0, p1]
    p1 = layout.anchor(b, "left" if dx > 0 else "right", e.get("to_off", 0))
    return [p0, (p0[0], p1[1]), p1]


def point_along(pts, frac):
    segs = [((pts[i][0], pts[i][1]), (pts[i + 1][0], pts[i + 1][1])) for i in range(len(pts) - 1)]
    lens = [abs(q[0] - p[0]) + abs(q[1] - p[1]) for p, q in segs]
    total = sum(lens) or 1
    target = frac * total
    run = 0.0
    for (p, q), ln in zip(segs, lens):
        if run + ln >= target:
            t = (target - run) / ln if ln else 0
            return (p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t, abs(q[0] - p[0]) >= abs(q[1] - p[1]))
        run += ln
    return (pts[-1][0], pts[-1][1], True)


def render(data, theme_name: str) -> str:
    t = THEMES[theme_name]
    W, H = data["canvas"]
    lay = Layout(data)
    out = []
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
               f'role="img" aria-labelledby="title">')
    out.append(f'<title id="title">{escape(data["title"])}</title>')
    # defs: glyph symbols, tile gradients per colour, arrowhead
    out.append("<defs>")
    for gid, body in GLYPHS.items():
        out.append(f'<symbol id="g-{gid}" viewBox="0 0 24 24">{body}</symbol>')
    colours = sorted({n["color"] for n in data["nodes"] if not n.get("planned")})
    for c in colours:
        cid = c.lstrip("#")
        out.append(f'<linearGradient id="grad-{cid}" x1="0" y1="0" x2="0" y2="1">'
                   f'<stop offset="0" stop-color="{c}"/><stop offset="1" stop-color="{darken(c, 0.72)}"/></linearGradient>')
    out.append(f'<linearGradient id="grad-planned" x1="0" y1="0" x2="0" y2="1">'
               f'<stop offset="0" stop-color="{t["planned_fill_a"]}"/><stop offset="1" stop-color="{t["planned_fill_b"]}"/></linearGradient>')
    out.append('<linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1">'
               '<stop offset="0" stop-color="#fff" stop-opacity="0.32"/><stop offset="1" stop-color="#fff" stop-opacity="0.02"/></linearGradient>')
    out.append('<linearGradient id="titlebar" x1="0" y1="0" x2="1" y2="0">'
               '<stop offset="0" stop-color="#f9b234"/><stop offset="1" stop-color="#e8891a"/></linearGradient>')
    out.append(f'<marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
               f'<path d="M0 0L10 5L0 10z" fill="{t["edge"]}"/></marker>')
    out.append(f'<marker id="arrow-planned" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
               f'<path d="M0 0L10 5L0 10z" fill="{t["planned_stroke"]}"/></marker>')
    out.append("</defs>")

    out.append(f'<rect width="{W}" height="{H}" fill="{t["bg"]}"/>')
    # title bar
    out.append(f'<rect x="0" y="0" width="{W}" height="58" fill="url(#titlebar)"/>')
    out.append(text(24, 39, data["title"], 27, t["title_text"], "700", "start"))
    out.append(text(W - 24, 27, data["title_right"][0], 11.5, t["title_text"], "600", "end"))
    out.append(text(W - 24, 45, data["title_right"][1], 11.5, t["title_text"], "500", "end"))
    out.append(text(24, 80, data["subtitle"], 12.5, t["legend"], "500", "start"))

    # areas
    for a in data["areas"]:
        out.append(f'<rect x="{a["x"]}" y="{a["y"]}" width="{a["w"]}" height="{a["h"]}" rx="10" '
                   f'fill="{t["area_fill"]}" stroke="{t["area_stroke"]}" stroke-width="1"/>')
        out.append(f'<rect x="{a["x"]}" y="{a["y"]}" width="{a["w"]}" height="5" rx="2.5" fill="{a["color"]}"/>')
        out.append(text(a["x"] + 14, a["y"] + 24, a["title"].upper(), 11.5, t["area_head"], "700", "start",
                        extra=' letter-spacing="1.2"'))
        if a.get("note"):
            for i, line in enumerate(a["note"]):
                out.append(text(a["x"] + a["w"] - 14, a["y"] + 21 + i * 12, line, 9.5, t["svc"], "500", "end", italic=True))

    for tb in data.get("texts", []):
        for i, line in enumerate(tb["lines"]):
            out.append(text(tb["x"], tb["y"] + i * (tb["size"] + 3), line, tb["size"], t["svc"], "500", "start"))

    # edges (under the tiles)
    for e in data["edges"]:
        pts = route(lay, e)
        planned = e.get("planned", False)
        stroke = t["planned_stroke"] if planned else t["edge"]
        dash = ' stroke-dasharray="6 4"' if planned else ""
        d = "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in pts)
        arrow = "arrow-planned" if planned else "arrow"
        both = ' marker-start="url(#%s)"' % arrow if e.get("both") else ""
        out.append(f'<path d="{d}" fill="none" stroke="{stroke}" stroke-width="1.7" stroke-linejoin="round"{dash} '
                   f'marker-end="url(#{arrow})"{both}/>')
    # edge labels (above the lines, halo'd)
    for e in data["edges"]:
        if not e.get("label"):
            continue
        pts = route(lay, e)
        x, y, horizontal = point_along(pts, e.get("label_at", 0.5))
        lx, ly = x + e.get("label_dx", 0), y + e.get("label_dy", (-5 if horizontal else 0))
        anchor = "middle" if horizontal else "start"
        if not horizontal:
            lx += 6
        fill = t["planned_text"] if e.get("planned") else t["edge_label"]
        lines = e["label"] if isinstance(e["label"], list) else [e["label"]]
        for i, line in enumerate(lines):
            # first line above the line; further lines (service, TS) stack below it
            yy = ly if i == 0 else ly + 11 + (i - 1) * 9.5
            out.append(text(lx, yy, line, 9.2 if i == 0 else 7.8, fill, "700" if i == 0 else "500",
                            anchor, italic=i > 0, halo=t["halo"]))

    # tiles
    for n in data["nodes"]:
        x, y = lay.pos[n["id"]]
        left, top = x - TILE / 2, y - TILE / 2
        planned = n.get("planned", False)
        if planned:
            out.append(f'<rect x="{left}" y="{top}" width="{TILE}" height="{TILE}" rx="{TILE_R}" fill="url(#grad-planned)" '
                       f'stroke="{t["planned_stroke"]}" stroke-width="1.6" stroke-dasharray="5 3"/>')
            glyph_fill = n["color"]
            out.append(f'<g transform="translate({left + 14},{top + 14})" style="filter:none">'
                       f'<use href="#g-{n["glyph"]}" width="32" height="32" style="color:{glyph_fill}"/></g>')
            # the glyph symbols are white; tint planned ones by overlaying at reduced opacity
            out.append(f'<rect x="{left + 1}" y="{top + 1}" width="{TILE - 2}" height="{TILE - 2}" rx="{TILE_R - 1}" fill="{n["color"]}" opacity="0.28"/>')
        else:
            cid = n["color"].lstrip("#")
            out.append(f'<rect x="{left}" y="{top}" width="{TILE}" height="{TILE}" rx="{TILE_R}" fill="url(#grad-{cid})" '
                       f'stroke="{darken(n["color"], 0.6)}" stroke-width="1"/>')
            out.append(f'<rect x="{left + 2}" y="{top + 2}" width="{TILE - 4}" height="{TILE / 2 - 2}" rx="{TILE_R - 2}" fill="url(#gloss)"/>')
            out.append(f'<use href="#g-{n["glyph"]}" x="{left + 14}" y="{top + 14}" width="32" height="32"/>')
        # chip (top-right): service count for built NFs, "planned" for the rest
        chip = "planned" if planned else n.get("chip")
        if chip:
            cw = 7 + 5.6 * len(chip)
            cx, cy = x + TILE / 2 - cw / 2 + 6, top - 7
            bg = t["planned_stroke"] if planned else t["chip_bg"]
            fg = "#ffffff" if planned else t["chip_fg"]
            out.append(f'<rect x="{cx - cw / 2:.1f}" y="{cy - 7}" width="{cw:.1f}" height="14" rx="7" fill="{bg}"/>')
            out.append(text(cx, cy + 3.5, chip, 8.5, fg, "700"))
        # labels
        ly = top + TILE + 15
        halo = t["area_fill"]
        out.append(text(x, ly, n["acronym"], 13, t["planned_text"] if planned else t["acronym"], "700", halo=halo))
        ly += 12
        for line in n.get("name", []):
            out.append(text(x, ly, line, 9.8, t["name"], "500", halo=halo))
            ly += 11
        for line in n.get("ts", []):
            out.append(text(x, ly, line, 8.8, t["ts"], "600", halo=halo))
            ly += 10.5
        for line in n.get("svc", []):
            out.append(text(x, ly, line, 8.3, t["svc"], "500", italic=True, halo=halo))
            ly += 10

    # legend
    lg = data["legend"]
    ly = H - 34
    x = 24
    out.append(f'<rect x="{x}" y="{ly - 10}" width="34" height="18" rx="5" fill="url(#grad-1f5fbf)" stroke="{darken("#1f5fbf", 0.6)}"/>'
               if "#1f5fbf" in colours else
               f'<rect x="{x}" y="{ly - 10}" width="34" height="18" rx="5" fill="{colours[0] if colours else "#888"}"/>')
    out.append(text(x + 42, ly + 4, lg["built"], 10.5, t["legend"], "500", "start"))
    x += 42 + 6.2 * len(lg["built"]) + 30
    out.append(f'<rect x="{x}" y="{ly - 10}" width="34" height="18" rx="5" fill="url(#grad-planned)" stroke="{t["planned_stroke"]}" stroke-dasharray="5 3" stroke-width="1.5"/>')
    out.append(text(x + 42, ly + 4, lg["planned"], 10.5, t["legend"], "500", "start"))
    x += 42 + 6.2 * len(lg["planned"]) + 30
    out.append(f'<path d="M{x} {ly} L{x + 40} {ly}" stroke="{t["edge"]}" stroke-width="1.7" marker-end="url(#arrow)"/>')
    out.append(text(x + 48, ly + 4, lg["edge"], 10.5, t["legend"], "500", "start"))
    x += 48 + 6.2 * len(lg["edge"]) + 30
    out.append(f'<rect x="{x}" y="{ly - 8}" width="30" height="14" rx="7" fill="{t["chip_bg"]}"/>')
    out.append(text(x + 15, ly + 2.5, "9 svc", 8.5, t["chip_fg"], "700"))
    out.append(text(x + 38, ly + 4, lg["chip"], 10.5, t["legend"], "500", "start"))
    out.append(text(W - 24, ly + 4, lg["footer"], 10, t["svc"], "500", "end", italic=True))
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main() -> int:
    data = json.loads(SRC.read_text())
    check = "--check" in sys.argv
    ok = True
    for theme, name in (("light", "architecture.svg"), ("dark", "architecture-dark.svg")):
        svg = render(data, theme)
        target = ROOT / "docs/diagrams" / name
        if check:
            if not target.exists() or target.read_text() != svg:
                print(f"{target.relative_to(ROOT)} is stale: re-run tools/diagrams/render_architecture.py")
                ok = False
        else:
            target.write_text(svg)
            print(f"wrote {target.relative_to(ROOT)} ({len(svg)} bytes)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
