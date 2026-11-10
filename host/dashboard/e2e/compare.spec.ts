// SPDX-License-Identifier: Apache-2.0
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { expect, test } from "@playwright/test";

const REPLAY = "ws://127.0.0.1:7181";
const SIM = "ws://127.0.0.1:7182";
const RUN = join(import.meta.dirname, "../../../docs/evidence/runs/qemu-run1");

/** The expected readouts, read from the recording itself (not hard-coded). */
function summary(mode: "legacy" | "protected") {
  const lines = readFileSync(`${RUN}.${mode}.jsonl`, "utf8").trim().split("\n").map((l) => JSON.parse(l));
  const s = lines.find((o) => o.type === "summary");
  const peak = Math.max(...s.cases.filter((c: { type: string }) => c.type === "chilled").map((c: { peak_product: number }) => c.peak_product));
  return { minutes: s.minutes_above_total.toFixed(1), peak: peak.toFixed(1), applied: String(s.a1_applied), blocked: String(s.a1_blocked), a3: String(s.a3_succeeded) };
}

test("overview shows readouts computed from the replayed run", async ({ page }) => {
  await page.goto(`/?relay=${encodeURIComponent(REPLAY)}`);
  const outcome = page.getByTestId("outcome");
  for (const mode of ["legacy", "protected"] as const) {
    await expect(outcome).toHaveAttribute(`data-source-${mode}`, "summary");
    const want = summary(mode);
    await expect(page.getByTestId(`${mode}-minutes`)).toHaveText(want.minutes);
    await expect(page.getByTestId(`${mode}-peak`)).toHaveText(want.peak);
    await expect(page.getByTestId(`${mode}-applied`)).toHaveText(want.applied);
  }
  await expect(page.getByTestId("connection")).toContainText("Recorded on QEMU");
  await expect(page.getByTestId("chart-chilled").locator(".recharts-line-curve").first()).toBeVisible();
  await expect(page.getByTestId("top-rules").locator("tbody tr")).not.toHaveCount(0);
  await page.screenshot({ path: "test-results/overview.png", fullPage: true });
});

test("commands table pairs each command with both verdicts", async ({ page }) => {
  await page.goto(`/commands?relay=${encodeURIComponent(REPLAY)}`);
  for (const mode of ["legacy", "protected"] as const) {
    const want = summary(mode);
    await expect(page.getByTestId(`${mode}-applied`)).toHaveText(want.applied);
    await expect(page.getByTestId(`${mode}-blocked`)).toHaveText(want.blocked);
  }
  await expect(page.getByTestId("command-count")).toHaveText("17");
  const rows = page.getByTestId("attack-table").locator("tbody tr");
  await expect(rows).toHaveCount(10);
  await expect(rows.first()).toContainText("Start defrost");
  await expect(rows.first()).toContainText("case 1");
  await expect(rows.first()).toContainText("Applied");
  await expect(rows.first()).toContainText("Clamped");
  await page.getByRole("tab", { name: /Needs key/ }).click();
  await expect(rows).toHaveCount(1);
  await expect(rows.first()).toContainText("Disable alarms");
  await expect(page.getByTestId("a3-table")).toContainText("Operation not permitted");
  await page.screenshot({ path: "test-results/commands.png", fullPage: true });
});

test("cases and key requests render", async ({ page }) => {
  await page.goto(`/cases?relay=${encodeURIComponent(REPLAY)}`);
  await expect(page.getByTestId("case-protected-1")).toBeVisible();
  await expect(page.locator("[data-testid^=case-protected-]")).toHaveCount(8);
  await expect(page.locator("[data-testid^=case-legacy-]")).toHaveCount(8);
  await page.screenshot({ path: "test-results/cases.png", fullPage: true });
  await page.getByRole("link", { name: "Key requests" }).click();
  await expect(page.getByTestId("key-requests")).toContainText("disable_alarms");
  await expect(page.getByRole("button", { name: /approve/i })).toHaveCount(0);
});

test("an attack can be run from the dashboard in the host simulator", async ({ page }) => {
  await page.goto(`/?relay=${encodeURIComponent(SIM)}`);
  await expect(page.getByTestId("connection")).toContainText("Host simulator");
  await page.getByRole("button", { name: "Run attack" }).click();
  for (const name of [/A2/, /A3/, /A4/]) await page.getByRole("checkbox", { name }).click();
  await page.getByRole("combobox", { name: "Run speed" }).click();
  await page.getByRole("option", { name: "Instant" }).click();
  await page.getByRole("button", { name: "Run A1" }).click();
  await expect(page.getByTestId("attack-log")).toContainText("[A1]");
  await expect(page.getByTestId("attack-log")).toContainText("coldkeep: CLAMP");
  await page.screenshot({ path: "test-results/run-panel.png" });
  await page.keyboard.press("Escape");
  await page.getByRole("link", { name: "Commands" }).click();
  await expect(page.getByTestId("command-count")).toHaveText("17", { timeout: 30000 });
  await expect(page.getByTestId("a3-table")).toHaveCount(0);
});

test("control view is read-only against a recording", async ({ page }) => {
  await page.goto(`/control?relay=${encodeURIComponent(REPLAY)}`);
  await expect(page.getByTestId("control-offline")).toContainText("Replay and simulator runs are read-only");
  await expect(page.getByTestId("control-offline")).toContainText("--mgmt");
  await expect(page.getByTestId("control-send")).toBeDisabled();
  await expect(page.getByTestId("control-preset-plus1")).toBeDisabled();
  await expect(page.getByTestId("control-recent")).toHaveCount(0);
  await expect(page.getByRole("button", { name: /approve/i })).toHaveCount(0);
  await page.screenshot({ path: "test-results/control-readonly.png", fullPage: true });
});
