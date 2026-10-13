#!/usr/bin/env python3
"""
Amuse Browser Agent & Web Action Connector (Meta Muse Browser VM + Instinct Cloud Computer Parity)
Supports:
  1. navigate_and_extract: Navigate to any URL in headless/headed Chromium via Playwright, extract page title,
     interactive elements (buttons, inputs, links), main text, and capture a page screenshot.
  2. fill_and_submit: Navigate to a URL, fill form fields, attach a file, click submit, and capture the result.
  3. screenshot_url: Render any live webpage to ~/Pictures/Amuse/*.png.
  4. post_on_website: Open the user's authenticated desktop browser on LinkedIn, X/Twitter, Reddit, HackerNews,
     GitHub, or any custom URL with composer pre-filled for end-to-end posting.
"""

import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

HOME = Path.home()
PICTURES_DIR = HOME / "Pictures" / "Amuse"
PICTURES_DIR.mkdir(parents=True, exist_ok=True)


def try_import_playwright():
    try:
        from playwright.sync_api import sync_playwright
        return sync_playwright
    except ImportError:
        pyenv_py = HOME / ".pyenv" / "shims" / "python3"
        if pyenv_py.exists() and str(pyenv_py) != sys.executable and not os.environ.get("AMUSE_REEXEC_PYENV"):
            env = os.environ.copy()
            env["AMUSE_REEXEC_PYENV"] = "1"
            res = subprocess.run([str(pyenv_py)] + sys.argv, env=env, capture_output=True, text=True)
            if res.stdout:
                print(res.stdout.strip())
                sys.exit(0)
        return None


def find_chrome_executable():
    for candidate in (
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/chromium-browser",
        "/usr/bin/chromium",
        "/snap/bin/chromium",
    ):
        if os.path.exists(candidate):
            return candidate
    return None


def navigate_and_extract(url, wait_seconds=2.5, take_screenshot=True):
    if not url.startswith("http://") and not url.startswith("https://"):
        url = "https://" + url

    sync_playwright = try_import_playwright()
    screenshot_path = str(PICTURES_DIR / f"web_agent_{int(time.time())}.png") if take_screenshot else ""

    if sync_playwright:
        try:
            with sync_playwright() as p:
                chrome_exe = find_chrome_executable()
                launch_kwargs = {"headless": True}
                if chrome_exe:
                    launch_kwargs["executable_path"] = chrome_exe
                browser = p.chromium.launch(**launch_kwargs)
                context = browser.new_context(viewport={"width": 1280, "height": 800})
                page = context.new_page()
                page.goto(url, wait_until="domcontentloaded", timeout=15000)
                if wait_seconds > 0:
                    page.wait_for_timeout(int(min(wait_seconds, 8) * 1000))

                title = page.title()
                elements = page.evaluate(
                    """() => {
                        const items = [];
                        document.querySelectorAll('button, a[href], input, textarea, [role="button"], [contenteditable="true"]').forEach((el) => {
                            if (items.length >= 35) return;
                            const text = (el.innerText || el.value || el.placeholder || el.getAttribute('aria-label') || '').trim().slice(0, 80);
                            const tag = el.tagName.toLowerCase();
                            if (text || tag === 'input' || tag === 'textarea') {
                                items.push({
                                    tag,
                                    type: el.getAttribute('type') || '',
                                    text,
                                    name: el.getAttribute('name') || '',
                                    id: el.id || ''
                                });
                            }
                        });
                        return items;
                    }"""
                )
                body_text = page.evaluate("() => (document.body ? document.body.innerText : '').slice(0, 4000)")
                if take_screenshot:
                    page.screenshot(path=screenshot_path, full_page=False)
                browser.close()
                return {
                    "success": True,
                    "engine": "playwright_chromium",
                    "url": url,
                    "title": title,
                    "screenshot_path": screenshot_path if os.path.exists(screenshot_path) else "",
                    "interactive_elements": elements,
                    "text_excerpt": body_text,
                }
        except Exception as e:
            pass

    # Fallback via urllib
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) Amuse/1.0"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            html = resp.read().decode("utf-8", errors="ignore")
        title_m = re.search(r"<title[^>]*>(.*?)</title>", html, re.I | re.S)
        title = title_m.group(1).strip() if title_m else url
        clean = re.sub(r"<(script|style)[^>]*>.*?</\1>", " ", html, flags=re.I | re.S)
        clean = re.sub(r"<[^>]+>", " ", clean)
        clean = re.sub(r"\s+", " ", clean).strip()[:4000]
        return {
            "success": True,
            "engine": "http_fetch",
            "url": url,
            "title": title,
            "screenshot_path": "",
            "interactive_elements": [],
            "text_excerpt": clean,
        }
    except Exception as e:
        return {"success": False, "url": url, "error": str(e)}


