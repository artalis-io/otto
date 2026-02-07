/**
 * CRT Terminal Typing Animation
 * Progressively renders code blocks like a 70s terminal with blinking cursor.
 */
(function() {
    'use strict';

    const CURSOR = '\u2588'; // Block cursor character
    const codeBlocks = document.querySelectorAll('.code-block');
    const blockData = new Map();

    // Initialize after page is fully rendered
    function initBlocks() {
        codeBlocks.forEach(block => {
            const pre = block.querySelector('pre');
            if (pre && pre.innerHTML.trim().length > 0 && !pre.id && !blockData.has(block)) {
                const finalHeight = pre.offsetHeight;
                blockData.set(block, {
                    originalHTML: pre.innerHTML,
                    finalHeight: finalHeight,
                    animated: false
                });
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
        block.classList.add('typing');

        // Get plain text and build a map of positions to HTML
        const tempDiv = document.createElement('div');
        tempDiv.innerHTML = originalHTML;
        const fullText = tempDiv.textContent || tempDiv.innerText;

        // Remove trailing whitespace for cursor positioning
        const trimmedText = fullText.replace(/\s+$/, '');
        const textLength = trimmedText.length;

        let charIndex = 0;

        function typeNext() {
            if (charIndex >= textLength) {
                // Animation complete - show final content with blinking cursor
                block.classList.remove('typing');
                block.classList.add('typed');
                pre.style.minHeight = '';
                // Find position to insert cursor (before trailing whitespace)
                const cursorHTML = '<span class="typed-cursor">' + CURSOR + '</span>';
                // Insert cursor at end of trimmed content
                pre.innerHTML = originalHTML.replace(/(\s*)$/, cursorHTML + '$1');
                return;
            }

            // Show text up to current position with cursor
            const currentText = trimmedText.substring(0, charIndex + 1);
            const remaining = trimmedText.substring(charIndex + 1);

            // Rebuild HTML with visible portion + cursor
            // Simple approach: show original HTML structure but with cursor inserted
            tempDiv.innerHTML = originalHTML;
            const walker = document.createTreeWalker(tempDiv, NodeFilter.SHOW_TEXT, null, false);

            let pos = 0;
            let cursorInserted = false;
            const targetPos = charIndex + 1;

            while (walker.nextNode()) {
                const node = walker.currentNode;
                const nodeText = node.textContent;
                const nodeStart = pos;
                const nodeEnd = pos + nodeText.length;

                if (!cursorInserted && targetPos <= nodeEnd) {
                    // Cursor goes in this node
                    const localPos = targetPos - nodeStart;
                    const before = nodeText.substring(0, localPos);
                    const after = nodeText.substring(localPos);

                    // Create cursor span
                    const cursorSpan = document.createElement('span');
                    cursorSpan.className = 'typing-cursor';
                    cursorSpan.textContent = CURSOR;

                    // Replace text node with before + cursor + hidden after
                    const parent = node.parentNode;
                    const beforeNode = document.createTextNode(before);
                    const afterSpan = document.createElement('span');
                    afterSpan.style.visibility = 'hidden';
                    afterSpan.textContent = after;

                    parent.insertBefore(beforeNode, node);
                    parent.insertBefore(cursorSpan, node);
                    parent.insertBefore(afterSpan, node);
                    parent.removeChild(node);

                    cursorInserted = true;
                    break;
                }

                pos = nodeEnd;
            }

            // Hide all text after cursor position
            if (cursorInserted) {
                // Continue walking to hide remaining text
                while (walker.nextNode()) {
                    const node = walker.currentNode;
                    const hiddenSpan = document.createElement('span');
                    hiddenSpan.style.visibility = 'hidden';
                    hiddenSpan.textContent = node.textContent;
                    node.parentNode.insertBefore(hiddenSpan, node);
                    node.parentNode.removeChild(node);
                }
            }

            pre.innerHTML = tempDiv.innerHTML;
            charIndex++;

            // Variable typing speed
            const char = trimmedText[charIndex - 1];
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
