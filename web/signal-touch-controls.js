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
    cargo: 15,
    tractorUpgrade: 16,
    one: 20,
    two: 21,
    three: 22,
    four: 23,
    five: 24,
    auto: 30
  };

  var FLAG = {
    docked: 1 << 0,
    inDockRange: 1 << 1,
    dockingApproach: 1 << 2,
    planActive: 1 << 3,
    planGhost: 1 << 4,
    towingScaffold: 1 << 5,
    targetModule: 1 << 6,
    targetDock: 1 << 7,
    inspectingTarget: 1 << 8,
    stationDock: 1 << 9,
    stationTrade: 1 << 10,
    stationWork: 1 << 11,
    stationYard: 1 << 12,
    hasCargo: 1 << 13,
    autopilotOn: 1 << 14,
    autopilotReady: 1 << 15,
    canFlight: 1 << 16,
    canMine: 1 << 17,
    canTractor: 1 << 18,
    canScan: 1 << 19,
    canUse: 1 << 20,
    canPlan: 1 << 21,
    canCycle: 1 << 22,
    canPage: 1 << 23,
    canSell: 1 << 24,
    canDigits: 1 << 25,
    canRepair: 1 << 26,
    canUpgradeMine: 1 << 27,
    canUpgradeHold: 1 << 28,
    canUpgradeTractor: 1 << 29
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
    15: ["c", "KeyC", 67],
    16: ["t", "KeyT", 84],
    20: ["1", "Digit1", 49],
    21: ["2", "Digit2", 50],
    22: ["3", "Digit3", 51],
    23: ["4", "Digit4", 52],
    24: ["5", "Digit5", 53],
    30: ["o", "KeyO", 79]
  };

  var controls = {};
  var groups = [];

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
    window.setTimeout(function () { send(action, false); }, 80);
  }

  function focusGame() {
    var c = canvas();
    if (c && c.focus) c.focus();
  }

  function has(flags, flag) {
    return (flags & flag) !== 0;
  }

  function fallbackFlags() {
    return FLAG.canFlight | FLAG.canMine | FLAG.canTractor |
      FLAG.canScan | FLAG.canUse | FLAG.canPlan | FLAG.autopilotReady;
  }

  function mobileFlags() {
    var m = gameModule();
    if (!m || typeof m.ccall !== "function") return fallbackFlags();
    try {
      return m.ccall("signal_mobile_control_flags", "number", [], []) | 0;
    } catch (_) {
      return fallbackFlags();
    }
  }

  function mobileDigitMask(flags) {
    var m = gameModule();
    if (!has(flags, FLAG.canDigits)) return 0;
    if (!m || typeof m.ccall !== "function") return 31;
    try {
      return m.ccall("signal_mobile_digit_mask", "number", [], []) | 0;
    } catch (_) {
      return 31;
    }
  }

  function button(label, action, mode, className, controlName) {
    var el = document.createElement("button");
    el.type = "button";
    el.className = "signal-touch-button" + (className ? " " + className : "");
    el.textContent = label;
    el.setAttribute("aria-label", label);
    el.dataset.action = String(action);
    el.dataset.mode = mode || "tap";
    el.dataset.control = controlName;
    controls[controlName] = el;
    return el;
  }

  function addButton(parent, name, label, action, mode, className) {
    var el = button(label, action, mode, className, name);
    parent.appendChild(el);
    return el;
  }

  function installStyles() {
    var style = document.createElement("style");
    style.textContent =
      ".signal-touch-controls{position:fixed;inset:0;z-index:12;pointer-events:none;font-family:\"IBM Plex Mono\",\"SFMono-Regular\",ui-monospace,monospace;-webkit-user-select:none;user-select:none}" +
      ".signal-touch-controls *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}" +
      ".signal-touch-button{pointer-events:auto;min-width:52px;min-height:48px;padding:0 8px;border:1px solid rgba(245,239,223,.24);border-radius:8px;background:rgba(8,7,6,.50);color:#f5efdf;font:700 11px/1.05 \"IBM Plex Mono\",\"SFMono-Regular\",ui-monospace,monospace;letter-spacing:0;text-transform:uppercase;box-shadow:0 8px 22px rgba(0,0,0,.28);backdrop-filter:blur(10px);touch-action:none;white-space:normal;overflow:hidden;word-break:break-word}" +
      ".signal-touch-button[hidden],.signal-touch-stack[hidden]{display:none!important}" +
      ".signal-touch-button:active,.signal-touch-button.is-held{background:rgba(128,255,213,.18);border-color:rgba(128,255,213,.72);color:#dffff4}" +
      ".signal-touch-stack{position:absolute;display:grid;gap:8px;pointer-events:none}" +
      ".signal-touch-left{left:max(14px,env(safe-area-inset-left));bottom:max(16px,env(safe-area-inset-bottom));grid-template-columns:repeat(2,64px);grid-template-rows:52px 62px}" +
      ".signal-touch-left .boost{grid-column:1/span 2;color:#91bcff}" +
      ".signal-touch-left .left,.signal-touch-left .right{min-height:62px;color:#80ffd5;font-size:20px}" +
      ".signal-touch-right{right:max(14px,env(safe-area-inset-right));bottom:max(16px,env(safe-area-inset-bottom));grid-template-columns:66px 78px;grid-template-rows:56px 62px 62px 46px}" +
      ".signal-touch-right .use{grid-column:1/span 2;min-height:56px;color:#f7d389;border-color:rgba(247,211,137,.46);font-size:12px}" +
      ".signal-touch-right .thrust,.signal-touch-right .brake{grid-column:2;min-height:62px;color:#f5efdf}" +
      ".signal-touch-right .fire,.signal-touch-right .tractor{grid-column:1;min-height:62px}" +
      ".signal-touch-right .fire{color:#f1b566}.signal-touch-right .tractor{color:#80ffd5}" +
      ".signal-touch-right .scan,.signal-touch-right .auto{min-height:46px;font-size:10px;background:rgba(8,7,6,.40)}" +
      ".signal-touch-secondary{right:max(14px,env(safe-area-inset-right));bottom:calc(max(16px,env(safe-area-inset-bottom)) + 260px);grid-template-columns:repeat(2,68px);grid-auto-rows:42px}" +
      ".signal-touch-secondary .signal-touch-button{min-height:42px;font-size:10px;background:rgba(8,7,6,.40)}" +
      ".signal-touch-station{left:50%;bottom:max(16px,env(safe-area-inset-bottom));transform:translateX(-50%);grid-template-columns:repeat(8,44px);grid-auto-rows:42px;justify-content:center}" +
      ".signal-touch-station .signal-touch-button{min-width:40px;min-height:42px;font-size:10px;background:rgba(8,7,6,.40)}" +
      ".signal-touch-station .wide{grid-column:span 2;min-width:88px}" +
      "@media (min-width:860px) and (pointer:fine){.signal-touch-controls{display:none}}" +
      "@media (max-width:560px){.signal-touch-left{grid-template-columns:repeat(2,58px);grid-template-rows:48px 58px;gap:7px}.signal-touch-right{grid-template-columns:60px 72px;grid-template-rows:52px 58px 58px 42px;gap:7px}.signal-touch-button{min-width:46px;min-height:44px;font-size:10px}.signal-touch-left .left,.signal-touch-left .right{min-height:58px;font-size:18px}.signal-touch-right .thrust,.signal-touch-right .brake,.signal-touch-right .fire,.signal-touch-right .tractor{min-height:58px}.signal-touch-secondary{bottom:calc(max(16px,env(safe-area-inset-bottom)) + 236px);grid-template-columns:repeat(2,64px);gap:7px}.signal-touch-station{width:calc(100vw - 24px);grid-template-columns:repeat(5,1fr);gap:6px}}" +
      "@media (max-height:520px){.signal-touch-secondary{right:max(14px,env(safe-area-inset-right));bottom:calc(max(12px,env(safe-area-inset-bottom)) + 224px)}.signal-touch-left,.signal-touch-right,.signal-touch-station{bottom:max(12px,env(safe-area-inset-bottom))}.signal-touch-left{grid-template-columns:repeat(2,56px);grid-template-rows:44px 54px}.signal-touch-right{grid-template-columns:56px 68px;grid-template-rows:48px 54px 54px 38px}.signal-touch-left .left,.signal-touch-left .right,.signal-touch-right .thrust,.signal-touch-right .brake,.signal-touch-right .fire,.signal-touch-right .tractor{min-height:54px}.signal-touch-station .signal-touch-button{min-height:38px}}";
    document.head.appendChild(style);
  }

  function bindButton(el) {
    var action = Number(el.dataset.action);
    var mode = el.dataset.mode;
    var pointerId = null;
    var held = false;

    function releaseHeld() {
      if (held) send(action, false);
      held = false;
      pointerId = null;
      el.classList.remove("is-held");
    }

    el.signalTouchRelease = releaseHeld;

    el.addEventListener("pointerdown", function (ev) {
      ev.preventDefault();
      ev.stopPropagation();
      if (el.hidden) return;
      focusGame();
      pointerId = ev.pointerId;
      if (el.setPointerCapture) el.setPointerCapture(pointerId);
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
      releaseHeld();
    }

    el.addEventListener("pointerup", release);
    el.addEventListener("pointercancel", release);
    el.addEventListener("lostpointercapture", releaseHeld);
  }

  function setButton(name, visible, label) {
    var el = controls[name];
    if (!el) return;
    if (label && el.textContent !== label) {
      el.textContent = label;
      el.setAttribute("aria-label", label);
    }
    if (!visible && !el.hidden && el.signalTouchRelease) {
      el.signalTouchRelease();
    }
    el.hidden = !visible;
  }

  function syncGroups() {
    groups.forEach(function (group) {
      var visible = Array.prototype.some.call(
        group.querySelectorAll(".signal-touch-button"),
        function (el) { return !el.hidden; }
      );
      group.hidden = !visible;
    });
  }

  function useLabel(flags) {
    if (has(flags, FLAG.docked)) return "Launch";
    if (has(flags, FLAG.planActive)) {
      return has(flags, FLAG.planGhost) ? "Lock" : "Place";
    }
    if (has(flags, FLAG.towingScaffold)) return "Place";
    if (has(flags, FLAG.targetDock)) return "Dock";
    if (has(flags, FLAG.targetModule)) {
      return has(flags, FLAG.inspectingTarget) ? "Close" : "Inspect";
    }
    if (has(flags, FLAG.inDockRange)) return "Dock";
    return "Use";
  }

  function refreshControls() {
    var flags = mobileFlags();
    var flight = has(flags, FLAG.canFlight);
    var docked = has(flags, FLAG.docked);
    var planActive = has(flags, FLAG.planActive);
    var dockView = has(flags, FLAG.stationDock);
    var tradeView = has(flags, FLAG.stationTrade);
    var workView = has(flags, FLAG.stationWork);
    var yardView = has(flags, FLAG.stationYard);

    setButton("left", flight, "<");
    setButton("right", flight, ">");
    setButton("boost", flight, "Boost");
    setButton("thrust", flight, "Accel");
    setButton("brake", flight, "Brake");

    setButton("use", has(flags, FLAG.canUse), useLabel(flags));
    setButton("fire", has(flags, FLAG.canMine), "Mine");
    setButton("tractor", has(flags, FLAG.canTractor), has(flags, FLAG.towingScaffold) ? "Release" : "Tow");
    setButton("scan", has(flags, FLAG.canScan), "Scan");
    setButton("auto", has(flags, FLAG.autopilotReady), has(flags, FLAG.autopilotOn) ? "Auto Off" : "Auto");
    setButton("plan", has(flags, FLAG.canPlan), planActive ? "Exit" : "Plan");
    setButton("cycle", has(flags, FLAG.canCycle), "Type");

    setButton("tab", docked, "View");
    setButton("page", has(flags, FLAG.canPage), "Page");
    setButton("sell", has(flags, FLAG.canSell), workView ? "Deliver" : "Sell");
    setButton("repair", dockView && has(flags, FLAG.canRepair), "Repair");
    setButton("laser", dockView && has(flags, FLAG.canUpgradeMine), "Laser");
    setButton("cargo", dockView && has(flags, FLAG.canUpgradeHold), "Cargo");
    setButton("tractorUpgrade", dockView && has(flags, FLAG.canUpgradeTractor), "Tractor");

    var digitMask = (tradeView || workView || yardView) ? mobileDigitMask(flags) : 0;
    setButton("one", (digitMask & 1) !== 0, "1");
    setButton("two", (digitMask & 2) !== 0, "2");
    setButton("three", (digitMask & 4) !== 0, "3");
    setButton("four", (digitMask & 8) !== 0, "4");
    setButton("five", (digitMask & 16) !== 0, "5");

    syncGroups();
  }

  function installControls() {
    if (document.querySelector(".signal-touch-controls")) return;
    installStyles();

    var root = document.createElement("div");
    root.className = "signal-touch-controls";
    root.setAttribute("aria-hidden", "false");

    var left = document.createElement("div");
    left.className = "signal-touch-stack signal-touch-left";
    addButton(left, "boost", "Boost", ACTION.boost, "hold", "boost");
    addButton(left, "left", "<", ACTION.left, "hold", "left");
    addButton(left, "right", ">", ACTION.right, "hold", "right");

    var right = document.createElement("div");
    right.className = "signal-touch-stack signal-touch-right";
    addButton(right, "use", "Use", ACTION.use, "tap", "use");
    addButton(right, "fire", "Mine", ACTION.fire, "hold", "fire");
    addButton(right, "thrust", "Accel", ACTION.thrust, "hold", "thrust");
    addButton(right, "tractor", "Tow", ACTION.tractor, "hold", "tractor");
    addButton(right, "brake", "Brake", ACTION.brake, "hold", "brake");
    addButton(right, "scan", "Scan", ACTION.hail, "tap", "scan");
    addButton(right, "auto", "Auto", ACTION.auto, "tap", "auto");

    var secondary = document.createElement("div");
    secondary.className = "signal-touch-stack signal-touch-secondary";
    addButton(secondary, "plan", "Plan", ACTION.plan, "tap", "plan");
    addButton(secondary, "cycle", "Type", ACTION.cycle, "tap", "cycle");

    var station = document.createElement("div");
    station.className = "signal-touch-stack signal-touch-station";
    addButton(station, "tab", "View", ACTION.tab, "tap", "wide");
    addButton(station, "page", "Page", ACTION.page, "tap", "wide");
    addButton(station, "sell", "Sell", ACTION.sell, "tap", "wide");
    addButton(station, "repair", "Repair", ACTION.cycle, "tap", "wide");
    addButton(station, "laser", "Laser", ACTION.fire, "tap", "wide");
    addButton(station, "cargo", "Cargo", ACTION.cargo, "tap", "wide");
    addButton(station, "tractorUpgrade", "Tractor", ACTION.tractorUpgrade, "tap", "wide");
    addButton(station, "one", "1", ACTION.one, "tap", "");
    addButton(station, "two", "2", ACTION.two, "tap", "");
    addButton(station, "three", "3", ACTION.three, "tap", "");
    addButton(station, "four", "4", ACTION.four, "tap", "");
    addButton(station, "five", "5", ACTION.five, "tap", "");

    root.appendChild(left);
    root.appendChild(right);
    root.appendChild(secondary);
    root.appendChild(station);
    document.body.appendChild(root);

    groups = [left, right, secondary, station];
    Array.prototype.forEach.call(root.querySelectorAll(".signal-touch-button"), bindButton);
    refreshControls();
    window.setInterval(refreshControls, 120);

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
