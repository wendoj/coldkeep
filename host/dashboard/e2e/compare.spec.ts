// SPDX-License-Identifier: Apache-2.0
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { expect, test } from "@playwright/test";

const RELAY = "ws://127.0.0.1:7181";
const RUN = join(import.meta.dirname, "../../../docs/evidence/runs/qemu-run1");

/** The expected readouts, read from the recording itself (not hard-coded). */
function summary(mode: "legacy" | "protected") {
  const lines = readFileSync(`${RUN}.${mode}.jsonl`, "utf8").trim().split("\n").map((l) => JSON.parse(l));
  const s = lines.find((o) => o.type === "summary");
  const peak = Math.max(...s.cases.filter((c: { type: string }) => c.type === "chilled").map((c: { peak_product: number }) => c.peak_product));
  return { minutes: s.minutes_above_total.toFixed(1), peak: peak.toFixed(1), applied: String(s.a1_applied), blocked: String(s.a1_blocked), a3: String(s.a3_succeeded) };
}

test("compare view shows readouts computed from the replayed run", async ({ page }) => {
  await page.goto(`/compare?relay=${encodeURIComponent(RELAY)}`);
  for (const mode of ["legacy", "protected"] as const) {
    const card = page.getByTestId(`readouts-${mode}`);
    await expect(card).toHaveAttribute("data-source", "summary");
    const want = summary(mode);
    await expect(page.getByTestId(`${mode}-minutes`)).toContainText(want.minutes);
    await expect(page.getByTestId(`${mode}-peak`)).toContainText(want.peak);
    await expect(page.getByTestId(`${mode}-applied`)).toContainText(want.applied);
    await expect(page.getByTestId(`${mode}-blocked`)).toContainText(want.blocked);
    await expect(page.getByTestId(`${mode}-a3`)).toContainText(want.a3);
  }
  await expect(page.getByTestId("chart-chilled").locator("path").first()).toBeVisible();
  await page.screenshot({ path: "test-results/compare.png", fullPage: true });
});

test("attack console pairs each command with both verdicts", async ({ page }) => {
  await page.goto(`/attacks?relay=${encodeURIComponent(RELAY)}`);
  const rows = page.getByTestId("attack-table").locator("tbody tr");
  await expect(rows).toHaveCount(17);
  await expect(rows.first()).toContainText("start_defrost case=1");
  await expect(rows.first()).toContainText("APPLIED");
  await expect(rows.first()).toContainText("CLAMP");
  await expect(page.getByTestId("a3-table")).toContainText("Operation not permitted");
  await page.screenshot({ path: "test-results/attacks.png", fullPage: true });
});

test("store view and key requests render", async ({ page }) => {
  await page.goto(`/?relay=${encodeURIComponent(RELAY)}`);
  await expect(page.getByTestId("case-1")).toBeVisible();
  await expect(page.locator(".tile")).toHaveCount(8);
  await page.screenshot({ path: "test-results/store.png", fullPage: true });
  await page.getByRole("link", { name: "Key requests" }).click();
  await expect(page.getByTestId("key-requests")).toContainText("disable_alarms");
  await expect(page.getByRole("button", { name: /approve/i })).toHaveCount(0);
});
