import path from "path"
import tailwindcss from "@tailwindcss/vite"
import react from "@vitejs/plugin-react"
import { defineConfig } from "vite"

export default defineConfig({
  // Relative asset URLs so the same build works served from the device root and
  // from the relay's /devices/<id>/ subpath — no build-time prefix, no per-device
  // build. What keeps this safe is that routing lives in the HASH (use-route.ts):
  // the document URL stays the mount point, so "./assets/…" resolves against the
  // right directory at any route. A path-based router would break that, which is
  // exactly what it did before.
  base: "./",
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: {
      "@": path.resolve(__dirname, "./src"),
    },
  },
  build: {
    outDir: "../www",
    emptyOutDir: true,
    rollupOptions: {
      output: {
        manualChunks: undefined,
      },
    },
  },
})
