import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// `npm run dev` serves the UI on 127.0.0.1:5173 and forwards the API (and its
// WebSockets) to a central service on 127.0.0.1:6088.
const target = process.env.VMWATCHER_DEV_API || "http://127.0.0.1:6088";
export default defineConfig({
  plugins: [react()],
  build: { outDir: "dist", emptyOutDir: true, target: "es2022" },
  server: {
    proxy: {
      "/api": {
        target,
        ws: true,
        // The service only accepts same-origin requests: present its own origin.
        configure: (proxy) => {
          const rewrite = (request) => {
            request.setHeader("Host", new URL(target).host);
            if (request.getHeader("origin")) request.setHeader("Origin", target);
          };
          proxy.on("proxyReq", rewrite);
          proxy.on("proxyReqWs", rewrite);
        },
      },
    },
  },
});
