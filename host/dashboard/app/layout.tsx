// SPDX-License-Identifier: Apache-2.0
import type { Metadata } from "next";
import type { ReactNode } from "react";
import { Shell } from "@/components/Shell";
import { RelayProvider } from "@/lib/relay";
import "./globals.css";

export const metadata: Metadata = {
  title: "COLDKEEP",
  description: "Legacy versus COLDKEEP refrigeration stores under the same attack, from recorded or live QNX runs.",
};

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en">
      <body>
        <RelayProvider>
          <Shell>{children}</Shell>
        </RelayProvider>
      </body>
    </html>
  );
}
