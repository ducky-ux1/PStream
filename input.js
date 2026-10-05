/**
 * PStream DualSense & Remote Control Input Engine for PlayStation 5
 * Handles:
 * 1. DualSense Gamepad API polling (standard & webkit interfaces, buttons 0-17 + axes)
 * 2. Hardware Keyboard Event mapping (DualSense emulated keys & shortcuts)
 * 3. Native DOM contextmenu event interception (Hardware Options button)
 * 4. Hold-to-repeat for D-pad and analog sticks
 * 5. Remote server telemetry logging (/log?msg=...)
 */
var Input = (function () {
    "use strict";

    var KEY = {
        CROSS:    13,  // DualSense X / Enter
        CIRCLE:   27,  // DualSense O / Escape
        LEFT:     37,  // D-Pad Left / ArrowLeft
        UP:       38,  // D-Pad Up / ArrowUp
        RIGHT:    39,  // D-Pad Right / ArrowRight
        DOWN:     40,  // D-Pad Down / ArrowDown
        TRIANGLE: 112, // DualSense Triangle / F1
        SQUARE:   113, // DualSense Square / F2
        L1:       116, // Left Shoulder (L1) - F5 (116)
        R1:       117, // Right Shoulder (R1) - F6 (117) / F4 (115)
        OPTIONS:  114, // Physical DualSense Options / Start Button - F3 (114) / F10 (121) / ContextMenu (93)

        REWIND:   133,
        FAST_FWD: 134,
        PLAY_PAUSE: 135
    };

    var HANDLED = [13, 27, 8, 37, 38, 39, 40, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 33, 34, 93, 18, 133, 134, 135];
    var listeners = [];

    // Hold-to-repeat tracking
    var buttonTimestamps = {};
    var INITIAL_REPEAT_MS = 280;
    var REPEAT_INTERVAL_MS = 130;

    // Last remote log timestamp to prevent network spam
    var lastLogTime = 0;
    function remoteLog(msg) {
        var now = Date.now();
        if (now - lastLogTime < 80) return; // limit to ~12 logs/sec max
        lastLogTime = now;
        try {
            var xhr = new XMLHttpRequest();
            xhr.open("GET", "/log?msg=" + encodeURIComponent("[INPUT] " + msg), true);
            xhr.send();
        } catch (e) { }
    }

    function isButtonPressed(btn) {
        if (!btn) return false;
        if (typeof btn === "boolean") return btn;
        if (typeof btn === "number") return btn > 0.5;
        if (typeof btn === "object") {
            return !!(btn.pressed === true || (typeof btn.value === "number" && btn.value > 0.5));
        }
        return false;
    }

    function init() {
        // Ensure window has focus so keyboard events are captured
        try { window.focus(); } catch (e) { }
        window.addEventListener("click", function () {
            try { window.focus(); } catch (e) { }
        });

        // 1. Capture hardware contextmenu (Physical DualSense Options Button)
        window.addEventListener("contextmenu", function (e) {
            e.preventDefault();
            e.stopPropagation();
            remoteLog("Hardware Options event: contextmenu captured");
            dispatch(KEY.OPTIONS);
            return false;
        }, true);

        // 2. Capture keyboard events on both window and document
        function handleKeyDown(e) {
            var raw = e.keyCode || e.which;
            var keyStr = e.key || "";
            var code = 0;

            var activeEl = document.activeElement;
            var isInputActive = !!(activeEl && (activeEl.tagName === "INPUT" || activeEl.tagName === "TEXTAREA" || activeEl.id === "search-input"));

            if (isInputActive) {
                // 1. Backspace & Delete keys: ALWAYS allow native character deletion!
                // NEVER preventDefault, NEVER blur, NEVER dispatch - let browser delete characters!
                if (raw === 8 || raw === 46 || keyStr === "Backspace" || keyStr === "Delete") {
                    return;
                }
                // 2. Cursor navigation inside input: allow ArrowLeft & ArrowRight
                if (raw === 37 || raw === 39 || keyStr === "ArrowLeft" || keyStr === "ArrowRight") {
                    return;
                }
                // 3. Enter key: commits search, closes OSK so results can be browsed
                if (raw === 13 || keyStr === "Enter") {
                    activeEl.blur();
                    e.preventDefault();
                    e.stopPropagation();
                    if (window.onSearchCommit) {
                        window.onSearchCommit();
                    }
                    return;
                }
                // 4. Circle / Escape key: dismisses OSK
                if (raw === 27 || keyStr === "Escape") {
                    activeEl.blur();
                    e.preventDefault();
                    e.stopPropagation();
                    return;
                }
                // 5. D-pad Down: exits search input into search results
                if (raw === 40 || keyStr === "ArrowDown") {
                    activeEl.blur();
                    // fall through to dispatch KEY.DOWN so browse navigates down
                } else if (raw !== 116 && raw !== 117 && raw !== 33 && raw !== 34) {
                    // Regular typing keys (letters, numbers, space, punctuation)
                    // Let native input handle text without interception!
                    return;
                }
            }

            // DualSense L1 button emits F5 (116) on PS5 WebKit, PageUp (33), '[' (219), 'q' (81), or '-' (109)
            if (raw === 116 || raw === 33 || raw === 219 || raw === 81 || raw === 109 ||
                keyStr === "F5" || keyStr === "PageUp" || keyStr === "[" || keyStr === "ChannelDown") {
                code = KEY.L1;
            }
            // DualSense R1 button emits F6 (117) on PS5 WebKit, F4 (115), PageDown (34), ']' (221), 'F8' (119), 'e' (69), or '+' (107)
            else if (raw === 117 || raw === 115 || raw === 34 || raw === 221 || raw === 119 || raw === 69 || raw === 107 ||
                     keyStr === "F6" || keyStr === "F4" || keyStr === "PageDown" || keyStr === "]" || keyStr === "ChannelUp") {
                code = KEY.R1;
            }
            // DualSense Options / Start button emits F3 (114) on PS5 WebKit, F10 (121), ContextMenu (93), Menu (18), F9 (120), F11 (122), F12 (123)
            else if (raw === 114 || raw === 121 || raw === 93 || raw === 18 || raw === 120 || raw === 122 || raw === 123 ||
                     keyStr === "F3" || keyStr === "ContextMenu" || keyStr === "F10" || keyStr === "Menu" || keyStr === "AppMenu") {
                code = KEY.OPTIONS;
            }
            // DualSense Square button emits F2 (113) or 's' (83)
            else if (raw === 113 || keyStr === "F2") {
                code = KEY.SQUARE;
            }
            // DualSense Triangle button emits F1 (112) or 't' (84)
            else if (raw === 112 || keyStr === "F1") {
                code = KEY.TRIANGLE;
            }
            // Standard action buttons
            else if (raw === 13 || keyStr === "Enter" || keyStr === "Select" || keyStr === " ") {
                code = KEY.CROSS;
            } else if (raw === 27 || keyStr === "Escape" || (!isInputActive && (raw === 8 || keyStr === "Backspace"))) {
                code = KEY.CIRCLE;
            } else if (raw === 37 || keyStr === "ArrowLeft") {
                code = KEY.LEFT;
            } else if (raw === 38 || keyStr === "ArrowUp") {
                code = KEY.UP;
            } else if (raw === 39 || keyStr === "ArrowRight") {
                code = KEY.RIGHT;
            } else if (raw === 40 || keyStr === "ArrowDown") {
                code = KEY.DOWN;
            } else {
                code = raw;
            }

            if (!code) return;

            // Prevent default browser actions for media & console keys (critically prevent F5 reload, F3, F6)
            if (HANDLED.indexOf(code) !== -1 || HANDLED.indexOf(raw) !== -1 || raw === 114 || raw === 115 || raw === 116 || raw === 117 || raw === 121 || raw === 93 || raw === 33 || raw === 34) {
                e.preventDefault();
                e.stopPropagation();
            }

            remoteLog("KeyDown raw=" + raw + " key=" + keyStr + " -> code=" + code);
            dispatch(code);
        }

        window.addEventListener("keydown", handleKeyDown, true);

        // 3. Gamepad connection notifications
        window.addEventListener("gamepadconnected", function (e) {
            var gp = e.gamepad;
            remoteLog("Gamepad Connected: " + (gp ? gp.id : "DualSense"));
        });
        window.addEventListener("gamepaddisconnected", function () {
            remoteLog("Gamepad Disconnected");
        });

        // 4. Poll HTML5 Gamepad API (DualSense direct access)
        startGamepadPoll();
    }

    function getGamepadsList() {
        if (navigator.getGamepads) {
            return navigator.getGamepads();
        }
        if (navigator.webkitGetGamepads) {
            return navigator.webkitGetGamepads();
        }
        return [];
    }

    function startGamepadPoll() {
        function poll() {
            var activeEl = document.activeElement;
            var isInputActive = !!(activeEl && (activeEl.tagName === "INPUT" || activeEl.tagName === "TEXTAREA" || activeEl.id === "search-input"));
            if (isInputActive) {
                // DualSense is currently operating the PS5 system On-Screen Keyboard.
                // Do not fire background gamepad navigation events while typing in OSK!
                requestAnimationFrame(poll);
                return;
            }

            var gamepads = getGamepadsList();
            var now = Date.now();

            for (var i = 0; i < gamepads.length; i++) {
                var gp = gamepads[i];
                if (!gp) continue;

                // Action Buttons (Edge-triggered)
                checkButtonOnce(gp, 0, KEY.CROSS, "Cross");       // DualSense Cross (X)
                checkButtonOnce(gp, 1, KEY.CIRCLE, "Circle");     // DualSense Circle (O)
                checkButtonOnce(gp, 2, KEY.SQUARE, "Square");     // DualSense Square
                checkButtonOnce(gp, 3, KEY.TRIANGLE, "Triangle"); // DualSense Triangle
                
                // Shoulder Bumpers (Tab Switching)
                checkButtonOnce(gp, 4, KEY.L1, "L1");             // L1 (Previous Tab)
                checkButtonOnce(gp, 5, KEY.R1, "R1");             // R1 (Next Tab)
                checkButtonOnce(gp, 6, KEY.L1, "L2_Fallback");    // L2 as fallback L1
                checkButtonOnce(gp, 7, KEY.R1, "R2_Fallback");    // R2 as fallback R1

                // Menu & Options
                checkButtonOnce(gp, 8, KEY.OPTIONS, "Share_Options"); // Share / Create button as Options fallback
                checkButtonOnce(gp, 9, KEY.OPTIONS, "Options");       // Physical DualSense Options Button
                checkButtonOnce(gp, 17, KEY.OPTIONS, "Touchpad_Click"); // Touchpad Click as Options fallback

                // D-Pad Directional Buttons (with Repeat)
                checkButtonRepeat(gp, 12, KEY.UP, now);      // D-Pad Up
                checkButtonRepeat(gp, 13, KEY.DOWN, now);    // D-Pad Down
                checkButtonRepeat(gp, 14, KEY.LEFT, now);    // D-Pad Left
                checkButtonRepeat(gp, 15, KEY.RIGHT, now);   // D-Pad Right

                // Left Analog Stick Deadzone Navigation (with Repeat)
                if (gp.axes && gp.axes.length >= 2) {
                    var x = gp.axes[0];
                    var y = gp.axes[1];
                    var thresh = 0.50;

                    var padIdx = (typeof gp.index !== "undefined") ? gp.index : i;
                    checkAxisRepeat(padIdx + "_stick_x_neg", x < -thresh, KEY.LEFT, now);
                    checkAxisRepeat(padIdx + "_stick_x_pos", x > thresh, KEY.RIGHT, now);
                    checkAxisRepeat(padIdx + "_stick_y_neg", y < -thresh, KEY.UP, now);
                    checkAxisRepeat(padIdx + "_stick_y_pos", y > thresh, KEY.DOWN, now);
                }
            }
            requestAnimationFrame(poll);
        }
        requestAnimationFrame(poll);
    }

    var prevBtnStates = {};
    function checkButtonOnce(gp, btnIdx, keyCode, name) {
        if (!gp.buttons || !gp.buttons[btnIdx]) return;
        var pressed = isButtonPressed(gp.buttons[btnIdx]);
        var padIdx = (typeof gp.index !== "undefined") ? gp.index : 0;
        var key = padIdx + "_" + btnIdx;

        if (pressed && !prevBtnStates[key]) {
            remoteLog("Gamepad Button Press: " + name + " (idx=" + btnIdx + ") -> code=" + keyCode);
            dispatch(keyCode);
        }
        prevBtnStates[key] = pressed;
    }

    function checkButtonRepeat(gp, btnIdx, keyCode, now) {
        if (!gp.buttons || !gp.buttons[btnIdx]) return;
        var pressed = isButtonPressed(gp.buttons[btnIdx]);
        var padIdx = (typeof gp.index !== "undefined") ? gp.index : 0;
        var key = padIdx + "_dpad_" + btnIdx;
        processRepeat(key, pressed, keyCode, now);
    }

    function checkAxisRepeat(key, pressed, keyCode, now) {
        processRepeat(key, pressed, keyCode, now);
    }

    function processRepeat(key, pressed, keyCode, now) {
        if (pressed) {
            if (!buttonTimestamps[key]) {
                // First press
                buttonTimestamps[key] = { start: now, last: now };
                dispatch(keyCode);
            } else {
                var elapsed = now - buttonTimestamps[key].start;
                var sinceLast = now - buttonTimestamps[key].last;
                if (elapsed > INITIAL_REPEAT_MS && sinceLast > REPEAT_INTERVAL_MS) {
                    buttonTimestamps[key].last = now;
                    dispatch(keyCode);
                }
            }
        } else {
            delete buttonTimestamps[key];
        }
    }

    function onKey(fn) {
        listeners.push(fn);
    }

    function dispatch(code) {
        for (var i = 0; i < listeners.length; i++) {
            try {
                listeners[i](code, KEY);
            } catch (err) {
                console.error("Input dispatch error:", err);
            }
        }
    }

    return {
        init: init,
        onKey: onKey,
        KEY: KEY
    };
})();
