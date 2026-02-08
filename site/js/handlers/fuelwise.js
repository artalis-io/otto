/**
 * FuelWise WASM Demo Handlers
 * Auto-loaded by build-api-docs.py
 */

// Sample problem matching test case from fuelwise/tests/test_fuelwise.c
// 1000 miles at 10 mpg = 100 gal needed, start with 50, need 10 at end
// So need to purchase 60 gallons across the stations
const SAMPLE_PROBLEM = {
    total_distance: 1000,
    tank_capacity: 100,
    current_fuel: 50,
    consumption_mpg: 10,
    minimum_fuel: 10,
    stations: [
        { id: 1, distance: 200, price: 1.20 },
        { id: 2, distance: 500, price: 1.00 },
        { id: 3, distance: 700, price: 1.30 }
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
        result.innerHTML = `<span class="demo-error">Error: ${err.message}</span>`;
        status.textContent = '';
        status.className = 'demo-status';
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
