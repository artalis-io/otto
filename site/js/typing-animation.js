/**
 * CRT Terminal Typing Animation
 * Progressively renders code blocks like a 70s terminal with blinking cursor.
 */
(function() {
    'use strict';

    const codeBlocks = document.querySelectorAll('.code-block');
    const blockData = new Map();

    // Track active animations to prevent overlapping renders
    const activeAnimations = new Map();
    let animationIdCounter = 0;

    /**
     * Animate typing of HTML content into a code block.
     * Can be called dynamically for WASM demo responses.
     * Safe to call multiple times - cancels any existing animation.
     *
     * @param {HTMLElement} block - The .code-block container
     * @param {string} html - The HTML content to type
     */
    function typeAnimateContent(block, html) {
        const pre = block.querySelector('pre');
        if (!pre) return;

        // Cancel any existing animation on this block
        const thisAnimationId = ++animationIdCounter;
        activeAnimations.set(block, thisAnimationId);

        // Clear and prepare
        pre.innerHTML = '';
        pre.style.whiteSpace = 'pre';
        block.classList.remove('typed');
        block.classList.add('typing');

        // Parse HTML into segments
        const tempDiv = document.createElement('div');
        tempDiv.innerHTML = html;

        const segments = [];
        function extractSegments(node, wrapper) {
            if (node.nodeType === Node.TEXT_NODE) {
                const text = node.textContent;
                for (let i = 0; i < text.length; i++) {
                    segments.push({ char: text[i], wrapper: wrapper });
                }
            } else if (node.nodeType === Node.ELEMENT_NODE) {
                const newWrapper = { tag: node.tagName.toLowerCase(), className: node.className };
                for (const child of node.childNodes) {
                    extractSegments(child, newWrapper);
                }
            }
        }
        for (const child of tempDiv.childNodes) {
            extractSegments(child, null);
        }

        // Type characters
        let index = 0;
        let currentSpan = null;
        let currentWrapper = null;

        function typeNext() {
            // Abort if a newer animation started on this block
            if (activeAnimations.get(block) !== thisAnimationId) {
                return;
            }

            if (index >= segments.length) {
                block.classList.remove('typing');
                block.classList.add('typed');
                activeAnimations.delete(block);
                return;
            }

            const segment = segments[index];
            const wrapperKey = segment.wrapper ? `${segment.wrapper.tag}.${segment.wrapper.className}` : null;

            if (wrapperKey !== currentWrapper) {
                currentWrapper = wrapperKey;
                if (segment.wrapper) {
                    currentSpan = document.createElement(segment.wrapper.tag);
                    currentSpan.className = segment.wrapper.className;
                    pre.appendChild(currentSpan);
                } else {
                    currentSpan = null;
                }
            }

            const textNode = document.createTextNode(segment.char);
            if (currentSpan) {
                currentSpan.appendChild(textNode);
            } else {
                pre.appendChild(textNode);
            }

            index++;

            // Faster typing for dynamic content
            let delay;
            if (segment.char === '\n') {
                delay = 15 + Math.random() * 10;
            } else if (segment.char === ' ') {
                delay = 4 + Math.random() * 4;
            } else {
                delay = 6 + Math.random() * 8;
            }

            setTimeout(typeNext, delay);
        }

        typeNext();
    }

    // Export for dynamic use
    window.typeAnimateContent = typeAnimateContent;

    // Initialize after page fully loads (fonts, CSS applied) for correct height
    function initBlocks() {
        codeBlocks.forEach(block => {
            const pre = block.querySelector('pre');
            // Only animate static code blocks (those with content, not demo result containers)
            if (pre && pre.innerHTML.trim().length > 0 && !pre.id && !blockData.has(block)) {
                // Capture final height before hiding
                const finalHeight = pre.offsetHeight;
                blockData.set(block, {
                    originalHTML: pre.innerHTML,
                    finalHeight: finalHeight,
                    animated: false
                });
                // Set fixed height to prevent layout shift
                pre.style.minHeight = finalHeight + 'px';
                pre.style.visibility = 'hidden';
            }
        });
    }

    if (document.readyState === 'complete') {
        requestAnimationFrame(initBlocks);
    } else {
        window.addEventListener('load', function() {
            requestAnimationFrame(initBlocks);
        });
    }

    function typeContent(block) {
        const data = blockData.get(block);
        if (!data || data.animated) return;
        data.animated = true;

        const pre = block.querySelector('pre');
        const originalHTML = data.originalHTML;
        pre.style.visibility = 'visible';
        pre.style.whiteSpace = 'pre';  // Ensure whitespace is preserved
        pre.innerHTML = '';
        block.classList.add('typing');

        // Parse HTML into text chunks with their tags
        const tempDiv = document.createElement('div');
        tempDiv.innerHTML = originalHTML;

        // Extract all text content with formatting
        const segments = [];
        function extractSegments(node, wrapper) {
            if (node.nodeType === Node.TEXT_NODE) {
                const text = node.textContent;
                for (let i = 0; i < text.length; i++) {
                    segments.push({ char: text[i], wrapper: wrapper });
                }
            } else if (node.nodeType === Node.ELEMENT_NODE) {
                const newWrapper = { tag: node.tagName.toLowerCase(), className: node.className };
                for (const child of node.childNodes) {
                    extractSegments(child, newWrapper);
                }
            }
        }
        // Start extraction from container's children (not the container itself)
        for (const child of tempDiv.childNodes) {
            extractSegments(child, null);
        }

        // Type characters with variable speed
        let index = 0;
        let currentSpan = null;
        let currentWrapper = null;

        function typeNext() {
            if (index >= segments.length) {
                block.classList.remove('typing');
                block.classList.add('typed');
                pre.style.minHeight = '';
                return;
            }

            const segment = segments[index];
            const wrapperKey = segment.wrapper ? `${segment.wrapper.tag}.${segment.wrapper.className}` : null;

            // Handle wrapper changes
            if (wrapperKey !== currentWrapper) {
                currentWrapper = wrapperKey;
                if (segment.wrapper) {
                    currentSpan = document.createElement(segment.wrapper.tag);
                    currentSpan.className = segment.wrapper.className;
                    pre.appendChild(currentSpan);
                } else {
                    currentSpan = null;
                }
            }

            // Add character
            const textNode = document.createTextNode(segment.char);
            if (currentSpan) {
                currentSpan.appendChild(textNode);
            } else {
                pre.appendChild(textNode);
            }

            index++;

            // Variable typing speed
            let delay;
            if (segment.char === '\n') {
                delay = 30 + Math.random() * 20;
            } else if (segment.char === ' ') {
                delay = 8 + Math.random() * 8;
            } else {
                delay = 12 + Math.random() * 18;
            }

            setTimeout(typeNext, delay);
        }

        typeNext();
    }

    const observer = new IntersectionObserver((entries) => {
        entries.forEach(entry => {
            if (entry.isIntersecting && blockData.has(entry.target)) {
                typeContent(entry.target);
                observer.unobserve(entry.target);
            }
        });
    }, { threshold: 0.3 });

    codeBlocks.forEach(block => {
        observer.observe(block);
    });
})();
