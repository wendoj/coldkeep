import { defineConfig } from "vitest/config";

export default defineConfig({
  test: { include: ["shared/test/**/*.test.ts", "relay/test/**/*.test.ts", "attack/test/**/*.test.ts"] },
});
