// SPDX-License-Identifier: Apache-2.0
//
// Smoke test of the dashboard in replay mode: the relay replays a recorded QEMU
// run as fast as possible and the dashboard must show readouts computed from it.
import { defineConfig } from "@playwright/test";

const RUN = "../../docs/evidence/runs/qemu-run1";

export default defineConfig({
  testDir: "e2e",
  timeout: 60_000,
  use: { baseURL: "http://127.0.0.1:3100" },
  webServer: [
    {
      command: `npx tsx ../relay/src/main.ts --replay ${RUN}.legacy.jsonl --replay ${RUN}.protected.jsonl --rate max --port 7181`,
      url: "http://127.0.0.1:7181",
      reuseExistingServer: false,
    },
    {
      command: "npx next start -p 3100 -H 127.0.0.1",
      url: "http://127.0.0.1:3100",
      reuseExistingServer: false,
      timeout: 120_000,
    },
  ],
});
