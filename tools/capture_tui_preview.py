"""Render the C prototype's own SVG canvas, not a separate HTML mockup.

Optional development tool: requires Playwright and its Chromium installation.
Neither Python nor a browser is required to run the terminal prototype.
"""
import argparse
from pathlib import Path
import subprocess
from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default="build/tui-preview-images")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    out = Path(args.output_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 1064, "height": 822}, device_scale_factor=1)
        # The SVG is generated locally and contains no external references.
        page.route("**/*", lambda route: route.abort())
        for view in ("home", "chat", "models", "computers", "share"):
            svg = subprocess.check_output([str(root / "build/tui-preview"), "--svg", "--view", view], text=True)
            page.set_content('<body style="margin:0;background:#191a1b">' + svg + '</body>')
            page.locator("svg").screenshot(path=str(out / f"{view}.png"))
            print(out / f"{view}.png", flush=True)
        browser.close()


if __name__ == "__main__":
    main()
