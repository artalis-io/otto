/**
 * FuelWise WASM Demo Handlers
 * Auto-loaded by gen_api.py
 */

function fetchFuelWiseHealth() {
    fetchJsonEndpoint(fuelwiseDemo, '/api/v1/health', 'fuelwise-api-v1-health-try-btn', 'fuelwise-api-v1-health-result', 'fuelwise-api-v1-health-status', 'fuelwise-api-v1-health-output', 'Check Health');
}

function fetchFuelWiseStats() {
    fetchJsonEndpoint(fuelwiseDemo, '/api/v1/stats', 'fuelwise-api-v1-stats-try-btn', 'fuelwise-api-v1-stats-result', 'fuelwise-api-v1-stats-status', 'fuelwise-api-v1-stats-output', 'Get Stats');
}

function initFuelWiseDemo() {
    enableBtn('fuelwise-api-v1-health-try-btn', 'Check Health');
    enableBtn('fuelwise-api-v1-stats-try-btn', 'Get Stats');
    const fuelwiseStatus = document.getElementById('fuelwise-api-v1-health-status');
    if (fuelwiseStatus) {
        fuelwiseStatus.textContent = `FuelWise ${fuelwiseDemo.getVersion()} ready`;
        fuelwiseStatus.className = 'demo-status success';
        document.getElementById('fuelwise-api-v1-health-output').classList.add('visible');
    }
}

function handleFuelWiseError(err) {
    disableBtn('fuelwise-api-v1-health-try-btn');
    disableBtn('fuelwise-api-v1-stats-try-btn');
    const fuelwiseStatus = document.getElementById('fuelwise-api-v1-health-status');
    if (fuelwiseStatus) {
        fuelwiseStatus.textContent = 'Failed to load WASM: ' + err.message;
        fuelwiseStatus.className = 'demo-status error';
        document.getElementById('fuelwise-api-v1-health-output').classList.add('visible');
    }
}
