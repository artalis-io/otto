import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      // Proxy tile and API requests to the tile server (Carta)
      '/tiles': {
        target: process.env.TILE_SERVER_URL || 'http://localhost:8081',
        changeOrigin: true,
      },
      '/api/v1/stats': {
        target: process.env.TILE_SERVER_URL || 'http://localhost:8081',
        changeOrigin: true,
      },
      '/api/v1/health': {
        target: process.env.TILE_SERVER_URL || 'http://localhost:8081',
        changeOrigin: true,
      },
      // Proxy routing requests to the Velo route server
      '/api/v1/route': {
        target: process.env.ROUTE_SERVER_URL || 'http://localhost:8082',
        changeOrigin: true,
      },
    },
  },
})
