/**
 * CRT Terminal Typing Animation
 * Progressively renders code blocks like a 70s terminal with blinking cursor.
 */
(function() {
    'use strict';

    // Store original content and track animated blocks
    const codeBlocks = document.querySelectorAll('.code-block');
    const blockData = new Map();

    codeBlocks.forEach(block => {
        const pre = block.querySelector('pre');
        // Only animate static code blocks (those with content, not demo result containers)
        if (pre && pre.innerHTML.trim().length > 0 && !pre.id) {
            // Capture the final height before hiding content
            const finalHeight = pre.offsetHeight;
            blockData.set(block, {
                originalHTML: pre.innerHTML,
                finalHeight: finalHeight,
                animated: false
            });
            // Set fixed height to prevent layout shift, hide content for animation
            pre.style.minHeight = finalHeight + 'px';
            pre.style.visibility = 'hidden';
        }
    });

    // Typing animation function
    function typeContent(block) {
        const data = blockData.get(block);
        if (!data || data.animated) return;
        data.animated = true;

        const pre = block.querySelector('pre');
        const originalHTML = data.originalHTML;
        pre.style.visibility = 'visible';
        block.classList.add('typing');

        // Build a flat list of characters with their HTML wrapper info
        const chars = [];
        let inTag = false;
        let currentTag = '';
        let tagStack = [];

        // Parse the HTML to extract characters with their formatting context
        const tempDiv = document.createElement('div');
        tempDiv.innerHTML = originalHTML;

        function walkNodes(node, wrapperStack) {
            if (node.nodeType === Node.TEXT_NODE) {
                const text = node.textContent;
                // Clone current wrapper stack for each character
                for (let i = 0; i < text.length; i++) {
                    chars.push({
                        char: text[i],
                        wrappers: wrapperStack.slice()
                    });
                }
            } else if (node.nodeType === Node.ELEMENT_NODE) {
                const newStack = wrapperStack.slice();
                newStack.push({
                    tag: node.tagName.toLowerCase(),
                    className: node.className
                });
                for (let child of node.childNodes) {
                    walkNodes(child, newStack);
                }
            }
        }

        walkNodes(tempDiv, []);

        // Now render characters one by one
        let index = 0;
        let builtHTML = '';

        function getOpenTags(wrappers) {
            return wrappers.map(w => {
                if (w.className) {
                    return `<${w.tag} class="${w.className}">`;
                }
                return `<${w.tag}>`;
            }).join('');
        }

        function getCloseTags(wrappers) {
            return wrappers.slice().reverse().map(w => `</${w.tag}>`).join('');
        }

        function escapeHTML(char) {
            if (char === '<') return '&lt;';
            if (char === '>') return '&gt;';
            if (char === '&') return '&amp;';
            return char;
        }

        function typeNext() {
            if (index >= chars.length) {
                block.classList.remove('typing');
                block.classList.add('typed');
                // Remove fixed height constraint now that content is complete
                pre.style.minHeight = '';
                return;
            }

            const charData = chars[index];
            const openTags = getOpenTags(charData.wrappers);
            const closeTags = getCloseTags(charData.wrappers);

            // Rebuild full HTML up to current character
            builtHTML = '';
            for (let i = 0; i <= index; i++) {
                const c = chars[i];
                const prevWrappers = i > 0 ? chars[i-1].wrappers : [];
                const currWrappers = c.wrappers;

                // Find where wrappers diverge
                let commonLen = 0;
                while (commonLen < prevWrappers.length &&
                       commonLen < currWrappers.length &&
                       prevWrappers[commonLen].tag === currWrappers[commonLen].tag &&
                       prevWrappers[commonLen].className === currWrappers[commonLen].className) {
                    commonLen++;
                }

                // Close tags that are no longer needed
                for (let j = prevWrappers.length - 1; j >= commonLen; j--) {
                    builtHTML += `</${prevWrappers[j].tag}>`;
                }

                // Open new tags
                for (let j = commonLen; j < currWrappers.length; j++) {
                    const w = currWrappers[j];
                    if (w.className) {
                        builtHTML += `<${w.tag} class="${w.className}">`;
                    } else {
                        builtHTML += `<${w.tag}>`;
                    }
                }

                builtHTML += escapeHTML(c.char);
            }

            // Close any remaining open tags
            const lastWrappers = chars[index].wrappers;
            for (let j = lastWrappers.length - 1; j >= 0; j--) {
                builtHTML += `</${lastWrappers[j].tag}>`;
            }

            pre.innerHTML = builtHTML;
            index++;

            // Variable typing speed
            const char = charData.char;
            let delay;
            if (char === '\n') {
                delay = 25 + Math.random() * 15;
            } else if (char === ' ') {
                delay = 6 + Math.random() * 6;
            } else {
                delay = 10 + Math.random() * 15;
            }

            setTimeout(typeNext, delay);
        }

        typeNext();
    }

    // Intersection Observer for triggering animation on scroll
    const observer = new IntersectionObserver((entries) => {
        entries.forEach(entry => {
            if (entry.isIntersecting) {
                typeContent(entry.target);
                observer.unobserve(entry.target);
            }
        });
    }, { threshold: 0.3 });

    codeBlocks.forEach(block => {
        if (blockData.has(block)) {
            observer.observe(block);
        }
    });
})();
