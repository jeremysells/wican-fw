// SPDX-License-Identifier: GPL-3.0-or-later
import assert from "node:assert/strict";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
const moduleRoot = process.env.WICAN_WEB_TEST_MODULES;
const { chromium } = moduleRoot
  ? await import(pathToFileURL(resolve(moduleRoot, "playwright/index.mjs")))
  : await import("playwright");
const preview = process.env.WICAN_PAN_PREVIEW_OUTPUT || "/tmp/wican-pan-ui-preview.html";
const browser = await chromium.launch({ headless: true, args: ["--no-sandbox"] });
try {
  for (const size of [{ width: 1440, height: 1000 }, { width: 390, height: 844 }]) {
    const page = await browser.newPage({ viewport: size });
    const errors = [];
    page.on("pageerror", error => errors.push(error.message));
    await page.goto(pathToFileURL(preview).href + "#/settings/bluetooth");
    const panel = page.locator("[data-usb-pan]");
    try { await panel.waitFor({ timeout: 8000 }); }
    catch (error) {
      process.stderr.write(JSON.stringify(await page.evaluate(() => ({ hash: location.hash,
        text: document.querySelector("#view")?.textContent,
        pages: typeof PAGES, state: typeof conn !== "undefined" ? conn.state : "missing",
        mock: !!window.__MOCK_SETTINGS__, pan: window.__panStatus,
        panels: document.querySelectorAll("[data-usb-pan]").length }))) + "\n");
      process.stderr.write(JSON.stringify(errors) + "\n");
      throw error;
    }
    await page.locator('[data-pan-action="pair"]').click();
    await page.locator("[data-pan-code]").waitFor();
    const overflowing = await panel.evaluate((element, width) => {
      return [...element.querySelectorAll("button,input,select,[data-pan-code]")]
        .filter(control => control.getClientRects().length)
        .filter(control => {
          const bounds = control.getBoundingClientRect();
          return bounds.left < -1 || bounds.right > width + 1;
        }).map(control => control.textContent || control.getAttribute("type"));
    }, size.width);
    assert.deepEqual(overflowing, [], "PAN controls fit the viewport");
    const outsideNotice = await page.locator("[data-pan-pairing] .note").evaluate(element => {
      const parent = element.getBoundingClientRect();
      return [...element.children].some(child => {
        const bounds = child.getBoundingClientRect();
        return bounds.left < parent.left - 1 || bounds.right > parent.right + 1;
      });
    });
    assert.equal(outsideNotice, false, "Pairing controls fit their notice");
    assert.deepEqual(errors, [], "No browser errors");
    await page.locator("[data-pan-code]").scrollIntoViewIfNeeded();
    await page.screenshot({ path: `/tmp/wican-pan-${size.width}.png`, fullPage: true });
    await page.getByRole("button", { name: "Confirm code", exact: true }).click();
    assert.match(await page.locator("[data-pan-status]").textContent(), /192\.168\.44\.2/);
    await page.close();
  }
  process.stdout.write("USB PAN desktop/mobile layout: PASS\n");
} finally { await browser.close(); }
