# OTTO Landing Page

Zero-dependency static landing page for the OTTO platform.

## Stack

- **Single HTML file** with inline CSS
- **No JavaScript** required
- **No build step** - just serve static files
- **System monospace fonts** - no external font loading

## Serve Locally

```bash
make serve
# or
python3 -m http.server 8000
```

Open http://localhost:8000

## Deploy

Copy `index.html` to any static file host:
- GitHub Pages
- Netlify
- Vercel
- nginx
- S3 + CloudFront

## Design

- Dark theme with monospace typography
- Mirrors OTTO's zero-dependency philosophy
- Mobile responsive
- Fast loading (~15KB total)
