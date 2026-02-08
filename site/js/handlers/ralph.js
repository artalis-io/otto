/**
 * Ralph WASM Demo Handlers
 * Auto-loaded by build-api-docs.py
 */

async function solveRalphProblem() {
    if (!ralphDemo || !ralphDemo.isReady()) return;

    const btn = document.getElementById('ralph-api-v1-solve-try-btn');
    const result = document.getElementById('ralph-api-v1-solve-result');
    const status = document.getElementById('ralph-api-v1-solve-status');
    const output = document.getElementById('ralph-api-v1-solve-output');

    // Get problem from textarea (ID matches demo_id pattern)
    const problemInput = document.getElementById('ralph-api-v1-solve-problem');
    const problem = problemInput ? problemInput.value : `max: 5 x + 3 y
subject to
wood: 2 x + 4 y <= 40
labor: 3 x + 2 y <= 24
bounds
x >= 0
y >= 0
end`;

    btn.disabled = true;
    btn.textContent = 'Solving...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        // Use format=lp query param to get SOL format output
        const response = await ralphDemo.fetch('/api/v1/solve?format=lp', {
            method: 'POST',
            headers: { 'Content-Type': 'text/plain' },
            body: problem  // Raw LP text, not JSON
        });
        const elapsed = (performance.now() - startTime).toFixed(1);

        const text = await response.text();

        // Display as plain text (SOL format)
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, text);
        } else {
            result.textContent = text;
        }

        if (response.ok && text.includes('OPTIMAL')) {
            // Extract objective from SOL output
            const objMatch = text.match(/objective value:\s*([\d.-]+)/);
            const obj = objMatch ? objMatch[1] : '?';
            status.textContent = `Solved in ${elapsed}ms: objective = ${obj}`;
            status.className = 'demo-status success';
        } else if (text.includes('INFEASIBLE')) {
            status.textContent = `Infeasible (${elapsed}ms)`;
            status.className = 'demo-status error';
        } else if (text.includes('error')) {
            status.textContent = `Error (${elapsed}ms)`;
            status.className = 'demo-status error';
        } else {
            status.textContent = `Completed in ${elapsed}ms`;
            status.className = 'demo-status warning';
        }
    } catch (err) {
        console.error('Solve failed:', err);
        result.textContent = `Error: ${err.message}`;
        status.textContent = '';
        status.className = 'demo-status';
    } finally {
        btn.disabled = false;
        btn.textContent = 'Solve';
    }
}

async function fetchRalphFormats() {
    if (!ralphDemo || !ralphDemo.isReady()) return;

    const btn = document.getElementById('ralph-api-v1-formats-try-btn');
    const result = document.getElementById('ralph-api-v1-formats-result');
    const status = document.getElementById('ralph-api-v1-formats-status');
    const output = document.getElementById('ralph-api-v1-formats-output');

    btn.disabled = true;
    btn.textContent = 'Loading...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const response = await ralphDemo.fetch('/api/v1/formats');
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
        const count = data.formats ? data.formats.length : 0;
        status.textContent = `${count} formats available (${elapsed}ms)`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Formats request failed:', err);
        result.innerHTML = `<span class="demo-error">Error: ${err.message}</span>`;
        status.textContent = '';
        status.className = 'demo-status';
    } finally {
        btn.disabled = false;
        btn.textContent = 'Get Formats';
    }
}

function fetchRalphHealth() {
    fetchJsonEndpoint(ralphDemo, '/api/v1/health', 'ralph-api-v1-health-try-btn', 'ralph-api-v1-health-result', 'ralph-api-v1-health-status', 'ralph-api-v1-health-output', 'Check Health');
}

function initRalphDemo() {
    enableBtn('ralph-api-v1-solve-try-btn', 'Solve');
    enableBtn('ralph-api-v1-formats-try-btn', 'Get Formats');
    enableBtn('ralph-api-v1-health-try-btn', 'Check Health');
    const ralphStatus = document.getElementById('ralph-api-v1-solve-status');
    if (ralphStatus) {
        ralphStatus.textContent = `Ralph ${ralphDemo.getVersion()} ready - LP/MIP solver in your browser`;
        ralphStatus.className = 'demo-status success';
        document.getElementById('ralph-api-v1-solve-output').classList.add('visible');
    }
}

function handleRalphError(err) {
    disableBtn('ralph-api-v1-solve-try-btn');
    disableBtn('ralph-api-v1-formats-try-btn');
    disableBtn('ralph-api-v1-health-try-btn');
    const ralphStatus = document.getElementById('ralph-api-v1-solve-status');
    if (ralphStatus) {
        ralphStatus.textContent = 'Failed to load WASM: ' + err.message;
        ralphStatus.className = 'demo-status error';
        document.getElementById('ralph-api-v1-solve-output').classList.add('visible');
    }
}
