/**
 * Locus WASM Demo Handlers
 * Auto-loaded by gen_api.py
 */

async function fetchLocusSearch() {
    if (!locusDemo || !locusDemo.isReady()) return;

    const btn = document.getElementById('locus-api-v1-search-try-btn');
    const result = document.getElementById('locus-api-v1-search-result');
    const status = document.getElementById('locus-api-v1-search-status');
    const output = document.getElementById('locus-api-v1-search-output');

    btn.disabled = true;
    btn.textContent = 'Searching...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const response = await locusDemo.fetch('/api/v1/search?q=Monte%20Carlo&limit=5');
        const elapsed = (performance.now() - startTime).toFixed(1);

        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const data = await response.json();
        const html = formatJsonWithHighlighting(data);
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, html);
        } else {
            result.innerHTML = html;
        }
        const total = data.total || data.results?.length || 0;
        status.textContent = `Search completed in ${elapsed}ms (${total} results)`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Search failed:', err);
        result.textContent = '';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = 'Search';
    }
}

async function fetchLocusAutocomplete() {
    if (!locusDemo || !locusDemo.isReady()) return;

    const btn = document.getElementById('locus-api-v1-autocomplete-try-btn');
    const result = document.getElementById('locus-api-v1-autocomplete-result');
    const status = document.getElementById('locus-api-v1-autocomplete-status');
    const output = document.getElementById('locus-api-v1-autocomplete-output');

    btn.disabled = true;
    btn.textContent = 'Loading...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const response = await locusDemo.fetch('/api/v1/autocomplete?q=Mon&limit=10');
        const elapsed = (performance.now() - startTime).toFixed(1);

        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const data = await response.json();
        const html = formatJsonWithHighlighting(data);
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, html);
        } else {
            result.innerHTML = html;
        }
        const count = Array.isArray(data) ? data.length : 0;
        status.textContent = `Autocomplete in ${elapsed}ms (${count} suggestions)`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Autocomplete failed:', err);
        result.textContent = '';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = 'Autocomplete';
    }
}

async function fetchLocusReverse() {
    if (!locusDemo || !locusDemo.isReady()) return;

    const btn = document.getElementById('locus-api-v1-reverse-try-btn');
    const result = document.getElementById('locus-api-v1-reverse-result');
    const status = document.getElementById('locus-api-v1-reverse-status');
    const output = document.getElementById('locus-api-v1-reverse-output');

    btn.disabled = true;
    btn.textContent = 'Geocoding...';
    output.classList.add('visible');

    const lat = 43.7384;
    const lon = 7.4246;

    try {
        const startTime = performance.now();
        const response = await locusDemo.fetch(`/api/v1/reverse?lat=${lat}&lon=${lon}`);
        const elapsed = (performance.now() - startTime).toFixed(1);

        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const data = await response.json();
        const html = formatJsonWithHighlighting(data);
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, html);
        } else {
            result.innerHTML = html;
        }
        status.textContent = `Reverse geocode in ${elapsed}ms`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Reverse geocoding failed:', err);
        result.textContent = '';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = 'Reverse Geocode';
    }
}

function fetchLocusHealth() {
    fetchJsonEndpoint(locusDemo, '/api/v1/health', 'locus-api-v1-health-try-btn', 'locus-api-v1-health-result', 'locus-api-v1-health-status', 'locus-api-v1-health-output', 'Check Health');
}

function fetchLocusStats() {
    fetchJsonEndpoint(locusDemo, '/api/v1/stats', 'locus-api-v1-stats-try-btn', 'locus-api-v1-stats-result', 'locus-api-v1-stats-status', 'locus-api-v1-stats-output', 'Get Stats');
}

function initLocusDemo() {
    enableBtn('locus-api-v1-search-try-btn', 'Search');
    enableBtn('locus-api-v1-autocomplete-try-btn', 'Autocomplete');
    enableBtn('locus-api-v1-reverse-try-btn', 'Reverse Geocode');
    enableBtn('locus-api-v1-health-try-btn', 'Check Health');
    enableBtn('locus-api-v1-stats-try-btn', 'Get Stats');
    const locusStatus = document.getElementById('locus-api-v1-search-status');
    if (locusStatus) {
        locusStatus.textContent = `Locus ${locusDemo.getVersion()} ready (Monaco: ${locusDemo.getEntityCount()} entities)`;
        locusStatus.className = 'demo-status success';
        document.getElementById('locus-api-v1-search-output').classList.add('visible');
    }
}

function handleLocusError(err) {
    disableBtn('locus-api-v1-search-try-btn');
    disableBtn('locus-api-v1-autocomplete-try-btn');
    disableBtn('locus-api-v1-reverse-try-btn');
    disableBtn('locus-api-v1-health-try-btn');
    disableBtn('locus-api-v1-stats-try-btn');
    const locusStatus = document.getElementById('locus-api-v1-search-status');
    if (locusStatus) {
        locusStatus.textContent = 'Failed to load WASM: ' + err.message;
        locusStatus.className = 'demo-status error';
        document.getElementById('locus-api-v1-search-output').classList.add('visible');
    }
}
