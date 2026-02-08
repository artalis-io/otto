/**
 * FuelWise WASM Demo Handlers
 * Auto-loaded by build-api-docs.py
 */

// Sample refueling problem for demo
const SAMPLE_PROBLEM = {
    total_distance: 500,
    tank_capacity: 100,
    current_fuel: 30,
    consumption_mpg: 6.5,
    minimum_fuel: 25,
    stations: [
        { id: 1, distance: 80, price: 3.45 },
        { id: 2, distance: 150, price: 3.29 },
        { id: 3, distance: 250, price: 3.55 },
        { id: 4, distance: 320, price: 3.19 },
        { id: 5, distance: 420, price: 3.39 }
    ]
};

async function solveFuelWiseProblem() {
    if (!fuelwiseDemo || !fuelwiseDemo.isReady()) return;

    const btn = document.getElementById('fuelwise-api-v1-solve-try-btn');
    const result = document.getElementById('fuelwise-api-v1-solve-result');
    const status = document.getElementById('fuelwise-api-v1-solve-status');
    const output = document.getElementById('fuelwise-api-v1-solve-output');

    btn.disabled = true;
    btn.textContent = 'Solving...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const solution = await fuelwiseDemo.solve(SAMPLE_PROBLEM);
        const elapsed = (performance.now() - startTime).toFixed(1);

        const html = formatJsonWithHighlighting(solution);
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, html);
        } else {
            result.innerHTML = html;
        }
        status.textContent = `Solved in ${elapsed}ms (${solution.num_stops} stops, $${solution.total_cost.toFixed(2)} total)`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Solve failed:', err);
        result.textContent = '';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = 'Solve Problem';
    }
}

function fetchFuelWiseHealth() {
    fetchJsonEndpoint(fuelwiseDemo, '/api/v1/health', 'fuelwise-api-v1-health-try-btn', 'fuelwise-api-v1-health-result', 'fuelwise-api-v1-health-status', 'fuelwise-api-v1-health-output', 'Check Health');
}

function fetchFuelWiseStats() {
    fetchJsonEndpoint(fuelwiseDemo, '/api/v1/stats', 'fuelwise-api-v1-stats-try-btn', 'fuelwise-api-v1-stats-result', 'fuelwise-api-v1-stats-status', 'fuelwise-api-v1-stats-output', 'Get Stats');
}

function initFuelWiseDemo() {
    enableBtn('fuelwise-api-v1-solve-try-btn', 'Solve Problem');
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
