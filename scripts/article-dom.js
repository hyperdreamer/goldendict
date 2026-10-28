/* Pure helpers are defined at DocumentCreation; readiness needs DOM + channel. */
(() => {
  "use strict";
  const generation = window.__gdDocumentGeneration;
  const documentId = (globalThis.crypto && crypto.randomUUID) ? crypto.randomUUID() :
    `${generation}-${Date.now()}-${Math.random()}`;
  window.__gdFrameDocumentId = documentId;
  let ready = false, readingTimer = null, restoreNumber = 0;
  const selections = new Map(), marks = new Set();
  let selectionNumber = 0, hits = [], textVersion = 0, lastText = null;
  const members = () => {
    const ids = Array.isArray(window.gdArticleContents) ? window.gdArticleContents :
      String(window.gdArticleContents || "").trim().split(/\s+/u);
    return [...new Set(ids.filter(id => typeof id === "string" && id &&
      document.getElementById(`gdfrom-${id}`)))];
  };
  const snapshot = kind => {
    const articles = members();
    const current = String(window.gdCurrentArticle || "");
    const audio = window.gdAudioLinks;
    return {kind, currentArticle: articles.some(id => `gdfrom-${id}` === current) ? current : "",
      sx: window.scrollX, sy: window.scrollY, articles,
      hasAudio: !!(audio && (audio.first || articles.some(id => audio[id])))};
  };
  const changed = () => {
    if (!ready) return;
    clearTimeout(readingTimer);
    readingTimer = setTimeout(() => {
      if (generation === window.__gdDocumentGeneration)
        window.articleview.onJsReadingState(snapshot("state"));
    }, 100);
  };
  window.gdOnBridgeReady = () => {
    if (ready || !window.__gdChannelReady || document.readyState === "loading") return;
    ready = true;
    window.articleview.onJsReadingState(snapshot("ready"));
    window.dispatchEvent(new Event("gd-article-ready"));
  };
  window.addEventListener("scroll", changed, {passive: true});
  window.addEventListener("gd-active-article", changed);
  document.addEventListener("DOMContentLoaded", window.gdOnBridgeReady, {once: true});

  const excluded = node => !node.parentElement ||
    !!node.parentElement.closest("script,style,noscript,input,textarea,select,button,mark,[contenteditable=true]");
  const textNodes = root => {
    const result = [];
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, {
      acceptNode: node => excluded(node) || !node.parentElement.getClientRects().length ?
        NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT
    });
    for (let node = walker.nextNode(); node; node = walker.nextNode()) result.push(node);
    return result;
  };
  const textMap = () => {
    const nodes = document.body ? textNodes(document.body) : [];
    let offset = 0;
    const map = nodes.map(node => {
      const entry = {node, start: offset, end: offset + node.length};
      offset = entry.end;
      return entry;
    });
    const text = nodes.map(node => node.data).join("");
    if (text !== lastText) { lastText = text; ++textVersion; }
    window.__gdTextSnapshot = {text, version: textVersion};
    return {map, text, version: textVersion};
  };
  window.gdWordAtPoint = (x, y) => {
    if (!Number.isFinite(x) || !Number.isFinite(y)) return "";
    const element = document.elementFromPoint(x, y);
    if (!element || element.closest("input,textarea,select,button,[contenteditable=true]")) return "";
    // All offsets are UTF-16, but geometry is measured one complete scalar at a
    // time. Native caret APIs may snap to either side of a glyph, or fail on
    // embedded content. The fallback has a finite node/scalar budget, including
    // zero-width glyphs, and never changes the user's selection.
    const maxNodes = 256, maxScalars = 8192, maxText = 65536;
    let remaining = maxScalars;
    const valid = node => node && node.nodeType === Node.TEXT_NODE && node.isConnected &&
      node.ownerDocument === document && node.getRootNode() === document && !excluded(node) &&
      (element.contains(node) || node.parentElement.contains(element));
    const scalarStart = (text, offset) => offset > 0 && offset < text.length &&
      /[\uD800-\uDBFF]/u.test(text[offset - 1]) && /[\uDC00-\uDFFF]/u.test(text[offset]) ? offset - 1 : offset;
    const contains = (node, start) => {
      if (start < 0 || start >= node.length || remaining-- <= 0) return false;
      const end = start + (node.data.codePointAt(start) > 0xffff ? 2 : 1);
      const range = document.createRange();
      range.setStart(node, start); range.setEnd(node, end);
      return Array.from(range.getClientRects()).some(rect => rect.width > 0 && rect.height > 0 &&
        x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom);
    };
    let hit = null;
    for (const api of ["caretPositionFromPoint", "caretRangeFromPoint"]) {
      try {
        if (typeof document[api] !== "function") continue;
        const caret = document[api](x, y);
        const node = caret && (api === "caretPositionFromPoint" ? caret.offsetNode : caret.startContainer);
        const offset = caret && (api === "caretPositionFromPoint" ? caret.offset : caret.startOffset);
        if (!valid(node) || !Number.isInteger(offset) || offset < 0 || offset > node.length) continue;
        const at = scalarStart(node.data, offset);
        for (const start of [at, scalarStart(node.data, at - 1)])
          if (contains(node, start)) { hit = {node, offset: start}; break; }
        if (hit) break;
      } catch (_) { /* Try the other native API, then bounded Range geometry. */ }
    }
    if (!hit) {
      const walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
      let count = 0;
      for (let node = walker.nextNode(); node && count++ < maxNodes && remaining > 0; node = walker.nextNode()) {
        if (!valid(node)) continue;
        let offset = 0;
        for (const scalar of node.data) {
          if (contains(node, offset)) { hit = {node, offset}; break; }
          offset += scalar.length;
          if (remaining <= 0) break;
        }
        if (hit) break;
      }
    }
    if (!hit) return "";
    let block = hit.node.parentElement;
    while (block.parentElement && ["inline", "contents"].includes(getComputedStyle(block).display))
      block = block.parentElement;
    const walker = document.createTreeWalker(block, NodeFilter.SHOW_TEXT);
    let text = "", position = -1, count = 0;
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      if (++count > maxNodes || text.length + node.length > maxText) return "";
      if (excluded(node) || !node.parentElement.getClientRects().length) continue;
      if (node === hit.node) position = text.length + hit.offset;
      text += node.data;
    }
    if (position < 0) return "";
    const pattern = /[\p{L}\p{M}\p{N}]+(?:['’\-][\p{L}\p{M}\p{N}]+)*/gu;
    for (const match of text.matchAll(pattern))
      if (position >= match.index && position < match.index + match[0].length) return match[0];
    return "";
  };
  window.gdSaveSelection = () => {
    const selection = window.getSelection();
    const token = `${generation}:${++selectionNumber}`;
    selections.set(token, {generation, ranges: selection ?
      Array.from({length: selection.rangeCount}, (_, i) => selection.getRangeAt(i).cloneRange()) : []});
    return token;
  };
  window.gdRestoreSelection = token => {
    const saved = selections.get(token);
    selections.delete(token);
    if (!saved || saved.generation !== window.__gdDocumentGeneration ||
        saved.ranges.some(range => !range.startContainer.isConnected || !range.endContainer.isConnected ||
          range.startContainer.ownerDocument !== document || range.endContainer.ownerDocument !== document)) return;
    const selection = window.getSelection();
    if (!selection) return;
    selection.removeAllRanges();
    for (const range of saved.ranges) selection.addRange(range);
  };
  window.gdClearHighlights = () => {
    const parents = new Set();
    for (const mark of marks) {
      if (!mark.parentNode) continue;
      parents.add(mark.parentNode);
      mark.replaceWith(...mark.childNodes);
    }
    marks.clear(); hits = [];
    for (const parent of parents) parent.normalize();
    textMap();
  };
  window.gdHighlightAll = (words, matchCase) => {
    window.gdClearHighlights();
    const captured = textMap();
    let spans = [];
    if (words && !Array.isArray(words) && typeof words === "object") {
      // PCRE is evaluated by C++; offsets and lengths are UTF-16, just like QString.
      if (words.text !== captured.text || (words.version !== undefined && words.version !== captured.version))
        return -1;
      spans = Array.isArray(words.spans) ? words.spans.map(span => ({start: span.start, length: span.length})) : [];
    } else {
      if (!Array.isArray(words)) return 0;
      for (const word of words) {
        if (typeof word !== "string" || !word) continue;
        // Only escaped literal words enter JS RegExp; never a Qt PCRE pattern.
        const escaped = word.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
        const pattern = new RegExp(escaped, matchCase ? "gu" : "giu");
        for (const match of captured.text.matchAll(pattern)) spans.push({start: match.index, length: match[0].length});
      }
    }
    spans.sort((a, b) => a.start - b.start || b.length - a.length);
    let end = 0;
    spans = spans.filter(span => {
      if (!Number.isSafeInteger(span.start) || !Number.isSafeInteger(span.length) || span.length <= 0 ||
          span.start < end || span.start + span.length > captured.text.length) return false;
      // Do not split a surrogate pair at either boundary.
      for (const p of [span.start, span.start + span.length])
        if (p > 0 && p < captured.text.length && /[\uD800-\uDBFF]/.test(captured.text[p - 1]) &&
            /[\uDC00-\uDFFF]/.test(captured.text[p])) return false;
      end = span.start + span.length;
      return true;
    });
    hits = spans.map(() => []);
    for (let i = spans.length - 1; i >= 0; --i) {
      const span = spans[i];
      for (let j = captured.map.length - 1; j >= 0; --j) {
        const entry = captured.map[j];
        const start = Math.max(span.start, entry.start) - entry.start;
        const finish = Math.min(span.start + span.length, entry.end) - entry.start;
        if (start >= finish) continue;
        const selected = entry.node.splitText(start);
        selected.splitText(finish - start);
        const mark = document.createElement("mark");
        mark.setAttribute("data-gd-highlight", documentId);
        selected.replaceWith(mark); mark.appendChild(selected);
        marks.add(mark); hits[i].unshift(mark);
      }
    }
    return hits.length;
  };
  const activate = id => {
    if (!id || !members().some(member => `gdfrom-${member}` === id)) return false;
    if (window.gdMakeArticleActive) window.gdMakeArticleActive(id.slice(7));
    else {
      const previous = document.getElementById(String(window.gdCurrentArticle || ""));
      if (previous) previous.classList.remove("gdactivearticle");
      document.getElementById(id).classList.add("gdactivearticle");
      window.gdCurrentArticle = id;
      if (window.gdAudioLinks) window.gdAudioLinks.current = id.slice(7);
      window.articleview.onJsActiveArticleChanged(id);
    }
    return true;
  };
  window.gdScrollToHit = i => {
    if (!Number.isInteger(i) || i < 0 || i >= hits.length || !hits[i][0] || !hits[i][0].isConnected) return false;
    const mark = hits[i][0];
    const article = mark.closest("[id^=gdfrom-]");
    if (article) activate(article.id);
    mark.scrollIntoView({block: "center"});
    return true;
  };
  window.gdFrameRect = name => {
    const frame = Array.from(document.querySelectorAll("iframe")).find(item => item.name === name || item.id === name);
    if (!frame) return null;
    const rect = frame.getBoundingClientRect(), style = getComputedStyle(frame);
    return {x: rect.x, y: rect.y, width: rect.width, height: rect.height,
      borderLeft: parseFloat(style.borderLeftWidth) || 0, borderTop: parseFloat(style.borderTopWidth) || 0};
  };
  window.gdMeasureFrame = () => {
    const root = document.documentElement, body = document.body;
    const rect = body && body.getBoundingClientRect();
    // A subframe's root scrollHeight includes the viewport's minimum height.
    // Feeding that back into the iframe prevents a later, shorter document from
    // shrinking. Measure the body content and its margins in that document.
    const height = body ? Math.max(body.scrollHeight, body.offsetHeight,
      rect.bottom + window.scrollY + (parseFloat(getComputedStyle(body).marginBottom) || 0)) :
      (root ? root.scrollHeight : 0);
    const width = Math.max(root ? root.scrollWidth : 0, root ? root.offsetWidth : 0,
      body ? body.scrollWidth : 0, body ? body.offsetWidth : 0);
    return Number.isFinite(height) && Number.isFinite(width) && height > 0 && width > 0 ?
      {docId: documentId, height, width} : null;
  };
  window.gdCaptureState = () => ({currentArticle: snapshot("state").currentArticle, sx: scrollX, sy: scrollY});
  window.gdRestoreArticleState = state => {
    if (!state || typeof state.currentArticle !== "string" || !Number.isFinite(state.sx) || !Number.isFinite(state.sy))
      return {completed: false};
    const operation = ++restoreNumber, start = performance.now();
    activate(state.currentArticle);
    const completion = {completed: false, currentArticle: String(window.gdCurrentArticle || "")};
    let frames = 0, lastHeight = -1;
    const settle = () => {
      if (generation !== window.__gdDocumentGeneration || operation !== restoreNumber) return;
      const height = document.documentElement.scrollHeight;
      if (state.restoreScroll === false || height === lastHeight || ++frames >= 10 || performance.now() - start >= 500) {
        window.scrollTo(state.restoreScroll === false ? 0 : state.sx, state.restoreScroll === false ? 0 : state.sy);
        completion.completed = true;
        changed();
      } else { lastHeight = height; requestAnimationFrame(settle); }
    };
    requestAnimationFrame(settle);
    return completion;
  };
  window.gdOnBridgeReady();
})();
