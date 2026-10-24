/* Save evidence comes from the local writer's completed generation. */
Module['postRun'] = Module['postRun'] || [];
Module['postRun'].push(function () {
  var style = document.createElement('style');
  style.textContent = `
    #signal-save-status { position:fixed; z-index:12; top:24px; right:24px;
      max-width:calc(100vw - 48px); display:flex; gap:8px; align-items:center;
      color:#b5d6cc; background:rgba(8,12,13,.9); border:1px solid #33534c;
      border-radius:6px; padding:7px 10px; font:12px/1.4 monospace; }
    #signal-save-status[data-state="3"] { color:#f1b566; border-color:#a77a38; }
    #signal-save-status button { min-height:44px; padding:6px 12px;
      border:1px solid #80ffd5; border-radius:4px; color:#80ffd5;
      background:#112a25; font:inherit; cursor:pointer; touch-action:manipulation; }
    #signal-save-status button[hidden] { display:none; }
    #signal-save-status button:focus-visible { outline:2px solid #fff; outline-offset:2px; }
    @media(max-width:859px), (pointer:coarse) { #signal-save-status { top:auto; bottom:12px;
      left:calc(max(12px, env(safe-area-inset-left)) + 130px);
      right:calc(max(12px, env(safe-area-inset-right)) + 146px);
      max-width:none; box-sizing:border-box;
      font-size:10px; flex-direction:column; align-items:stretch; gap:4px; padding:6px; }
      #signal-save-status button { padding:6px 4px; } }
    @media(max-width:560px) { #signal-save-status {
      left:calc(max(10px, env(safe-area-inset-left)) + 120px);
      right:calc(max(10px, env(safe-area-inset-right)) + 134px); } }
    @media(max-height:520px) and (max-width:859px),
      (max-height:520px) and (pointer:coarse) { #signal-save-status {
      flex-direction:row; align-items:center; gap:8px; font-size:11px; }
      #signal-save-status button { flex-shrink:0; padding:6px 12px; } }
  `;
  document.head.appendChild(style);
  var panel = document.createElement('div');
  panel.id = 'signal-save-status';
  panel.setAttribute('aria-label', 'Save status');
  var label = document.createElement('span');
  label.setAttribute('role', 'status');
  label.setAttribute('aria-live', 'polite');
  var retry = document.createElement('button');
  retry.type = 'button';
  retry.textContent = 'Retry save';
  retry.hidden = true;
  panel.append(label, retry);
  document.body.appendChild(panel);
  retry.addEventListener('click', function () {
    Module.ccall('signal_save_local_world', 'number', [], []);
    document.getElementById('canvas').focus({ preventScroll:true });
    update();
  });
  function update() {
    if (!Module['_signal_save_status_json']) return;
    var status = JSON.parse(Module.ccall('signal_save_status_json', 'string', [], []));
    panel.dataset.state = String(status.state);
    panel.dataset.mode = status.mode;
    // Keep age updates quiet for screen readers; announce state changes.
    label.setAttribute('aria-live', label.dataset.state === String(status.state) ? 'off' : 'polite');
    label.dataset.state = String(status.state);
    if (label.textContent !== status.text) label.textContent = status.text;
    retry.hidden = !status.retry;
  }
  update();
  window.setInterval(update, 250);
});
