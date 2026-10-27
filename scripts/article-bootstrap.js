/* This source is embedded with Qt's installed qwebchannel.js before navigation. */
(() => {
  "use strict";
  const generation = /* GD_GENERATION */;
  const pending = [];
  let real = null;
  const forward = (method, args) => {
    if (real) real[method](...args, generation);
    else pending.push({method, args});
  };
  window.__gdDocumentGeneration = generation;
  window.__gdChannelReady = false;
  // Keep this facade permanently: article_maker and dictionaries use one argument.
  window.articleview = {
    onJsActiveArticleChanged(id) {
      forward("onJsActiveArticleChanged", [String(id)]);
      window.dispatchEvent(new Event("gd-active-article"));
    },
    onJsReadingState(state) { forward("onJsReadingState", [state]); },
    onJsFrameEvent(event) { forward("onJsFrameEvent", [event]); }
  };
  /* GD_WEBCHANNEL_SOURCE */
  new QWebChannel(qt.webChannelTransport, channel => {
    real = channel.objects.articleview;
    for (const call of pending.splice(0)) real[call.method](...call.args, generation);
    window.__gdChannelReady = true;
    if (window.gdOnBridgeReady) window.gdOnBridgeReady();
  });
})();
