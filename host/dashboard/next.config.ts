import type { NextConfig } from "next";

const config: NextConfig = {
  transpilePackages: ["@coldkeep/shared"],
  reactStrictMode: true,
};

export default config;
