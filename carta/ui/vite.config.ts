import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      // Proxy tile and API requests to the tile server
      '/tiles': {
        target: process.env.TILE_SERVER_URL || 'http://localhost:8081',
        changeOrigin: true,
      },
      '/api': {
        target: process.env.TILE_SERVER_URL || 'http://localhost:8081',
        changeOrigin: true,
      },
    },
  },
})
