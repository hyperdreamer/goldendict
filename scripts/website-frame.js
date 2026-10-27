/* Runs in MainWorld in each real frame document, including later navigations. */
(() => {
  "use strict";
  const generation = /* GD_GENERATION */;
  if (window.__gdWebsiteFrame && window.__gdWebsiteFrame.generation === generation) return;
  const docId = window.__gdFrameDocumentId || ((globalThis.crypto && crypto.randomUUID) ?
    crypto.randomUUID() : `${generation}-${Date.now()}-${Math.random()}-${Math.random()}`);
  const topDocument = window === window.top;
  const children = new WeakMap(), tokens = new WeakMap(), hints = new WeakMap();
  let serial = 0, ready = false, lastHeight = 0;
  const pending = [];
  const events = new Set(["ready", "click", "context", "measure", "resize"]);
  const number = value => typeof value === "number" && Number.isFinite(value) && value > 0 && value <= 2 ** 24;
  const valid = value => value && typeof value === "object" && events.has(value.gdFrameEvent) &&
    value.generation === generation && typeof value.docId === "string" && value.docId.length > 0 &&
    value.docId.length <= 256 && Array.isArray(value.path) && value.path.length < 32 &&
    value.path.every(part => part && typeof part.name === "string" && part.name.length <= 1024 &&
      Number.isInteger(part.index) && part.index >= 0 && part.index < 65536) &&
    (!["click", "context"].includes(value.gdFrameEvent) || typeof value.text === "string") &&
    (!["measure", "resize"].includes(value.gdFrameEvent) || (number(value.height) && number(value.width)));
  const elements = () => Array.from(document.querySelectorAll("iframe,frame"));
  const indexOf = frame => {
    for (let index = 0; index < window.frames.length; ++index)
      if (window.frames[index] === frame.contentWindow) return index;
    return -1;
  };
  const nameOf = frame => frame.hasAttribute("name") ? frame.getAttribute("name") : (frame.id || "");
  const root = frame => {
    const name = nameOf(frame);
    if (!name.startsWith("gdexpandframe-") || frame.id !== name || document.getElementById(name) !== frame)
      return false;
    const id = name.slice(14), article = document.getElementById(`gdfrom-${id}`);
    const members = Array.isArray(window.gdArticleContents) ? window.gdArticleContents :
      String(window.gdArticleContents || "").trim().split(/\s+/u);
    return !!article && article.contains(frame) && members.includes(id);
  };
  const token = frame => {
    if (!tokens.has(frame)) tokens.set(frame, `${docId}:${++serial}`);
    return tokens.get(frame);
  };
  const forward = value => {
    if (!topDocument) { window.parent.postMessage(value, "*"); return; }
    if (!window.__gdChannelReady || document.readyState === "loading") pending.push(value);
    else window.articleview.onJsFrameEvent(value);
  };
  const emit = (kind, fields = {}) => forward({gdFrameEvent: kind, generation, docId, path: [], ...fields});
  window.addEventListener("message", event => {
    const value = event.data;
    // Validate before touching a loader, forwarding, or retaining a child nonce.
    if (!valid(value)) return;
    const frame = elements().find(item => item.contentWindow === event.source);
    if (!frame || (topDocument && !root(frame))) return;
    const index = indexOf(frame);
    if (index < 0) return;
    if (value.path.length === 0 && value.gdFrameEvent === "ready") children.set(frame, value.docId);
    const path = [{name: nameOf(frame), index}, ...value.path];
    if (path.length > 32) return;
    const message = {gdFrameEvent: value.gdFrameEvent, generation, docId: value.docId, path};
    if (topDocument) message.name = path[0].name;
    if (typeof value.text === "string") message.text = value.text.slice(0, 65536);
    if (number(value.height) && number(value.width)) { message.height = value.height; message.width = value.width; }
    forward(message);
  });
  window.gdFrameChildren = () => elements().flatMap(frame => {
    const index = indexOf(frame);
    if (index < 0 || (topDocument && !root(frame))) return [];
    return [{name: nameOf(frame), index, docId: children.get(frame) || "", token: token(frame),
      url: frame.src, root: topDocument}];
  });
  const child = (index, nonce) => elements().find(frame => indexOf(frame) === index &&
    children.get(frame) === nonce && (!topDocument || root(frame)));
  window.gdApplyFrameHeight = (parentDoc, index, nonce, height, width) => {
    if (parentDoc !== docId || !number(height) || !number(width)) return false;
    const frame = child(index, nonce);
    if (!frame) return false;
    // Height is the content viewport height, independent of border/padding.
    frame.style.boxSizing = "content-box";
    frame.style.height = `${height}px`;
    frame.style.display = "block";
    const hint = hints.get(frame);
    if (hint) { hint.remove(); hints.delete(frame); }
    return true;
  };
  window.gdFrameUnavailable = (index, expectedToken) => {
    const frame = elements().find(item => indexOf(item) === index && root(item) && token(item) === expectedToken);
    if (!frame || hints.has(frame)) return false;
    const hint = document.createElement("p");
    hint.className = "gd-website-frame-unavailable";
    hint.textContent = "This site could not be framed. Try inline mode in the website dictionary preferences.";
    frame.insertAdjacentElement("afterend", hint); hints.set(frame, hint);
    return true;
  };
  window.gdFrameHit = (x, y) => {
    if (!Number.isFinite(x) || !Number.isFinite(y)) return null;
    const element = document.elementFromPoint(x, y);
    if (element && ["IFRAME", "FRAME"].includes(element.tagName)) {
      const frame = element, rect = frame.getBoundingClientRect(), style = getComputedStyle(frame);
      const left = frame.clientLeft + (parseFloat(style.paddingLeft) || 0);
      const top = frame.clientTop + (parseFloat(style.paddingTop) || 0);
      const width = frame.clientWidth - (parseFloat(style.paddingLeft) || 0) - (parseFloat(style.paddingRight) || 0);
      const height = frame.clientHeight - (parseFloat(style.paddingTop) || 0) - (parseFloat(style.paddingBottom) || 0);
      const cx = x - rect.x - left, cy = y - rect.y - top;
      if (cx < 0 || cy < 0 || cx >= width || cy >= height || (topDocument && !root(frame))) return {docId, word: ""};
      return {docId, child: children.get(frame) || "", index: indexOf(frame), name: nameOf(frame), x: cx, y: cy};
    }
    return {docId, word: window.gdWordAtPoint ? window.gdWordAtPoint(x, y) : ""};
  };
  const measure = () => {
    if (!ready || !window.gdMeasureFrame) return;
    const value = window.gdMeasureFrame();
    if (value && number(value.height) && number(value.width) && value.height !== lastHeight) {
      lastHeight = value.height;
      emit("resize", {height: value.height, width: value.width});
    }
  };
  const start = () => {
    if (ready) return;
    ready = true;
    if (!topDocument) {
      emit("ready");
      const observer = new ResizeObserver(measure);
      if (document.documentElement) observer.observe(document.documentElement);
      if (document.body) observer.observe(document.body);
      window.addEventListener("pagehide", () => observer.disconnect(), {once: true});
    }
  };
  document.addEventListener("click", event => {
    const link = event.target && event.target.closest ? event.target.closest("a") : null;
    window.gdLastUrlText = link ? link.textContent.slice(0, 65536) : "";
    if (!topDocument) emit("click", {text: window.gdLastUrlText});
  }, true);
  document.addEventListener("contextmenu", event => {
    const link = event.target && event.target.closest ? event.target.closest("a") : null;
    const selection = window.getSelection();
    window.gdLastUrlText = link ? link.textContent.slice(0, 65536) : "";
    const text = window.gdLastUrlText || (selection && selection.toString()) ||
      (window.gdWordAtPoint ? window.gdWordAtPoint(event.clientX, event.clientY) : "");
    if (!topDocument) emit("context", {text: text.slice(0, 65536)});
  }, true);
  document.addEventListener("load", event => {
    if (!["IFRAME", "FRAME"].includes(event.target.tagName)) return;
    tokens.set(event.target, `${docId}:${++serial}`);
  }, true);
  const flush = () => {
    if (!topDocument || !window.__gdChannelReady || document.readyState === "loading") return;
    for (const value of pending.splice(0)) window.articleview.onJsFrameEvent(value);
  };
  window.addEventListener("gd-article-ready", flush);
  window.__gdWebsiteFrame = {generation, docId, get ready() { return ready; }};
  document.addEventListener("DOMContentLoaded", () => { start(); flush(); }, {once: true});
  if (document.readyState !== "loading") { start(); flush(); }
})();
