(function () {
  var forced = /(?:^|[?&])touch=1(?:&|$)/.test(window.location.search);
  var coarse = window.matchMedia && window.matchMedia("(pointer: coarse)").matches;
  var hasTouch = navigator.maxTouchPoints && navigator.maxTouchPoints > 0;
  if (!forced && !coarse && !hasTouch) return;

  var ACTION = {
    thrust: 1,
    brake: 2,
    left: 3,
    right: 4,
    fire: 5,
    tractor: 6,
    use: 7,
    hail: 8,
    boost: 9,
    plan: 10,
    cycle: 11,
    tab: 12,
    page: 13,
    sell: 14,
    one: 20,
    two: 21,
    three: 22,
    four: 23,
    five: 24,
    auto: 30,
    back: 31
  };

  var fallbackKeys = {
    1: ["w", "KeyW", 87],
    2: ["s", "KeyS", 83],
    3: ["a", "KeyA", 65],
    4: ["d", "KeyD", 68],
    5: ["m", "KeyM", 77],
    6: [" ", "Space", 32],
    7: ["e", "KeyE", 69],
    8: ["h", "KeyH", 72],
    9: ["Shift", "ShiftLeft", 16],
    10: ["b", "KeyB", 66],
    11: ["r", "KeyR", 82],
    12: ["Tab", "Tab", 9],
    13: ["f", "KeyF", 70],
    14: ["s", "KeyS", 83],
    20: ["1", "Digit1", 49],
    21: ["2", "Digit2", 50],
    22: ["3", "Digit3", 51],
    23: ["4", "Digit4", 52],
    24: ["5", "Digit5", 53],
    30: ["o", "KeyO", 79],
    31: ["Escape", "Escape", 27]
  };

  function gameModule() {
    return window.SignalGameModule || window.Module;
  }

  function canvas() {
    var m = gameModule();
    return (m && m.canvas) || document.getElementById("canvas");
  }

  function fallbackKey(action, down) {
    var def = fallbackKeys[action];
    if (!def) return;
    var target = canvas() || window;
    var ev = new KeyboardEvent(down ? "keydown" : "keyup", {
      key: def[0],
      code: def[1],
      keyCode: def[2],
      which: def[2],
      bubbles: true,
      cancelable: true
    });
    target.dispatchEvent(ev);
    window.dispatchEvent(ev);
  }

  function send(action, down) {
    var m = gameModule();
    if (m && typeof m.ccall === "function") {
      m.ccall("signal_mobile_key", null, ["number", "number"], [action, down ? 1 : 0]);
    } else {
      fallbackKey(action, down);
    }
  }

  function clearHeld() {
    var m = gameModule();
    if (m && typeof m.ccall === "function") {
      m.ccall("signal_mobile_clear", null, [], []);
    }
  }

  function tap(action) {
    send(action, true);
    window.setTimeout(function () { send(action, false); }, 70);
  }

  function focusGame() {
    var c = canvas();
    if (c && c.focus) c.focus();
  }

  function button(label, action, mode, extraClass) {
    var el = document.createElement("button");
    el.type = "button";
    el.className = "signal-touch-button" + (extraClass ? " " + extraClass : "");
    el.textContent = label;
    el.setAttribute("aria-label", label);
    el.dataset.action = String(action);
    el.dataset.mode = mode || "tap";
    return el;
  }

  function padButton(label, action, cls) {
    return button(label, action, "hold", "signal-touch-pad-button " + cls);
  }

  function installStyles() {
    var style = document.createElement("style");
    style.textContent =
      ".signal-touch-controls{position:fixed;inset:0;z-index:12;pointer-events:none;font-family:\"IBM Plex Mono\",\"SFMono-Regular\",ui-monospace,monospace;-webkit-user-select:none;user-select:none}" +
      ".signal-touch-controls *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}" +
      ".signal-touch-button{pointer-events:auto;min-width:52px;min-height:52px;border:1px solid rgba(245,239,223,.22);border-radius:8px;background:rgba(8,7,6,.48);color:#f5efdf;font:600 11px/1 \"IBM Plex Mono\",\"SFMono-Regular\",ui-monospace,monospace;letter-spacing:.08em;text-transform:uppercase;box-shadow:0 8px 22px rgba(0,0,0,.28);backdrop-filter:blur(10px);touch-action:none}" +
      ".signal-touch-button:active,.signal-touch-button.is-held{background:rgba(128,255,213,.18);border-color:rgba(128,255,213,.72);color:#dffff4}" +
      ".signal-touch-pad{position:absolute;left:max(14px,env(safe-area-inset-left));bottom:max(16px,env(safe-area-inset-bottom));display:grid;grid-template-columns:repeat(3,58px);grid-template-rows:repeat(3,58px);gap:8px;pointer-events:none}" +
      ".signal-touch-pad .up{grid-column:2;grid-row:1}.signal-touch-pad .left{grid-column:1;grid-row:2}.signal-touch-pad .right{grid-column:3;grid-row:2}.signal-touch-pad .down{grid-column:2;grid-row:3}" +
      ".signal-touch-pad-button{font-size:18px;color:#80ffd5}" +
      ".signal-touch-actions{position:absolute;right:max(14px,env(safe-area-inset-right));bottom:max(16px,env(safe-area-inset-bottom));display:grid;grid-template-columns:repeat(2,68px);gap:8px;pointer-events:none}" +
      ".signal-touch-actions .wide{grid-column:span 2;min-height:56px}" +
      ".signal-touch-actions .fire{min-height:68px;color:#f1b566}.signal-touch-actions .tractor{min-height:68px;color:#80ffd5}" +
      ".signal-touch-rail{position:absolute;left:50%;bottom:max(16px,env(safe-area-inset-bottom));transform:translateX(-50%);display:flex;gap:6px;align-items:center;justify-content:center;pointer-events:none}" +
      ".signal-touch-rail .signal-touch-button{min-width:42px;min-height:42px;font-size:10px;background:rgba(8,7,6,.36)}" +
      ".signal-touch-top{position:absolute;left:50%;top:max(10px,env(safe-area-inset-top));transform:translateX(-50%);display:flex;gap:8px;pointer-events:none}" +
      ".signal-touch-top .signal-touch-button{min-height:42px;min-width:52px;font-size:10px}" +
      "@media (min-width:860px) and (pointer:fine){.signal-touch-controls{display:none}}" +
      "@media (max-width:560px){.signal-touch-pad{grid-template-columns:repeat(3,52px);grid-template-rows:repeat(3,52px);gap:6px}.signal-touch-actions{grid-template-columns:repeat(2,62px);gap:6px}.signal-touch-button{min-width:48px;min-height:48px}.signal-touch-rail{bottom:calc(max(16px,env(safe-area-inset-bottom)) + 166px)}.signal-touch-rail .signal-touch-button{min-width:38px;min-height:38px}}" +
      "@media (max-height:520px){.signal-touch-top{display:none}.signal-touch-rail{bottom:max(12px,env(safe-area-inset-bottom))}.signal-touch-pad{bottom:max(12px,env(safe-area-inset-bottom))}.signal-touch-actions{bottom:max(12px,env(safe-area-inset-bottom))}}";
    document.head.appendChild(style);
  }

  function bindButton(el) {
    var action = Number(el.dataset.action);
    var mode = el.dataset.mode;
    var pointerId = null;
    var held = false;

    el.addEventListener("pointerdown", function (ev) {
      ev.preventDefault();
      ev.stopPropagation();
      focusGame();
      pointerId = ev.pointerId;
      el.setPointerCapture(pointerId);
      if (mode === "hold") {
        held = true;
        el.classList.add("is-held");
        send(action, true);
      } else {
        tap(action);
      }
    });

    function release(ev) {
      if (pointerId !== null && ev.pointerId !== pointerId) return;
      ev.preventDefault();
      ev.stopPropagation();
      if (held) {
        send(action, false);
        held = false;
        el.classList.remove("is-held");
      }
      pointerId = null;
    }

    el.addEventListener("pointerup", release);
    el.addEventListener("pointercancel", release);
    el.addEventListener("lostpointercapture", function () {
      if (held) send(action, false);
      held = false;
      pointerId = null;
      el.classList.remove("is-held");
    });
  }

  function installControls() {
    if (document.querySelector(".signal-touch-controls")) return;
    installStyles();

    var root = document.createElement("div");
    root.className = "signal-touch-controls";
    root.setAttribute("aria-hidden", "false");

    var top = document.createElement("div");
    top.className = "signal-touch-top";
    top.appendChild(button("Back", ACTION.back));
    top.appendChild(button("Plan", ACTION.plan));
    top.appendChild(button("Cycle", ACTION.cycle));

    var pad = document.createElement("div");
    pad.className = "signal-touch-pad";
    pad.appendChild(padButton("^", ACTION.thrust, "up"));
    pad.appendChild(padButton("<", ACTION.left, "left"));
    pad.appendChild(padButton(">", ACTION.right, "right"));
    pad.appendChild(padButton("v", ACTION.brake, "down"));

    var actions = document.createElement("div");
    actions.className = "signal-touch-actions";
    actions.appendChild(button("Fire", ACTION.fire, "hold", "fire"));
    actions.appendChild(button("Tow", ACTION.tractor, "hold", "tractor"));
    actions.appendChild(button("Use", ACTION.use));
    actions.appendChild(button("Scan", ACTION.hail));
    actions.appendChild(button("Boost", ACTION.boost, "hold"));
    actions.appendChild(button("Auto", ACTION.auto));

    var rail = document.createElement("div");
    rail.className = "signal-touch-rail";
    rail.appendChild(button("Tab", ACTION.tab));
    rail.appendChild(button("Page", ACTION.page));
    rail.appendChild(button("Sell", ACTION.sell));
    rail.appendChild(button("1", ACTION.one));
    rail.appendChild(button("2", ACTION.two));
    rail.appendChild(button("3", ACTION.three));
    rail.appendChild(button("4", ACTION.four));
    rail.appendChild(button("5", ACTION.five));

    root.appendChild(top);
    root.appendChild(pad);
    root.appendChild(actions);
    root.appendChild(rail);
    document.body.appendChild(root);
    Array.prototype.forEach.call(root.querySelectorAll(".signal-touch-button"), bindButton);

    document.addEventListener("visibilitychange", function () {
      if (document.hidden) clearHeld();
    });
    window.addEventListener("blur", clearHeld);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", installControls);
  } else {
    installControls();
  }
})();
