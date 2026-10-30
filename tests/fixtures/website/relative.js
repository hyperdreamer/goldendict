/*
 * GoldenDict remote-website fixture script.
 *
 * Fetches a resource relative to the *real* document origin. The fixture server
 * records the request, and the resolved marker is written into #fetch-marker so
 * a suite can prove the frame document ran script with its own origin (and its
 * non-default port) instead of gdlookup.
 */
(function () {
  var marker = document.getElementById('fetch-marker');
  fetch('./data.json')
    .then(function (response) { return response.json(); })
    .then(function (data) {
      if (marker) {
        marker.setAttribute('data-fetch', data.marker);
        marker.textContent = data.marker;
      }
      document.documentElement.setAttribute('data-origin-fetch', data.marker);
    })
    .catch(function (error) {
      if (marker) { marker.setAttribute('data-fetch', 'failed'); }
    });
})();
