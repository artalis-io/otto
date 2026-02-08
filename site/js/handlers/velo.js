/**
 * Velo WASM Demo Handlers
 * Auto-loaded by build-api-docs.py
 */

async function calculateVeloRoute() {
    if (!veloDemo || !veloDemo.isReady()) return;

    const btn = document.getElementById('velo-route-try-btn');
    const result = document.getElementById('velo-route-result');
    const status = document.getElementById('velo-route-status');
    const output = document.getElementById('velo-route-output');

    btn.disabled = true;
    btn.textContent = 'Calculating...';
    output.classList.add('visible');

    // Read from input fields (IDs match demo_id pattern: velo-route-*)
    const from = {
        lat: parseFloat(document.getElementById('velo-route-from_lat').value) || 43.7384,
        lon: parseFloat(document.getElementById('velo-route-from_lon').value) || 7.4246
    };
    const to = {
        lat: parseFloat(document.getElementById('velo-route-to_lat').value) || 43.7311,
        lon: parseFloat(document.getElementById('velo-route-to_lon').value) || 7.4197
    };
    const profile = document.getElementById('velo-route-profile').value || 'car';
    const mode = document.getElementById('velo-route-mode').value || 'fastest';
    const geometry = document.getElementById('velo-route-geometry').value === 'true';

    try {
        const startTime = performance.now();
        const route = await veloDemo.route(from, to, { profile, mode, geometry });
        const elapsed = (performance.now() - startTime).toFixed(1);

        const html = formatJsonWithHighlighting(route);
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, html);
        } else {
            result.innerHTML = html;
        }
        status.textContent = `Route calculated in ${elapsed}ms (${(route.route.distance).toFixed(0)}m, ${(route.route.duration).toFixed(0)}s)`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Route calculation failed:', err);
        result.textContent = '';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = 'Calculate Route';
    }
}

function fetchVeloHealth() {
    fetchJsonEndpoint(veloDemo, '/api/v1/health', 'velo-health-try-btn', 'velo-health-result', 'velo-health-status', 'velo-health-output', 'Check Health');
}

function fetchVeloStats() {
    fetchJsonEndpoint(veloDemo, '/api/v1/stats', 'velo-stats-try-btn', 'velo-stats-result', 'velo-stats-status', 'velo-stats-output', 'Get Stats');
}

function initVeloDemo() {
    enableBtn('velo-route-try-btn', 'Calculate Route');
    enableBtn('velo-health-try-btn', 'Check Health');
    enableBtn('velo-stats-try-btn', 'Get Stats');
    const veloStatus = document.getElementById('velo-route-status');
    if (veloStatus) {
        veloStatus.textContent = `Velo ${veloDemo.getVersion()} ready (Monaco: ${veloDemo.getNodeCount()} nodes)`;
        veloStatus.className = 'demo-status success';
        document.getElementById('velo-route-output').classList.add('visible');
    }
}

function handleVeloError(err) {
    disableBtn('velo-route-try-btn');
    disableBtn('velo-health-try-btn');
    disableBtn('velo-stats-try-btn');
    const veloStatus = document.getElementById('velo-route-status');
    if (veloStatus) {
        veloStatus.textContent = 'Failed to load WASM: ' + err.message;
        veloStatus.className = 'demo-status error';
        document.getElementById('velo-route-output').classList.add('visible');
    }
}
