"""Capture an actual C workspace frame from a PTY integration-test log.

Optional documentation tooling (Playwright); never part of the C runtime.
Only understands the shared canvas's full-frame cursor/truecolour vocabulary,
and refuses incomplete frames rather than inventing missing terminal output.
"""
import argparse
import html
from pathlib import Path
import re
import unicodedata
from playwright.sync_api import sync_playwright


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--contains", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--columns", type=int, default=140)
    ap.add_argument("--rows", type=int, default=35)
    args = ap.parse_args()
    frames = Path(args.log).read_text(errors="replace").split("\x1b[H")
    ansi = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")
    chosen = None
    for frame in frames:
        plain = ansi.sub("", frame)
        if args.contains not in plain:
            continue
        grid = [[None] * (args.columns - 1) for _ in range(args.rows)]
        x = y = i = 0
        fg, bg = "#ece6dd", "#191a1b"
        while i < len(frame):
            m = ansi.match(frame, i)
            if m:
                raw, op = m.groups()
                if raw.startswith("?"): break
                values = [int(v or 0) for v in raw.split(";")]
                if op == "H" and len(values) == 2:
                    y, x = values[0] - 1, values[1] - 1
                elif op == "m" and len(values) == 5 and values[1] == 2:
                    color = "#%02x%02x%02x" % tuple(values[2:])
                    if values[0] == 38: fg = color
                    elif values[0] == 48: bg = color
                elif op == "m" and values == [0]:
                    break  # ui_present ends each full frame with a reset
                i = m.end(); continue
            ch = frame[i]; i += 1
            if ch < " ": continue
            width = 0 if unicodedata.combining(ch) else 2 if unicodedata.east_asian_width(ch) in "WF" else 1
            if 0 <= y < args.rows and 0 <= x < args.columns - 1 and width:
                grid[y][x] = (ch, fg, bg)
                if width == 2 and x + 1 < args.columns - 1: grid[y][x + 1] = ("", fg, bg)
            x += width
        if all(cell is not None for row in grid for cell in row):
            chosen = grid
            break
    if chosen is None:
        raise SystemExit("No complete matching C canvas frame in this log")
    cw, ch = 10, 21
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{args.columns*cw}" height="{args.rows*ch}">',
           '<rect width="100%" height="100%" fill="#191a1b"/>']
    for y, row in enumerate(chosen):
        for x, (text, fg, bg) in enumerate(row):
            if bg != "#191a1b": svg.append(f'<rect x="{x*cw}" y="{y*ch}" width="{cw}" height="{ch}" fill="{bg}"/>')
            if text.strip(): svg.append(f'<text x="{x*cw}" y="{y*ch+16}" fill="{fg}" font-family="DejaVu Sans Mono,monospace" font-size="16">{html.escape(text)}</text>')
    svg.append('</svg>')
    out = Path(args.output); out.parent.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": args.columns*cw, "height": args.rows*ch})
        page.route("**/*", lambda route: route.abort())
        page.set_content('<body style="margin:0">' + ''.join(svg) + '</body>')
        page.locator("svg").screenshot(path=str(out))
        browser.close()
    print(out)


if __name__ == "__main__":
    main()
