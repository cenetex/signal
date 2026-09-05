/* Mount the local world before main. IDBFS publishes each complete generation
 * in one IndexedDB transaction. A Web Lock owns the origin's local authority. */
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  var storage = Module.signalLocalPersistence = { state: 0 };
  var params = new URLSearchParams(window.location.search);
  var remote = !params.has('singleplayer') &&
    (params.get('server') || window.SIGNAL_SERVER);
  if (remote) return;
  addRunDependency('signal-local-world');
  function fail(message, error) {
    storage.state = -1;
    console.error('[local-save] ' + message, error || '');
    var loading = document.getElementById('loading');
    if (loading) loading.textContent = message;
  }
  function mount() {
    try {
      FS.mkdir('/signal-client');
      FS.mount(IDBFS, {}, '/signal-client');
      FS.syncfs(true, function (error) {
        if (error) {
          fail('Local save recovery required. Check browser storage, then reload.', error);
          return;
        }
        storage.state = 1;
        removeRunDependency('signal-local-world');
      });
    } catch (error) {
      fail('Local save storage needs access. Check browser storage, then reload.', error);
    }
  }
  storage.flush = function () {
    if (storage.state === 2) return;
    storage.state = 2;
    FS.syncfs(false, function (error) {
      storage.state = error ? -1 : 1;
      if (error) console.warn('[local-save] Save retry required.', error);
    });
  };
  function checkpoint() {
    if (Module['_signal_save_local_world']) Module['_signal_save_local_world']();
  }
  document.addEventListener('visibilitychange', function () {
    if (document.visibilityState === 'hidden') checkpoint();
  });
  window.addEventListener('pagehide', checkpoint);
  if (!navigator.locks) {
    fail('Local saves need a secure browser connection. Use HTTPS or localhost.');
    return;
  }
  navigator.locks.request('signal-local-world', { ifAvailable: true }, function (lock) {
    if (!lock) {
      fail('Local world is open in another tab. Close that tab, then reload.');
      return;
    }
    mount();
    return new Promise(function () {});
  }).catch(function (error) {
    fail('Local save storage needs access. Check browser storage, then reload.', error);
  });
});
