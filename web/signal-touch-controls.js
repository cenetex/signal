(function () {
  var forced = /(?:^|[?&])touch=1(?:&|$)/.test(window.location.search);
  var coarse = window.matchMedia && window.matchMedia("(pointer: coarse)").matches;
  var hasTouch = navigator.maxTouchPoints && navigator.maxTouchPoints > 0;
  var hasGamepadApi = typeof navigator.getGamepads === "function";
  var installTouchUi = forced || coarse || hasTouch;
  if (!installTouchUi && !hasGamepadApi) return;

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
    lineage: 17,
    lineageProof: 18,
    one: 20,
    two: 21,
    three: 22,
    four: 23,
    five: 24,
    auto: 30,
    back: 31,
    recoveryConfirm: 32
  };

  var RECOVERY_FLAG = {
    visible: 1 << 0,
    canConfirm: 1 << 1,
    canCancel: 1 << 2,
    confirming: 1 << 3,
    result: 1 << 4,
    success: 1 << 5
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
    canUpgradeTractor: 1 << 29,
    canLineage: 1 << 30,
    lineageOpen: 1 << 31
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
    17: ["l", "KeyL", 76],
    18: ["i", "KeyI", 73],
    20: ["1", "Digit1", 49],
    21: ["2", "Digit2", 50],
    22: ["3", "Digit3", 51],
    23: ["4", "Digit4", 52],
    24: ["5", "Digit5", 53],
    30: ["o", "KeyO", 79],
    31: ["Escape", "Escape", 27],
    32: ["Enter", "Enter", 13]
  };

  var controls = {};
  var controlsRoot = null;
  var leftGroup = null;
  var rightGroup = null;
  var secondaryGroup = null;
  var stationGroup = null;
  var recoveryGroup = null;

  function gameModule() {
    return window.SignalGameModule || window.Module;
  }

  function wasmCall(name, returnType, argTypes, args) {
    var m = gameModule();
    try {
      if (m && typeof m["_" + name] === "function") {
        return m["_" + name].apply(m, args || []);
      }
      if (!window.SignalGameWasmReady) return null;
      if (m && typeof m.ccall === "function") {
        return m.ccall(name, returnType || null, argTypes || [], args || []);
      }
    } catch (_) {
      return null;
    }
    return null;
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
    if (wasmCall("signal_mobile_key", null, ["number", "number"],
                 [action, down ? 1 : 0]) === null) {
      fallbackKey(action, down);
    }
  }

  function clearHeld() {
    Object.keys(controls).forEach(function (name) {
      var el = controls[name];
      if (el && el.signalTouchRelease) el.signalTouchRelease();
    });
    wasmCall("signal_mobile_clear", null, [], []);
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
    var flags = wasmCall("signal_mobile_control_flags", "number", [], []);
    return flags === null ? fallbackFlags() : (flags | 0);
  }

  function recoveryFlags() {
    var flags = wasmCall(
      "signal_legacy_recovery_ui_flags", "number", [], []);
    return flags === null ? 0 : (flags | 0);
  }

  function mobileDigitMask(flags) {
    if (!has(flags, FLAG.canDigits)) return 0;
    var mask = wasmCall("signal_mobile_digit_mask", "number", [], []);
    return mask === null ? 31 : (mask | 0);
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
      ".signal-touch-controls{position:fixed;inset:0;z-index:12;pointer-events:none;font-family:\"IBM Plex Mono\",\"SFMono-Regular\",ui-monospace,monospace;-webkit-user-select:none;user-select:none;--edge:max(12px,env(safe-area-inset-bottom));--side:max(12px,env(safe-area-inset-right));--left-side:max(12px,env(safe-area-inset-left));--top-edge:max(12px,env(safe-area-inset-top))}",
      ".signal-touch-controls *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}",
      ".signal-touch-button{pointer-events:auto;min-width:48px;min-height:44px;padding:0 7px;border:1px solid rgba(245,239,223,.22);border-radius:7px;background:rgba(8,7,6,.50);color:#f5efdf;font:700 10px/1.05 \"IBM Plex Mono\",\"SFMono-Regular\",ui-monospace,monospace;letter-spacing:0;text-transform:uppercase;box-shadow:0 6px 16px rgba(0,0,0,.24);backdrop-filter:blur(8px);touch-action:none;white-space:normal;overflow:hidden;word-break:break-word;transition:opacity .12s,border-color .12s,background .12s,color .12s}",
      ".signal-touch-stack[hidden]{display:none!important}",
      ".signal-touch-button:disabled,.signal-touch-button.is-disabled{opacity:.34;background:rgba(8,7,6,.28);border-color:rgba(245,239,223,.12);box-shadow:none;color:rgba(245,239,223,.52)}",
      ".signal-touch-button:active,.signal-touch-button.is-held{background:rgba(128,255,213,.18);border-color:rgba(128,255,213,.72);color:#dffff4}",
      ".signal-touch-button:disabled:active,.signal-touch-button.is-disabled:active{background:rgba(8,7,6,.28);border-color:rgba(245,239,223,.12);color:rgba(245,239,223,.52)}",
      ".signal-touch-stack{position:absolute;display:grid;gap:7px;pointer-events:none}",
      ".signal-touch-left{left:var(--left-side);bottom:var(--edge);grid-template-columns:repeat(2,58px);grid-template-rows:46px 58px}",
      ".signal-touch-left .boost{grid-column:1/span 2;grid-row:1;color:#91bcff}",
      ".signal-touch-left .left{grid-column:1;grid-row:2}.signal-touch-left .right{grid-column:2;grid-row:2}",
      ".signal-touch-left .left,.signal-touch-left .right{min-height:58px;color:#80ffd5;font-size:18px}",
      ".signal-touch-right{right:var(--side);bottom:var(--edge);grid-template-columns:60px 72px;grid-template-rows:52px 58px 58px 40px}",
      ".signal-touch-right .use{grid-column:1/span 2;grid-row:1;min-height:52px;color:#f7d389;border-color:rgba(247,211,137,.44);font-size:11px}",
      ".signal-touch-right .fire{grid-column:1;grid-row:2}.signal-touch-right .thrust{grid-column:2;grid-row:2}.signal-touch-right .tractor{grid-column:1;grid-row:3}.signal-touch-right .brake{grid-column:2;grid-row:3}.signal-touch-right .scan{grid-column:1;grid-row:4}.signal-touch-right .auto{grid-column:2;grid-row:4}",
      ".signal-touch-right .thrust,.signal-touch-right .brake{min-height:58px;color:#f5efdf}",
      ".signal-touch-right .fire,.signal-touch-right .tractor{min-height:58px}",
      ".signal-touch-right .fire{color:#f1b566}.signal-touch-right .tractor{color:#80ffd5}",
      ".signal-touch-right .scan,.signal-touch-right .auto{min-height:40px;font-size:9px;background:rgba(8,7,6,.36)}",
      ".signal-touch-secondary{right:var(--side);bottom:calc(var(--edge) + 232px);grid-template-columns:repeat(2,62px);grid-template-rows:38px}",
      ".signal-touch-secondary .plan{grid-column:1;grid-row:1}.signal-touch-secondary .cycle{grid-column:2;grid-row:1}",
      ".signal-touch-secondary .signal-touch-button{min-height:38px;font-size:9px;background:rgba(8,7,6,.36)}",
      ".signal-touch-station{left:50%;top:var(--top-edge);transform:translateX(-50%);width:min(380px,calc(100vw - 20px));grid-template-columns:repeat(7,minmax(0,1fr));grid-auto-rows:36px;justify-content:center;gap:5px}",
      ".signal-touch-station .signal-touch-button{min-width:0;min-height:38px;padding:0 5px;font-size:9px;background:rgba(8,7,6,.36)}",
      ".signal-touch-station .wide{grid-column:span 2;min-width:0}",
      ".signal-touch-station .tab{grid-column:1/span 2;grid-row:1}.signal-touch-station .page{grid-column:3/span 2;grid-row:1}.signal-touch-station .sell{grid-column:5/span 3;grid-row:1}",
      ".signal-touch-station .repair{grid-column:1/span 2;grid-row:2}.signal-touch-station .laser{grid-column:3/span 2;grid-row:2}.signal-touch-station .cargo{grid-column:5/span 3;grid-row:2}",
      ".signal-touch-station .tractorUpgrade{grid-column:1/span 2;grid-row:3}.signal-touch-station .one{grid-column:3;grid-row:3}.signal-touch-station .two{grid-column:4;grid-row:3}.signal-touch-station .three{grid-column:5;grid-row:3}.signal-touch-station .four{grid-column:6;grid-row:3}.signal-touch-station .five{grid-column:7;grid-row:3}",
      ".signal-touch-station .lineage{grid-column:1/span 3;grid-row:4}.signal-touch-station .lineageProof{grid-column:4/span 2;grid-row:4}.signal-touch-station .back{grid-column:6/span 2;grid-row:4}",
      ".signal-touch-recovery{left:50%;bottom:var(--edge);transform:translateX(-50%);width:min(360px,calc(100vw - 24px));grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}",
      ".signal-touch-recovery .signal-touch-button{min-width:0;min-height:52px;font-size:11px;background:rgba(8,7,6,.78)}",
      ".signal-touch-recovery .recovery-confirm{color:#dffff4;border-color:rgba(128,255,213,.72)}",
      ".signal-touch-recovery .recovery-cancel{color:#f7d389;border-color:rgba(247,211,137,.52)}",
      "@media (min-width:860px) and (pointer:fine){.signal-touch-controls{display:none}}",
      "@media (max-width:560px){.signal-touch-controls{--edge:max(12px,env(safe-area-inset-bottom));--side:max(10px,env(safe-area-inset-right));--left-side:max(10px,env(safe-area-inset-left));--top-edge:max(10px,env(safe-area-inset-top))}.signal-touch-button{min-width:44px;min-height:42px;font-size:9px}.signal-touch-left{grid-template-columns:repeat(2,54px);grid-template-rows:42px 54px;gap:6px}.signal-touch-left .left,.signal-touch-left .right{min-height:54px;font-size:17px}.signal-touch-right{grid-template-columns:56px 66px;grid-template-rows:48px 54px 54px 38px;gap:6px}.signal-touch-right .use{min-height:48px}.signal-touch-right .thrust,.signal-touch-right .brake,.signal-touch-right .fire,.signal-touch-right .tractor{min-height:54px}.signal-touch-secondary{bottom:calc(var(--edge) + 218px);grid-template-columns:repeat(2,58px);gap:6px}.signal-touch-station{width:calc(100vw - 20px);grid-auto-rows:34px}}",
      "@media (max-height:520px){.signal-touch-controls{--edge:max(8px,env(safe-area-inset-bottom));--side:max(8px,env(safe-area-inset-right));--left-side:max(8px,env(safe-area-inset-left));--top-edge:max(8px,env(safe-area-inset-top))}.signal-touch-left{grid-template-columns:repeat(2,52px);grid-template-rows:40px 50px}.signal-touch-right{grid-template-columns:52px 62px;grid-template-rows:44px 50px 50px 36px}.signal-touch-left .left,.signal-touch-left .right,.signal-touch-right .thrust,.signal-touch-right .brake,.signal-touch-right .fire,.signal-touch-right .tractor{min-height:50px}.signal-touch-secondary{bottom:calc(var(--edge) + 224px);grid-template-columns:repeat(2,54px)}.signal-touch-station{grid-auto-rows:32px}.signal-touch-station .signal-touch-button{min-height:32px}}"
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
      if (el.hidden || el.disabled) return;
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

  function setButton(name, enabled, label) {
    var el = controls[name];
    if (!el) return;
    if (label && el.textContent !== label) {
      el.textContent = label;
      el.setAttribute("aria-label", label);
    }
    if (!enabled && !el.disabled && el.signalTouchRelease) {
      el.signalTouchRelease();
    }
    el.disabled = !enabled;
    el.classList.toggle("is-disabled", !enabled);
    el.setAttribute("aria-disabled", enabled ? "false" : "true");
  }

  function reassertHeldControls() {
    Object.keys(controls).forEach(function (name) {
      var el = controls[name];
      if (!el || el.hidden || el.disabled || !el.classList.contains("is-held")) return;
      send(Number(el.dataset.action), true);
    });
  }

  function setGroupVisible(group, visible) {
    if (!group) return;
    if (!visible && !group.hidden) {
      Array.prototype.forEach.call(group.querySelectorAll(".signal-touch-button"), function (el) {
        if (el.signalTouchRelease) el.signalTouchRelease();
      });
    }
    group.hidden = !visible;
  }

  function setDigits(enabled, digitMask) {
    ["one", "two", "three", "four", "five"].forEach(function (name, index) {
      setButton(name, enabled && ((digitMask & (1 << index)) !== 0), String(index + 1));
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
    var recovery = recoveryFlags();
    var recoveryVisible = has(recovery, RECOVERY_FLAG.visible);
    if (controlsRoot) {
      controlsRoot.classList.toggle("is-recovery", recoveryVisible);
    }
    if (recoveryVisible) {
      setButton(
        "recoveryConfirm",
        has(recovery, RECOVERY_FLAG.canConfirm),
        has(recovery, RECOVERY_FLAG.confirming) ? "Verifying" : "Recover");
      setButton(
        "recoveryCancel",
        has(recovery, RECOVERY_FLAG.canCancel),
        has(recovery, RECOVERY_FLAG.result) ? "Closed" : "Leave Untouched");
      setGroupVisible(leftGroup, false);
      setGroupVisible(rightGroup, false);
      setGroupVisible(secondaryGroup, false);
      setGroupVisible(stationGroup, false);
      setGroupVisible(recoveryGroup, true);
      return;
    }

    setGroupVisible(recoveryGroup, false);
    setGroupVisible(leftGroup, true);
    setGroupVisible(rightGroup, true);
    setGroupVisible(secondaryGroup, true);

    var flags = mobileFlags();
    var flight = has(flags, FLAG.canFlight);
    var docked = has(flags, FLAG.docked);
    var planActive = has(flags, FLAG.planActive);
    var dockView = has(flags, FLAG.stationDock);
    var tradeView = has(flags, FLAG.stationTrade);
    var workView = has(flags, FLAG.stationWork);
    var yardView = has(flags, FLAG.stationYard);
    var lineageOpen = has(flags, FLAG.lineageOpen);

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
    setButton("page", docked && has(flags, FLAG.canPage), lineageOpen ? "Proof Page" : "More");
    setButton("sell", docked && has(flags, FLAG.canSell), workView ? "Deliver" : "Sell");
    setButton("repair", docked && dockView && has(flags, FLAG.canRepair), "Repair");
    setButton("laser", docked && dockView && has(flags, FLAG.canUpgradeMine), "Laser+");
    setButton("cargo", docked && dockView && has(flags, FLAG.canUpgradeHold), "Hold+");
    setButton("tractorUpgrade", docked && dockView && has(flags, FLAG.canUpgradeTractor), "Tow+");
    setButton("lineage", docked && tradeView && has(flags, FLAG.canLineage),
      lineageOpen ? "Next Cargo" : "Lineage");
    setButton("lineageProof", docked && tradeView && lineageOpen, "Story / Proof");
    setButton("back", docked && tradeView && lineageOpen, "Market");

    var digitMask = (tradeView || workView || yardView) ? mobileDigitMask(flags) : 0;
    setDigits(docked, digitMask);

    reassertHeldControls();
    setGroupVisible(stationGroup, docked);
  }

  var gamepadPrevConfirm = false;
  var gamepadPrevCancel = false;
  var gamepadRecoveryArmed = false;
  var gamepadRecoveryWasVisible = false;

  function firstConnectedGamepad() {
    if (!hasGamepadApi) return null;
    try {
      var pads = navigator.getGamepads() || [];
      for (var i = 0; i < pads.length; i++) {
        if (pads[i] && pads[i].connected !== false) return pads[i];
      }
    } catch (_) {}
    return null;
  }

  function pollRecoveryGamepad() {
    var recovery = recoveryFlags();
    var visible = has(recovery, RECOVERY_FLAG.visible);
    var pad = firstConnectedGamepad();
    var confirm = !!(pad && pad.buttons && pad.buttons[0] &&
      pad.buttons[0].pressed);
    var cancel = !!(pad && pad.buttons && pad.buttons[1] &&
      pad.buttons[1].pressed);

    if (!visible || !pad) {
      gamepadRecoveryArmed = false;
    } else {
      if (!gamepadRecoveryWasVisible) gamepadRecoveryArmed = false;
      if (!confirm && !cancel) gamepadRecoveryArmed = true;
      if (gamepadRecoveryArmed &&
          cancel && !gamepadPrevCancel &&
                 has(recovery, RECOVERY_FLAG.canCancel)) {
        tap(ACTION.back);
      } else if (gamepadRecoveryArmed &&
                 confirm && !gamepadPrevConfirm &&
                 has(recovery, RECOVERY_FLAG.canConfirm)) {
        tap(ACTION.recoveryConfirm);
      }
    }

    gamepadPrevConfirm = confirm;
    gamepadPrevCancel = cancel;
    gamepadRecoveryWasVisible = visible;
    window.requestAnimationFrame(pollRecoveryGamepad);
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
    leftGroup = left;
    addButton(left, "boost", "Boost", ACTION.boost, "hold", "boost");
    addButton(left, "left", "<", ACTION.left, "hold", "left");
    addButton(left, "right", ">", ACTION.right, "hold", "right");

    var right = document.createElement("div");
    right.className = "signal-touch-stack signal-touch-right";
    rightGroup = right;
    addButton(right, "use", "Use", ACTION.use, "tap", "use");
    addButton(right, "fire", "Mine", ACTION.fire, "hold", "fire");
    addButton(right, "thrust", "Accel", ACTION.thrust, "hold", "thrust");
    addButton(right, "tractor", "Tow", ACTION.tractor, "toggle", "tractor");
    addButton(right, "brake", "Brake", ACTION.brake, "hold", "brake");
    addButton(right, "scan", "Scan", ACTION.hail, "tap", "scan");
    addButton(right, "auto", "Auto", ACTION.auto, "tap", "auto");

    var secondary = document.createElement("div");
    secondary.className = "signal-touch-stack signal-touch-secondary";
    secondaryGroup = secondary;
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
    addButton(station, "lineage", "Lineage", ACTION.lineage, "tap", "lineage");
    addButton(station, "lineageProof", "Story / Proof", ACTION.lineageProof, "tap", "lineageProof");
    addButton(station, "back", "Market", ACTION.back, "tap", "back");

    var recovery = document.createElement("div");
    recovery.className = "signal-touch-stack signal-touch-recovery";
    addButton(
      recovery, "recoveryConfirm", "Recover",
      ACTION.recoveryConfirm, "tap", "recovery-confirm");
    addButton(
      recovery, "recoveryCancel", "Leave Untouched",
      ACTION.back, "tap", "recovery-cancel");

    root.appendChild(left);
    root.appendChild(right);
    root.appendChild(secondary);
    root.appendChild(station);
    root.appendChild(recovery);
    document.body.appendChild(root);

    stationGroup = station;
    recoveryGroup = recovery;
    Array.prototype.forEach.call(root.querySelectorAll(".signal-touch-button"), bindButton);
    refreshControls();
    window.setInterval(refreshControls, 120);
    window.requestAnimationFrame(pollRecoveryGamepad);

    document.addEventListener("visibilitychange", function () {
      if (document.hidden) clearHeld();
    });
    window.addEventListener("blur", clearHeld);
  }

  if (!installTouchUi) {
    window.requestAnimationFrame(pollRecoveryGamepad);
  } else if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", installControls);
  } else {
    installControls();
  }
})();