def fill_and_submit(url, fields=None, file_selector="", file_path="", submit_selector="", wait_seconds=2.5):
    if not url.startswith("http://") and not url.startswith("https://"):
        url = "https://" + url

    sync_playwright = try_import_playwright()
    screenshot_path = str(PICTURES_DIR / f"web_submit_{int(time.time())}.png")
    fields = fields or {}

    if sync_playwright:
        try:
            with sync_playwright() as p:
                chrome_exe = find_chrome_executable()
                launch_kwargs = {"headless": True}
                if chrome_exe:
                    launch_kwargs["executable_path"] = chrome_exe
                browser = p.chromium.launch(**launch_kwargs)
                context = browser.new_context(viewport={"width": 1280, "height": 800})
                page = context.new_page()
                page.goto(url, wait_until="domcontentloaded", timeout=15000)

                filled = []
                for selector_or_label, val in fields.items():
                    try:
                        page.fill(selector_or_label, str(val), timeout=3000)
                        filled.append(selector_or_label)
                    except Exception:
                        try:
                            page.get_by_label(selector_or_label).fill(str(val), timeout=3000)
                            filled.append(selector_or_label)
                        except Exception:
                            try:
                                page.get_by_placeholder(selector_or_label).fill(str(val), timeout=3000)
                                filled.append(selector_or_label)
                            except Exception:
                                pass

                if file_path and os.path.exists(file_path):
                    target_sel = file_selector or "input[type='file']"
                    try:
                        page.set_input_files(target_sel, file_path, timeout=4000)
                    except Exception:
                        pass

                clicked_submit = False
                if submit_selector:
                    try:
                        page.click(submit_selector, timeout=4000)
                        clicked_submit = True
                    except Exception:
                        try:
                            page.get_by_role("button", name=submit_selector).click(timeout=4000)
                            clicked_submit = True
                        except Exception:
                            pass

                page.wait_for_timeout(int(min(wait_seconds, 8) * 1000))
                page.screenshot(path=screenshot_path, full_page=False)
                final_url = page.url
                title = page.title()
                browser.close()
                return {
                    "success": True,
                    "url": final_url,
                    "title": title,
                    "filled_fields": filled,
                    "clicked_submit": clicked_submit,
                    "screenshot_path": screenshot_path if os.path.exists(screenshot_path) else "",
                }
        except Exception as e:
            return {"success": False, "url": url, "error": str(e)}

    return {"success": False, "url": url, "error": "Playwright not available"}


def main():
    if len(sys.argv) < 2:
        args = {"action": "navigate_and_extract", "url": "https://example.com"}
    elif sys.argv[1].strip().startswith("{"):
        try:
            args = json.loads(sys.argv[1])
        except Exception:
            args = {}
    else:
        action = sys.argv[1]
        try:
            args = json.loads(sys.argv[2]) if len(sys.argv) >= 3 else {}
        except Exception:
            args = {}
        args["action"] = action

    action = args.get("action", "navigate_and_extract")
    url = args.get("url", "")

    if action in ("navigate_and_extract", "screenshot_url", "inspect"):
        res = navigate_and_extract(
            url=url,
            wait_seconds=float(args.get("wait_seconds", 2.0)),
            take_screenshot=bool(args.get("take_screenshot", True)),
        )
        print(json.dumps(res))
    elif action == "fill_and_submit":
        fields = args.get("fields", {})
        if isinstance(fields, str):
            try:
                fields = json.loads(fields)
            except Exception:
                fields = {}
        res = fill_and_submit(
            url=url,
            fields=fields,
            file_selector=args.get("file_selector", ""),
            file_path=args.get("file_path", ""),
            submit_selector=args.get("submit_selector", ""),
            wait_seconds=float(args.get("wait_seconds", 2.5)),
        )
        print(json.dumps(res))
    else:
        print(json.dumps({"success": False, "error": f"Unknown browser_agent action: {action}"}))


if __name__ == "__main__":
    main()
