// SPDX-License-Identifier: Apache-2.0
import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import type { ReactNode } from "react";
import { Shell } from "@/components/Shell";
import { TooltipProvider } from "@/components/ui/tooltip";
import { RelayProvider } from "@/lib/relay";
import "./globals.css";

const sans = Geist({ subsets: ["latin"], variable: "--font-geist" });
const mono = Geist_Mono({ subsets: ["latin"], variable: "--font-geist-mono" });

export const metadata: Metadata = {
  title: "Coldkeep",
  description: "Legacy versus Coldkeep refrigeration stores under the same attack.",
};

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en" className={`${sans.variable} ${mono.variable}`}>
      <body>
        <TooltipProvider delayDuration={150}>
          <RelayProvider>
            <Shell>{children}</Shell>
          </RelayProvider>
        </TooltipProvider>
      </body>
    </html>
  );
}
