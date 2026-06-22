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
  var controlsRoot = null;

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
    Object.keys(controls).forEach(function (name) {
      var el = controls[name];
      if (el && el.signalTouchRelease) el.signalTouchRelease();
    });
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
    style.textContent = [
      ".signal-touch-controls{position:fixed;inset:0;z-index:12;pointer-events:none;font-family:\"IBM Plex Mono\",\"SFMono-Regular\",ui-monospace,monospace;-webkit-user-select:none;user-select:none}",
      ".signal-touch-controls *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}",
      ".signal-touch-button{pointer-events:auto;min-width:48px;min-height:44px;padding:0 7px;border:1px solid rgba(245,239,223,.22);border-radius:7px;background:rgba(8,7,6,.44);color:#f5efdf;font:700 10px/1.05 \"IBM Plex Mono\",\"SFMono-Regular\",ui-monospace,monospace;letter-spacing:0;text-transform:uppercase;box-shadow:0 6px 16px rgba(0,0,0,.24);backdrop-filter:blur(8px);touch-action:none;white-space:normal;overflow:hidden;word-break:break-word}",
      ".signal-touch-button[hidden],.signal-touch-stack[hidden]{display:none!important}",
      ".signal-touch-button:active,.signal-touch-button.is-held{background:rgba(128,255,213,.18);border-color:rgba(128,255,213,.72);color:#dffff4}",
      ".signal-touch-stack{position:absolute;display:grid;gap:7px;pointer-events:none}",
      ".signal-touch-left{left:max(12px,env(safe-area-inset-left));bottom:max(12px,env(safe-area-inset-bottom));grid-template-columns:repeat(2,58px);grid-template-rows:46px 58px}",
      ".signal-touch-left .boost{grid-column:1/span 2;color:#91bcff}",
      ".signal-touch-left .left,.signal-touch-left .right{min-height:58px;color:#80ffd5;font-size:18px}",
      ".signal-touch-right{right:max(12px,env(safe-area-inset-right));bottom:max(12px,env(safe-area-inset-bottom));grid-template-columns:60px 72px;grid-template-rows:52px 58px 58px 40px}",
      ".signal-touch-right .use{grid-column:1/span 2;min-height:52px;color:#f7d389;border-color:rgba(247,211,137,.44);font-size:11px}",
      ".signal-touch-right .thrust,.signal-touch-right .brake{grid-column:2;min-height:58px;color:#f5efdf}",
      ".signal-touch-right .fire,.signal-touch-right .tractor{grid-column:1;min-height:58px}",
      ".signal-touch-right .fire{color:#f1b566}.signal-touch-right .tractor{color:#80ffd5}",
      ".signal-touch-right .scan,.signal-touch-right .auto{min-height:40px;font-size:9px;background:rgba(8,7,6,.36)}",
      ".signal-touch-secondary{right:max(12px,env(safe-area-inset-right));bottom:calc(max(12px,env(safe-area-inset-bottom)) + 232px);grid-template-columns:repeat(2,62px);grid-auto-rows:38px}",
      ".signal-touch-secondary .signal-touch-button{min-height:38px;font-size:9px;background:rgba(8,7,6,.36)}",
      ".signal-touch-station{left:50%;bottom:max(10px,env(safe-area-inset-bottom));transform:translateX(-50%);width:min(380px,calc(100vw - 20px));grid-template-columns:repeat(8,minmax(0,1fr));grid-auto-rows:38px;justify-content:center}",
      ".signal-touch-station .signal-touch-button{min-width:0;min-height:38px;padding:0 5px;font-size:9px;background:rgba(8,7,6,.36)}",
      ".signal-touch-station .wide{grid-column:span 2;min-width:0}",
      ".signal-touch-controls.is-docked .signal-touch-right{top:max(10px,env(safe-area-inset-top));right:max(10px,env(safe-area-inset-right));bottom:auto;grid-template-columns:repeat(3,58px);grid-template-rows:38px;gap:6px}",
      ".signal-touch-controls.is-docked .signal-touch-right .use{grid-column:auto;min-height:38px;font-size:9px}",
      ".signal-touch-controls.is-docked .signal-touch-right .scan,.signal-touch-controls.is-docked .signal-touch-right .auto{min-height:38px;font-size:9px}",
      ".signal-touch-controls.is-docked .signal-touch-station{bottom:max(8px,env(safe-area-inset-bottom));width:min(360px,calc(100vw - 20px));grid-template-columns:repeat(6,minmax(0,1fr));grid-auto-rows:36px;gap:5px}",
      ".signal-touch-controls.is-docked .signal-touch-station .signal-touch-button{min-height:36px;font-size:9px}",
      "@media (min-width:860px) and (pointer:fine){.signal-touch-controls{display:none}}",
      "@media (max-width:560px){.signal-touch-button{min-width:44px;min-height:42px;font-size:9px}.signal-touch-left{left:max(10px,env(safe-area-inset-left));grid-template-columns:repeat(2,54px);grid-template-rows:42px 54px;gap:6px}.signal-touch-left .left,.signal-touch-left .right{min-height:54px;font-size:17px}.signal-touch-right{right:max(10px,env(safe-area-inset-right));grid-template-columns:56px 66px;grid-template-rows:48px 54px 54px 38px;gap:6px}.signal-touch-right .use{min-height:48px}.signal-touch-right .thrust,.signal-touch-right .brake,.signal-touch-right .fire,.signal-touch-right .tractor{min-height:54px}.signal-touch-secondary{right:max(10px,env(safe-area-inset-right));bottom:calc(max(12px,env(safe-area-inset-bottom)) + 218px);grid-template-columns:repeat(2,58px);gap:6px}.signal-touch-station{width:calc(100vw - 20px);grid-template-columns:repeat(5,minmax(0,1fr));gap:5px}}",
      "@media (max-height:520px){.signal-touch-left,.signal-touch-right,.signal-touch-station{bottom:max(8px,env(safe-area-inset-bottom))}.signal-touch-left{grid-template-columns:repeat(2,52px);grid-template-rows:40px 50px}.signal-touch-right{grid-template-columns:52px 62px;grid-template-rows:44px 50px 50px 36px}.signal-touch-left .left,.signal-touch-left .right,.signal-touch-right .thrust,.signal-touch-right .brake,.signal-touch-right .fire,.signal-touch-right .tractor{min-height:50px}.signal-touch-secondary{right:max(8px,env(safe-area-inset-right));bottom:calc(max(8px,env(safe-area-inset-bottom)) + 224px);grid-template-columns:repeat(2,54px)}.signal-touch-station .signal-touch-button{min-height:34px}.signal-touch-controls.is-docked .signal-touch-right{top:max(8px,env(safe-area-inset-top));grid-template-columns:repeat(3,54px);grid-template-rows:34px}.signal-touch-controls.is-docked .signal-touch-right .use,.signal-touch-controls.is-docked .signal-touch-right .scan,.signal-touch-controls.is-docked .signal-touch-right .auto{min-height:34px}.signal-touch-controls.is-docked .signal-touch-station{bottom:max(6px,env(safe-area-inset-bottom));grid-auto-rows:34px}}"
    ].join("");
    document.head.appendChild(style);
  }

  function bindButton(el) {
    var action = Number(el.dataset.action);
    var mode = el.dataset.mode;
    var pointerId = null;
    var held = false;
    var toggled = false;

    function releaseHeld() {
      if (held) send(action, false);
      held = false;
      toggled = false;
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
      } else if (mode === "toggle") {
        toggled = !toggled;
        if (toggled) {
          held = true;
          el.classList.add("is-held");
          send(action, true);
        } else {
          releaseHeld();
        }
      } else {
        tap(action);
      }
    });

    function release(ev) {
      if (pointerId !== null && ev.pointerId !== pointerId) return;
      ev.preventDefault();
      ev.stopPropagation();
      if (mode === "hold") {
        releaseHeld();
      } else {
        pointerId = null;
      }
    }

    el.addEventListener("pointerup", release);
    el.addEventListener("pointercancel", release);
    el.addEventListener("lostpointercapture", function () {
      if (mode === "hold") {
        releaseHeld();
      } else {
        pointerId = null;
      }
    });
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

  function reassertHeldControls() {
    Object.keys(controls).forEach(function (name) {
      var el = controls[name];
      if (!el || el.hidden || !el.classList.contains("is-held")) return;
      send(Number(el.dataset.action), true);
    });
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

    if (controlsRoot) {
      controlsRoot.classList.toggle("is-docked", docked);
      controlsRoot.classList.toggle("is-flight", flight && !docked);
      controlsRoot.classList.toggle("is-plan", planActive);
      controlsRoot.classList.toggle("is-station", docked || tradeView || workView || yardView);
    }

    setButton("left", flight, "<");
    setButton("right", flight, ">");
    setButton("boost", flight, "Boost");
    setButton("thrust", flight, "Accel");
    setButton("brake", flight, "Brake");

    var tractorActive = controls.tractor && controls.tractor.classList.contains("is-held");
    var tractorLabel = has(flags, FLAG.towingScaffold) ? "Release" :
      (tractorActive ? "Tow On" : "Tow");

    setButton("use", has(flags, FLAG.canUse), useLabel(flags));
    setButton("fire", has(flags, FLAG.canMine), "Mine");
    setButton("tractor", has(flags, FLAG.canTractor), tractorLabel);
    setButton("scan", has(flags, FLAG.canScan), "Scan");
    setButton("auto", has(flags, FLAG.autopilotReady), has(flags, FLAG.autopilotOn) ? "Auto Off" : "Auto");
    setButton("plan", has(flags, FLAG.canPlan), planActive ? "Exit" : "Plan");
    setButton("cycle", has(flags, FLAG.canCycle), "Type");

    setButton("tab", docked, "Panel");
    setButton("page", has(flags, FLAG.canPage), "More");
    setButton("sell", has(flags, FLAG.canSell), workView ? "Deliver" : "Sell");
    setButton("repair", dockView && has(flags, FLAG.canRepair), "Repair");
    setButton("laser", dockView && has(flags, FLAG.canUpgradeMine), "Laser+");
    setButton("cargo", dockView && has(flags, FLAG.canUpgradeHold), "Hold+");
    setButton("tractorUpgrade", dockView && has(flags, FLAG.canUpgradeTractor), "Tow+");

    var digitMask = (tradeView || workView || yardView) ? mobileDigitMask(flags) : 0;
    setButton("one", (digitMask & 1) !== 0, "1");
    setButton("two", (digitMask & 2) !== 0, "2");
    setButton("three", (digitMask & 4) !== 0, "3");
    setButton("four", (digitMask & 8) !== 0, "4");
    setButton("five", (digitMask & 16) !== 0, "5");

    reassertHeldControls();
    syncGroups();
  }

  function installControls() {
    if (document.querySelector(".signal-touch-controls")) return;
    installStyles();

    var root = document.createElement("div");
    root.className = "signal-touch-controls";
    root.setAttribute("aria-hidden", "false");
    controlsRoot = root;

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
    addButton(right, "tractor", "Tow", ACTION.tractor, "toggle", "tractor");
    addButton(right, "brake", "Brake", ACTION.brake, "hold", "brake");
    addButton(right, "scan", "Scan", ACTION.hail, "tap", "scan");
    addButton(right, "auto", "Auto", ACTION.auto, "tap", "auto");

    var secondary = document.createElement("div");
    secondary.className = "signal-touch-stack signal-touch-secondary";
    addButton(secondary, "plan", "Plan", ACTION.plan, "tap", "plan");
    addButton(secondary, "cycle", "Type", ACTION.cycle, "tap", "cycle");

    var station = document.createElement("div");
    station.className = "signal-touch-stack signal-touch-station";
    addButton(station, "tab", "Panel", ACTION.tab, "tap", "wide");
    addButton(station, "page", "More", ACTION.page, "tap", "wide");
    addButton(station, "sell", "Sell", ACTION.sell, "tap", "wide");
    addButton(station, "repair", "Repair", ACTION.cycle, "tap", "wide");
    addButton(station, "laser", "Laser+", ACTION.fire, "tap", "wide");
    addButton(station, "cargo", "Hold+", ACTION.cargo, "tap", "wide");
    addButton(station, "tractorUpgrade", "Tow+", ACTION.tractorUpgrade, "tap", "wide");
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
