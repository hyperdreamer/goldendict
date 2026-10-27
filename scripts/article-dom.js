/* Pure helpers are defined at DocumentCreation; readiness needs DOM + channel. */
(() => {
  "use strict";
  const generation = window.__gdDocumentGeneration;
  const documentId = (globalThis.crypto && crypto.randomUUID) ? crypto.randomUUID() :
    `${generation}-${Date.now()}-${Math.random()}`;
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
    if (!element || element.closest("input,textarea,select,button")) return "";
    let node, offset;
    if (document.caretPositionFromPoint) {
      const caret = document.caretPositionFromPoint(x, y);
      if (caret) { node = caret.offsetNode; offset = caret.offset; }
    } else if (document.caretRangeFromPoint) {
      const caret = document.caretRangeFromPoint(x, y);
      if (caret) { node = caret.startContainer; offset = caret.startOffset; }
    }
    if (!node || node.nodeType !== Node.TEXT_NODE || excluded(node)) return "";
    // A caret can snap to text from whitespace: require the actual glyph rect.
    const glyph = document.createRange();
    const at = Math.min(offset, Math.max(0, node.length - 1));
    glyph.setStart(node, at); glyph.setEnd(node, Math.min(node.length, at + 1));
    let rect = glyph.getBoundingClientRect();
    if ((x < rect.left || x > rect.right) && offset > 0) {
      glyph.setStart(node, offset - 1); glyph.setEnd(node, offset);
      rect = glyph.getBoundingClientRect();
      if (x >= rect.left && x <= rect.right) offset--;
    }
    if (x < rect.left || x > rect.right || y < rect.top || y > rect.bottom) return "";
    let block = node.parentElement;
    while (block.parentElement && ["inline", "contents"].includes(getComputedStyle(block).display))
      block = block.parentElement;
    const nodes = textNodes(block);
    let text = "", position = -1;
    for (const part of nodes) {
      if (part === node) position = text.length + offset;
      text += part.data;
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
    const height = Math.max(root ? root.scrollHeight : 0, root ? root.offsetHeight : 0,
      body ? body.scrollHeight : 0, body ? body.offsetHeight : 0);
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
