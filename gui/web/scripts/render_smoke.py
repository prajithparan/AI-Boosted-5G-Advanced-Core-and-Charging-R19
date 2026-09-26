#!/usr/bin/env python3
"""Render smoke test for the built GUI (ADR-0420): loads gui/web/dist in headless Chromium with the
BFF's real CSP header and canned /api responses (no network, no ports -- every request is served
by Playwright's router), opens each screen, and fails on any page error, console error, CSP
violation or JSON Forms "No applicable renderer/cell". It proves the bundle renders the derived
schemas under the production CSP; it does not exercise the BFF (the gtest security suite does).

Usage: render_smoke.py <dist_dir>   (needs the `playwright` Python package + its Chromium)
"""

from __future__ import annotations

import json
import mimetypes
import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

CSP = ("default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
       "connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; "
       "frame-ancestors 'none'")
ME = {"userId": "u1", "username": "agent.a", "displayName": "Agent A", "orgUnit": "shop-a",
      "terminal": "terminal-shop-a",
      "permissions": ["customer_order:create", "customer_order:read", "product_offering:read",
                      "product_offering:create", "product_offering_price:read",
                      "product_offering_price:create", "nf_config:read", "nf_config:change"]}
API = {
    "/api/me": ME,
    "/api/tmf620/productOffering": [{"id": "1", "name": "Gold 5G", "lifecycleStatus": "Active"}],
    "/api/tmf620/productOfferingPrice": [],
    "/api/approvals": [],
    "/api/config/product-catalog": {
        "nf": "product-catalog", "editable": True, "apply": "restart",
        "content": {"port": 7785, "db_pool_size": 16, "advertised_ipv4": "127.0.0.1",
                    "metrics_bind_address": "0.0.0.0:9473", "database_url": "********"},
        "versions": [{"version": 1, "createdBy": "(imported from file)", "createdAt": "x",
                      "appliedAt": "x", "comment": "imported from file"}]},
}


def main(dist: Path) -> int:
    problems: list[str] = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page()
        page.on("pageerror", lambda e: problems.append(f"pageerror: {e}"))
        page.on("console", lambda m: problems.append(f"console.{m.type}: {m.text}")
                if m.type in ("error", "warning") else None)

        def handle(route):
            path = route.request.url.split("oam.test", 1)[1].split("?", 1)[0]
            if path.startswith("/api/"):
                body = API.get(path)
                return route.fulfill(status=200 if body is not None else 404,
                                     content_type="application/json",
                                     body=json.dumps(body if body is not None else {}))
            f = dist / ("index.html" if path in ("/", "") else path.lstrip("/"))
            if not f.is_file():
                return route.fulfill(status=404, body="")
            headers = {"content-security-policy": CSP} if f.name == "index.html" else {}
            return route.fulfill(status=200, body=f.read_bytes(), headers=headers,
                                 content_type=mimetypes.guess_type(f.name)[0] or "text/plain")

        page.route("**/*", handle)
        page.goto("https://oam.test/")
        page.wait_for_selector("text=Customer onboarding")
        for label in ("Catalog", "NF configuration", "Onboarding"):
            page.get_by_role("button", name=label).click()
            page.wait_for_timeout(300)
        page.get_by_role("button", name="Catalog").click()
        page.wait_for_selector("text=Propose a new productOffering")
        html = page.content()
        page.get_by_role("button", name="Onboarding").click()
        page.wait_for_selector("text=Submit order")
        html += page.content()
        browser.close()
    for needle in ("No applicable renderer", "No applicable cell"):
        if needle in html:
            problems.append(needle)
    if 'type="password"' in html:
        problems.append("a password-type input exists (browsers would offer to save SIM keys)")
    if 'class="secret"' not in html:
        problems.append("the SIM key inputs are not rendered with the secret cell")
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print("render smoke OK: all screens rendered under the production CSP, no errors")
    return 0


if __name__ == "__main__":
    sys.exit(main(Path(sys.argv[1])))
