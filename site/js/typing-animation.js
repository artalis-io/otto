/**
 * CRT Terminal Typing Animation
 * Progressively renders code blocks like a 70s terminal with blinking cursor.
 */
(function() {
    'use strict';

    const codeBlocks = document.querySelectorAll('.code-block');
    const blockData = new Map();

    codeBlocks.forEach(block => {
        const pre = block.querySelector('pre');
        // Only animate static code blocks (those with content, not demo result containers)
        if (pre && pre.innerHTML.trim().length > 0 && !pre.id) {
            blockData.set(block, {
                originalHTML: pre.innerHTML,
                animated: false
            });
            pre.style.visibility = 'hidden';
        }
    });

    function typeContent(block) {
        const data = blockData.get(block);
        if (!data || data.animated) return;
        data.animated = true;

        const pre = block.querySelector('pre');
        const originalHTML = data.originalHTML;
        pre.style.visibility = 'visible';
        pre.innerHTML = '';
        block.classList.add('typing');

        // Parse HTML into text chunks with their tags
        const tempDiv = document.createElement('div');
        tempDiv.innerHTML = originalHTML;

        // Extract all text content with formatting
        const segments = [];
        function extractSegments(node) {
            if (node.nodeType === Node.TEXT_NODE) {
                const text = node.textContent;
                for (let i = 0; i < text.length; i++) {
                    segments.push({ char: text[i], wrapper: null });
                }
            } else if (node.nodeType === Node.ELEMENT_NODE) {
                const tag = node.tagName.toLowerCase();
                const className = node.className;
                const children = node.childNodes;
                for (let i = 0; i < children.length; i++) {
                    const childNode = children[i];
                    if (childNode.nodeType === Node.TEXT_NODE) {
                        const text = childNode.textContent;
                        for (let j = 0; j < text.length; j++) {
                            segments.push({ char: text[j], wrapper: { tag, className } });
                        }
                    } else {
                        extractSegments(childNode);
                    }
                }
            }
        }
        extractSegments(tempDiv);

        // Type characters with variable speed
        let index = 0;
        let currentSpan = null;
        let currentWrapper = null;

        function typeNext() {
            if (index >= segments.length) {
                block.classList.remove('typing');
                block.classList.add('typed');
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
