/**
 * Carta WASM Demo Handlers
 * Auto-loaded by gen_api.py
 */

async function generateCartaTile() {
    if (!cartaDemo || !cartaDemo.isReady()) return;

    const btn = document.getElementById('carta-try-btn');
    const img = document.getElementById('carta-tile-img');
    const status = document.getElementById('carta-status');
    const output = document.getElementById('carta-output');

    const z = parseInt(document.getElementById('carta-z').value) || 14;
    const x = parseInt(document.getElementById('carta-x').value) || 8529;
    const y = parseInt(document.getElementById('carta-y').value) || 5974;

    btn.disabled = true;
    btn.textContent = 'Generating...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const dataUrl = await cartaDemo.getTileDataURL(z, x, y);
        const elapsed = (performance.now() - startTime).toFixed(1);

        img.src = dataUrl;
        img.style.display = 'block';
        status.textContent = `Tile ${z}/${x}/${y}.png generated in ${elapsed}ms`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Tile generation failed:', err);
        img.style.display = 'none';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = 'Generate Tile';
    }
}

async function fetchTileJSON() {
    if (!cartaDemo || !cartaDemo.isReady()) return;

    const btn = document.getElementById('tilejson-try-btn');
    const result = document.getElementById('tilejson-result');
    const status = document.getElementById('tilejson-status');
    const output = document.getElementById('tilejson-output');

    btn.disabled = true;
    btn.textContent = 'Fetching...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const tileJson = await cartaDemo.getTileJSON();
        const elapsed = (performance.now() - startTime).toFixed(1);

        const html = formatJsonWithHighlighting(tileJson);
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, html);
        } else {
            result.innerHTML = html;
        }
        status.textContent = `Fetched in ${elapsed}ms`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('TileJSON fetch failed:', err);
        result.textContent = '';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = 'Fetch TileJSON';
    }
}

function fetchCartaHealth() {
    fetchJsonEndpoint(cartaDemo, '/api/v1/health', 'health-try-btn', 'health-result', 'health-status', 'health-output', 'Check Health');
}

function fetchCartaStats() {
    fetchJsonEndpoint(cartaDemo, '/api/v1/stats', 'stats-try-btn', 'stats-result', 'stats-status', 'stats-output', 'Get Stats');
}

function initCartaDemo() {
    const pngBtn = document.getElementById('carta-try-btn');
    const pngStatus = document.getElementById('carta-status');
    pngBtn.textContent = 'Generate Tile';
    pngBtn.disabled = false;
    pngStatus.textContent = `Carta ${cartaDemo.getVersion()} ready (Monaco PBF: ${(cartaDemo.getPBFSize() / 1024).toFixed(0)} KB)`;
    pngStatus.className = 'demo-status success';
    document.getElementById('carta-output').classList.add('visible');
    enableBtn('tilejson-try-btn', 'Fetch TileJSON');
    enableBtn('health-try-btn', 'Check Health');
    enableBtn('stats-try-btn', 'Get Stats');
}

function handleCartaError(err) {
    const pngBtn = document.getElementById('carta-try-btn');
    const pngStatus = document.getElementById('carta-status');
    pngBtn.textContent = 'WASM unavailable';
    pngStatus.textContent = 'Failed to load WASM module: ' + err.message;
    pngStatus.className = 'demo-status error';
    document.getElementById('carta-output').classList.add('visible');
    disableBtn('tilejson-try-btn');
    disableBtn('health-try-btn');
    disableBtn('stats-try-btn');
}
