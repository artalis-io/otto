/**
 * Surge WASM Demo Handlers
 * Auto-loaded by build-api-docs.py
 */

// Sample VRPTW problem: 1 vehicle, 1 depot, 3 delivery tasks
const SURGE_SAMPLE_PROBLEM = {
    vehicles: [
        {id: 0, depot_start: 0, depot_end: 0,
         capacity: [20], shift: [0, 1000]}
    ],
    depots: [
        {id: 0, x: 40.0, y: 50.0, tw: [0, 1000]}
    ],
    tasks: [
        {id: 0, x: 45.0, y: 55.0, tw: [0, 500], service: 10, demand: [5]},
        {id: 1, x: 42.0, y: 58.0, tw: [0, 500], service: 10, demand: [3]},
        {id: 2, x: 38.0, y: 52.0, tw: [100, 800], service: 10, demand: [4]}
    ],
    requests: [
        {id: 0, delivery_task: 0},
        {id: 1, delivery_task: 1},
        {id: 2, delivery_task: 2}
    ],
    config: {max_iterations: 1000}
};

async function solveSurgeProblem() {
    if (!surgeDemo || !surgeDemo.isReady()) return;

    const btn = document.getElementById('surge-api-v1-solve-try-btn');
    const result = document.getElementById('surge-api-v1-solve-result');
    const status = document.getElementById('surge-api-v1-solve-status');
    const output = document.getElementById('surge-api-v1-solve-output');

    btn.disabled = true;
    btn.textContent = 'Solving...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const solution = await surgeDemo.solve(SURGE_SAMPLE_PROBLEM);
        const elapsed = (performance.now() - startTime).toFixed(1);

        const html = formatJsonWithHighlighting(solution);
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, html);
        } else {
            result.innerHTML = html;
        }

        const stats = solution.stats || {};
        const veh = stats.vehicles_used || '?';
        const dist = stats.total_distance ? stats.total_distance.toFixed(1) : '?';
        const unasgn = stats.unassigned || 0;
        status.textContent = `Solved in ${elapsed}ms (${veh} vehicles, ${dist} distance, ${unasgn} unassigned)`;
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

function fetchSurgeHealth() {
    fetchJsonEndpoint(surgeDemo, '/api/v1/health', 'surge-api-v1-health-try-btn', 'surge-api-v1-health-result', 'surge-api-v1-health-status', 'surge-api-v1-health-output', 'Check Health');
}

function initSurgeDemo() {
    enableBtn('surge-api-v1-solve-try-btn', 'Solve Problem');
    enableBtn('surge-api-v1-health-try-btn', 'Check Health');
    const surgeStatus = document.getElementById('surge-api-v1-health-status');
    if (surgeStatus) {
        surgeStatus.textContent = `Surge ${surgeDemo.getVersion()} ready - VRP solver in your browser`;
        surgeStatus.className = 'demo-status success';
        document.getElementById('surge-api-v1-health-output').classList.add('visible');
    }
}

function handleSurgeError(err) {
    disableBtn('surge-api-v1-solve-try-btn');
    disableBtn('surge-api-v1-health-try-btn');
    const surgeStatus = document.getElementById('surge-api-v1-health-status');
    if (surgeStatus) {
        surgeStatus.textContent = 'Failed to load WASM: ' + err.message;
        surgeStatus.className = 'demo-status error';
        document.getElementById('surge-api-v1-health-output').classList.add('visible');
    }
}
