/*
 * ESP32 Wi-Fi Setup/Web Server Template - Application JavaScript
 * Vanilla JS, no frameworks. Compact for ESP32 flash.
 */


/* ======== Constants & State ======== */
var DEFAULT_PASSWORD = 'admin123';
var dashboardInterval = null;
var statusInterval = null;

function cleanupCurrentView() {
    if (dashboardInterval) { clearInterval(dashboardInterval); dashboardInterval = null; }
    if (statusInterval) { clearInterval(statusInterval); statusInterval = null; }
    if (typeof pumpPollTimer !== 'undefined' && pumpPollTimer) { clearInterval(pumpPollTimer); pumpPollTimer = null; }
    if (typeof pumpTickTimer !== 'undefined' && pumpTickTimer) { clearInterval(pumpTickTimer); pumpTickTimer = null; }
    if (typeof coolingPollTimer !== 'undefined' && coolingPollTimer) { clearInterval(coolingPollTimer); coolingPollTimer = null; }
    if (typeof handlePumpVisibilityChange !== 'undefined') document.removeEventListener('visibilitychange', handlePumpVisibilityChange);
    if (typeof handleCoolingVisibilityChange !== 'undefined') document.removeEventListener('visibilitychange', handleCoolingVisibilityChange);
}

/* ======== Session / Auth ======== */

function getSessionToken() {
    /* 
     * Security note: logged_in=1 is a non-secret routing hint only.
     * The actual auth validation is always server-side via the HttpOnly session cookie.
     * If this marker is stale (session expired), the next authenticated API call will
     * return 401 -> handleUnauthorized() -> redirect to /login.
     */
    var c = document.cookie.match(/logged_in=1/);
    return c ? "1" : null;
}

function isAuthenticated() {
    return !!getSessionToken();
}

function checkAuth() {
    /* Skip auth check on login page itself */
    if (window.location.pathname === '/login') return;
    if (!isAuthenticated()) {
        navigateTo('/login');
    }
}

/* ======== API Helpers ======== */

var API_TIMEOUT_MS = 8000;

var s_handlingUnauthorized = false;

function isUnauthorizedError(err) {
    return err && err.message === 'HTTP 401';
}

function handleUnauthorized() {
    if (s_handlingUnauthorized) return;
    s_handlingUnauthorized = true;
    cleanupCurrentView();
    pumpStatusRequestInFlight = false;
    statusSummaryRequestInFlight = false;
    statusFullRequestInFlight = false;
    coolingStatusRequestInFlight = false;
    pumpLoadingConfig = false;
    coolingLoadingConfig = false;
    pumpSavingConfig = false;
    coolingSavingConfig = false;
    pumpPending = false;
    waterPending = false;
    washPending = false;
    coolingPending = false;
    hardwareLoadingMap = false;
    hardwareSavingMap = false;
    hardwarePending = false;
    document.cookie = 'session=; Path=/; Max-Age=0';
    document.cookie = 'logged_in=; Path=/; Max-Age=0';
    var forcePwdModal = document.getElementById('force-pwd-modal');
    if (forcePwdModal && forcePwdModal.open) forcePwdModal.close();
    navigateTo('/login');
    s_handlingUnauthorized = false;
}

function apiGet(url, cb, customTimeout, _retryState) {
    var MAX_RETRIES = 2;       // up to 2 automatic retries (3 total attempts)
    var RETRY_BASE_MS = 1500;  // 1.5s, 3s backoff
    var state = _retryState || { attempt: 0 };

    var xhr = new XMLHttpRequest();
    var done = false;

    function finish(err, data) {
        if (done) return;
        done = true;
        // Retry only on transient errors (network / timeout), not on HTTP 4xx/5xx
        if (err && state.attempt < MAX_RETRIES &&
            (err.message === 'Network error' || err.message === 'Request timeout')) {
            state.attempt++;
            var delay = RETRY_BASE_MS * state.attempt;
            setTimeout(function () {
                apiGet(url, cb, customTimeout, state);
            }, delay);
            return;
        }
        cb(err, data);
    }

    xhr.open('GET', url, true);
    xhr.timeout = customTimeout || API_TIMEOUT_MS;
    xhr.onload = function () {
        if (xhr.status === 403) {
            try {
                var data = JSON.parse(xhr.responseText);
                if (data.error === 'force_password_change') { showForcePasswordModal(); }
            } catch (e) { }
            finish(new Error('HTTP 403 Forbidden'), null);
            return;
        }
        if (xhr.status === 200) {
            try {
                var data = JSON.parse(xhr.responseText);
                if (url === '/api/status' && data && data.ok) {
                    var banner = document.getElementById('staging-banner');
                    if (banner) {
                        if (data.stg_type > 0) { banner.classList.remove('hidden'); }
                        else { banner.classList.add('hidden'); }
                    }
                }
                finish(null, data);
            } catch (e) { finish(e, null); }
        } else if (xhr.status === 401 && window.location.pathname !== '/login') {
            handleUnauthorized();
        } else {
            finish(new Error('HTTP ' + xhr.status), null);
        }
    };
    xhr.onerror = function () { finish(new Error('Network error'), null); };
    xhr.ontimeout = function () { finish(new Error('Request timeout'), null); };
    xhr.onabort = function () { finish(new Error('Request aborted'), null); };
    xhr.send();
}

function apiPost(url, body, cb) {
    var xhr = new XMLHttpRequest();
    var done = false;
    function finish(err, data) {
        if (done) return;
        done = true;
        cb(err, data);
    }
    xhr.open('POST', url, true);
    xhr.timeout = API_TIMEOUT_MS;
    xhr.setRequestHeader('Content-Type', 'application/json');
    xhr.onload = function () {
        if (xhr.status === 403) {
            try {
                var data = JSON.parse(xhr.responseText);
                if (data.error === 'force_password_change') {
                    showForcePasswordModal();
                }
            } catch (e) { }
            finish(new Error('HTTP 403 Forbidden'), null);
            return;
        }
        if (xhr.status === 401 && window.location.pathname !== '/login') {
            handleUnauthorized();
            return;
        }
        try {
            var parsed = JSON.parse(xhr.responseText);
            finish(null, parsed);
        } catch (e) {
            if (xhr.status === 200) {
                finish(e, null);
            } else {
                finish(new Error('HTTP ' + xhr.status), null);
            }
        }
    };
    xhr.onerror = function () { finish(new Error('Network error'), null); };
    xhr.ontimeout = function () { finish(new Error('Request timeout'), null); };
    xhr.onabort = function () { finish(new Error('Request aborted'), null); };
    xhr.send(JSON.stringify(body));
}

function apiDelete(url, cb) {
    var xhr = new XMLHttpRequest();
    var done = false;
    function finish(err, data) {
        if (done) return;
        done = true;
        cb(err, data);
    }
    xhr.open('DELETE', url, true);
    xhr.timeout = API_TIMEOUT_MS;
    xhr.onload = function () {
        if (xhr.status === 200) {
            try { finish(null, JSON.parse(xhr.responseText)); }
            catch (e) { finish(e, null); }
        } else if (xhr.status === 401 && window.location.pathname !== '/login') {
            handleUnauthorized();
        } else {
            finish(new Error('HTTP ' + xhr.status), null);
        }
    };
    xhr.onerror = function () { finish(new Error('Network error'), null); };
    xhr.ontimeout = function () { finish(new Error('Request timeout'), null); };
    xhr.onabort = function () { finish(new Error('Request aborted'), null); };
    xhr.send();
}

/* ======== Toast Notifications ======== */

function showToast(msg, type) {
    type = type || 'info';
    var container = document.getElementById('toast');
    if (!container) {
        container = document.body;
    }
    var el = document.createElement('div');
    el.className = 'toast-item ' + type;

    var iconHtml = '';
    if (type === 'success') {
        iconHtml = '<span class="toast-icon-wrap success"><svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="3" fill="none"><polyline points="20 6 9 17 4 12"></polyline></svg></span>';
    } else if (type === 'error') {
        iconHtml = '<span class="toast-icon-wrap error"><svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="3" fill="none"><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg></span>';
    } else {
        iconHtml = '<span class="toast-icon-wrap info"><svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="3" fill="none"><circle cx="12" cy="12" r="10"></circle><line x1="12" y1="16" x2="12" y2="12"></line><line x1="12" y1="8" x2="12.01" y2="8"></line></svg></span>';
    }

    el.innerHTML = '<div class="toast-content">' + iconHtml + '<span class="toast-msg">' + escHtml(msg) + '</span></div>';

    if (type === 'error') {
        el.setAttribute('role', 'alert');
    }
    container.appendChild(el);
    setTimeout(function () {
        el.style.opacity = '0';
        el.style.transition = 'opacity 0.3s';
        setTimeout(function () { if (el.parentNode) el.parentNode.removeChild(el); }, 300);
    }, 3000);
}

/* ======== Copy to Clipboard ======== */

function copyToClipboard(text, labelName) {
    labelName = labelName || 'ข้อมูล';
    if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(text).then(function () {
            showToast('คัดลอก ' + labelName + ' เรียบร้อยแล้ว', 'success');
        }).catch(function () {
            fallbackCopyTextToClipboard(text, labelName);
        });
    } else {
        fallbackCopyTextToClipboard(text, labelName);
    }
}

function fallbackCopyTextToClipboard(text, labelName) {
    var textArea = document.createElement("textarea");
    textArea.value = text;
    textArea.style.top = "0";
    textArea.style.left = "0";
    textArea.style.position = "fixed";
    document.body.appendChild(textArea);
    textArea.focus();
    textArea.select();

    try {
        var successful = document.execCommand('copy');
        if (successful) {
            showToast('คัดลอก ' + labelName + ' เรียบร้อยแล้ว', 'success');
        } else {
            showToast('คัดลอก ' + labelName + ' ไม่สำเร็จ', 'error');
        }
    } catch (err) {
        showToast('คัดลอก ' + labelName + ' ไม่สำเร็จ', 'error');
    }

    document.body.removeChild(textArea);
}

/* ======== Focus Trap & Dialog Keyboard Helpers ======== */
var activeFocusTrap = null;
var previousFocusedElement = null;

function trapFocus(element) {
    if (!element) return;

    previousFocusedElement = document.activeElement;
    activeFocusTrap = element;

    element.setAttribute('aria-hidden', 'false');

    var focusables = element.querySelectorAll('button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])');
    if (focusables.length > 0) {
        var first = Array.prototype.find.call(focusables, function (el) {
            return !el.disabled && el.offsetWidth > 0 && el.offsetHeight > 0;
        });
        if (first) {
            first.focus();
        }
    }
}

function untrapFocus(element) {
    if (element) {
        element.setAttribute('aria-hidden', 'true');
    }
    activeFocusTrap = null;
    if (previousFocusedElement) {
        try {
            previousFocusedElement.focus();
        } catch (e) { }
        previousFocusedElement = null;
    }
}
document.addEventListener('keydown', function (e) {
    if (e.key === 'Tab' && activeFocusTrap) {
        var focusables = activeFocusTrap.querySelectorAll('button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])');
        var visibleFocusables = Array.prototype.filter.call(focusables, function (el) {
            return !el.disabled && el.offsetWidth > 0 && el.offsetHeight > 0;
        });

        if (visibleFocusables.length === 0) {
            e.preventDefault();
            return;
        }

        var first = visibleFocusables[0];
        var last = visibleFocusables[visibleFocusables.length - 1];

        if (e.shiftKey) {
            if (document.activeElement === first) {
                last.focus();
                e.preventDefault();
            }
        } else {
            if (document.activeElement === last) {
                first.focus();
                e.preventDefault();
            }
        }
        return;
    }

    // ข้ามการทำคีย์ลัดหากกำลังอยู่ในช่องป้อนข้อมูล (Input/Textarea/Select)
    var activeTag = document.activeElement ? document.activeElement.tagName.toLowerCase() : '';
    if (activeTag === 'input' || activeTag === 'textarea' || activeTag === 'select') {
        return;
    }

    // คีย์ Spacebar: สลับสั่ง Start/Stop ของปั๊มน้ำเฉพาะที่หน้า Dashboard
    if (e.key === ' ' || e.code === 'Space') {
        if (window.location.pathname === '/dashboard') {
            e.preventDefault();
            var isRunning = pumpLastStatus && pumpLastStatus.running;
            if (isRunning) {
                stopPump();
            } else {
                startPump();
            }
        }
    }

    // ปุ่ม S หรือ s: บังคับซิงค์ข้อมูล (Manual Sync) ของหน้าแดชบอร์ดที่เปิดอยู่
    if (e.key === 's' || e.key === 'S') {
        if (window.location.pathname === '/dashboard') {
            e.preventDefault();
            syncPumpStatus(true);
        } else if (window.location.pathname === '/cooling') {
            e.preventDefault();
            syncCoolingStatus(true);
        }
    }
});

/* ======== UI Helpers ======== */


function setLoading(btn, isLoading) {
    if (typeof btn === 'string') btn = document.getElementById(btn);
    if (!btn) return;
    btn.disabled = !!isLoading;
    if (isLoading) {
        btn.classList.add('loading');
    } else {
        btn.classList.remove('loading');
    }
}

function showConfirmModal(title, message, onConfirm, isDangerous) {
    var container = document.getElementById('modal-container');
    var titleEl = document.getElementById('modal-title');
    var msgEl = document.getElementById('modal-message');
    var cancelBtn = document.getElementById('modal-cancel');
    var confirmBtn = document.getElementById('modal-confirm');

    if (!container || !titleEl || !msgEl || !cancelBtn || !confirmBtn) return;

    titleEl.textContent = title;
    msgEl.textContent = message;

    if (isDangerous) {
        confirmBtn.className = 'btn btn-danger';
    } else {
        confirmBtn.className = 'btn btn-primary';
    }

    function cleanup() {
        container.close();
        cancelBtn.onclick = null;
        confirmBtn.onclick = null;
    }

    cancelBtn.onclick = function () { cleanup(); };
    confirmBtn.onclick = function () { cleanup(); if (onConfirm) onConfirm(); };

    container.showModal();
}

/* ======== Login Page ======== */

function initLogin() {
    if (window.location.pathname !== '/login') return;

    var form = document.getElementById('login-form');
    var btn = document.getElementById('login-btn');
    var errEl = document.getElementById('login-error');

    form.onsubmit = function (e) {
        e.preventDefault();
        var uInput = document.getElementById('username');
        var pInput = document.getElementById('password');
        var rememberEl = document.getElementById('remember-me');
        var username = uInput ? uInput.value.trim() : '';
        var password = pInput ? pInput.value : '';
        var remember = rememberEl ? rememberEl.checked : false;

        if (!username || !password) {
            errEl.style.display = 'block';
            errEl.textContent = '\u0e01\u0e23\u0e38\u0e13\u0e32\u0e01\u0e23\u0e2d\u0e01\u0e0a\u0e37\u0e48\u0e2d\u0e1c\u0e39\u0e49\u0e43\u0e0a\u0e49\u0e41\u0e25\u0e30\u0e23\u0e2b\u0e31\u0e2a\u0e1c\u0e48\u0e32\u0e19';
            return;
        }

        setLoading(btn, true);
        if (uInput) uInput.disabled = true;
        if (pInput) pInput.disabled = true;
        if (rememberEl) rememberEl.disabled = true;
        errEl.style.display = 'none';

        apiPost('/api/login', { username: username, password: password, remember: remember }, function (err, data) {
            setLoading(btn, false);
            if (uInput) uInput.disabled = false;
            if (pInput) pInput.disabled = false;
            if (rememberEl) rememberEl.disabled = false;

            if (err || !data || !data.ok) {
                errEl.style.display = 'block';
                if (data && data.error === 'rate_limited' && data.retry_after) {
                    errEl.textContent = '\u0e1e\u0e22\u0e32\u0e22\u0e32\u0e21\u0e25\u0e47\u0e2d\u0e01\u0e2d\u0e34\u0e19\u0e21\u0e32\u0e01\u0e40\u0e01\u0e34\u0e19\u0e44\u0e1b \u0e01\u0e23\u0e38\u0e13\u0e32\u0e23\u0e2d ' + data.retry_after + ' \u0e27\u0e34\u0e19\u0e32\u0e17\u0e35\u0e41\u0e25\u0e49\u0e27\u0e25\u0e2d\u0e07\u0e43\u0e2b\u0e21\u0e48';
                } else if (data && data.error === 'invalid_credentials') {
                    errEl.textContent = '\u0e0a\u0e37\u0e48\u0e2d\u0e1c\u0e39\u0e49\u0e43\u0e0a\u0e49\u0e2b\u0e23\u0e37\u0e2d\u0e23\u0e2b\u0e31\u0e2a\u0e1c\u0e48\u0e32\u0e19\u0e44\u0e21\u0e48\u0e16\u0e39\u0e01\u0e15\u0e49\u0e2d\u0e07';
                } else {
                    errEl.textContent = '\u0e40\u0e01\u0e34\u0e14\u0e02\u0e49\u0e2d\u0e1c\u0e34\u0e14\u0e1e\u0e25\u0e32\u0e14 \u0e01\u0e23\u0e38\u0e13\u0e32\u0e25\u0e2d\u0e07\u0e43\u0e2b\u0e21\u0e48';
                }
                return;
            }

            if (data && data.require_password_change) {
                navigateTo('/dashboard');
                showForcePasswordModal();
            } else {
                navigateTo('/dashboard');
            }
        });
    };
}

/* ======== Logout ======== */

function doLogout() {
    showConfirmModal('ออกจากระบบ', 'คุณต้องการออกจากระบบใช่หรือไม่?', function () {
        apiPost('/api/logout', {}, function () {
            document.cookie = 'session=; Path=/; Max-Age=0';
            document.cookie = 'logged_in=; Path=/; Max-Age=0';
            navigateTo('/login');
        });
    }, true);
}

/* ======== Dashboard Page ======== */

var pumpConfig = null;
var pumpRelayPolarity = null;
var pumpConfigLoaded = false;
var pumpLoadingConfig = false;
var pumpSavingConfig = false;
var pumpEditVersion = 0;
var pumpDirty = false;
var waterPending = false;
var washPending = false;
var pumpPending = false;
var pumpLastStatus = null;
var pumpLastSyncMs = 0;
var pumpCountdownDeadlineMs = null;
var pumpCountdownStatusKey = '';
var pumpDisplayedCountdownSec = null;
var waterCountdownDeadlineMs = null;
var waterCountdownStatusKey = '';
var waterDisplayedCountdownSec = null;
var washCountdownDeadlineMs = null;
var washCountdownStatusKey = '';
var washDisplayedCountdownSec = null;
var pumpStatusRequestInFlight = false;
var pumpAwaitingRolloverSync = false;
var pumpPollTimer = null;
var pumpTickTimer = null;
var PUMP_STATUS_POLL_MS = 5000;
var PUMP_COUNTDOWN_TICK_MS = 250;
var PUMP_STALE_MS = 12000;
var PUMP_DEADLINE_DRIFT_MS = 1500;
var coolingConfig = null;
var coolingConfigLoaded = false;
var coolingLoadingConfig = false;
var coolingSavingConfig = false;
var coolingDirty = false;
var coolingPending = false;
var coolingLastStatus = null;
var coolingLastSyncMs = 0;
var coolingStatusRequestInFlight = false;
var coolingPollTimer = null;
var COOLING_STATUS_POLL_MS = 5000; /* A8 audit: align with pump poll interval */
var COOLING_STALE_MS = 12000;
var statusSummaryRequestInFlight = false;
var statusFullRequestInFlight = false;
var hardwareMapData = null;
var hardwareLoadingMap = false;
var hardwareSavingMap = false;
var hardwareDirty = false;
var hardwarePending = false;
var HARDWARE_FIELDS = [
    { key: 'float_input_gpio', select: 'hardware-float-input-gpio', wire: 'wire-float-input', label: 'ลูกลอย (Float Input)' },
    { key: 'pump_relay1_gpio', select: 'hardware-pump-relay1-gpio', wire: 'wire-pump-relay1', label: 'รีเลย์ปั๊ม 1 (Pump Relay 1)' },
    { key: 'pump_relay2_gpio', select: 'hardware-pump-relay2-gpio', wire: 'wire-pump-relay2', label: 'รีเลย์ปั๊ม 2 (Pump Relay 2)' },
    { key: 'ds18b20_gpio', select: 'hardware-ds18b20-gpio', wire: 'wire-ds18b20', label: 'เซนเซอร์วัดอุณหภูมิ (DS18B20)' },
    { key: 'cooling_relay_gpio', select: 'hardware-cooling-relay-gpio', wire: 'wire-cooling-relay', label: 'รีเลย์ระบบความเย็น (Cooling Relay)' }
];

function initDashboard() {
    initPumpDashboard();

    var pwdForm = document.getElementById('password-form');
    if (pwdForm) {
        pwdForm.onsubmit = doChangePassword;
    }
}

function initPumpDashboard() {
    if (window.location.pathname !== '/dashboard') return;
    wirePumpForm();
    wirePumpActions();
    initSettingsTabs();
    updatePumpConfigSaveButton(false);
    updatePumpButtons();
    loadPumpConfig();
    syncPumpStatus(true);
    refreshStatus();
    dashboardInterval = setInterval(refreshStatus, 10000);
    startPumpLiveTimers();
    document.addEventListener('visibilitychange', handlePumpVisibilityChange);
}

function pumpEl(id) {
    return document.getElementById(id);
}

function updateBadgeCardState(cardId, isActive) {
    var card = pumpEl(cardId);
    if (!card) return;
    if (isActive) {
        card.classList.add('state-active');
        card.classList.remove('state-inactive');
    } else {
        card.classList.add('state-inactive');
        card.classList.remove('state-active');
    }
}

var SYSTEM_PHASE_CLASSES = ['phase-active', 'phase-paused', 'phase-stopped'];

function applySystemPhaseVisuals(countdownEl, barFillEl, sysState) {
    var cls = 'phase-stopped';
    if (sysState && sysState.running) {
        cls = sysState.phase === 'ON' ? 'phase-active' : 'phase-paused';
    }
    [countdownEl, barFillEl].forEach(function(el) {
        if (!el) return;
        if (!el.classList.contains(cls)) {
            el.classList.remove.apply(el.classList, SYSTEM_PHASE_CLASSES);
            el.classList.add(cls);
        }
    });
}

function initCoolingDashboard() {
    if (window.location.pathname !== '/cooling') return;
    wireCoolingForm();
    wireCoolingActions();
    initSettingsTabs();
    updateCoolingButtons();
    loadCoolingConfig();
    syncCoolingStatus(true);
    startCoolingStatusTimer();
    document.addEventListener('visibilitychange', handleCoolingVisibilityChange);
}

function setPumpAlert(id, message) {
    var el = pumpEl(id);
    if (!el) return;
    if (message) {
        el.textContent = message;
        el.classList.remove('hidden');
    } else {
        el.textContent = '';
        el.classList.add('hidden');
    }
}

function setCoolingAlert(message) {
    setPumpAlert('cooling-config-error', message);
}

function wirePumpForm() {
    var form = pumpEl('pump-config-form');
    if (form) {
        form.onsubmit = function (e) {
            e.preventDefault();
            savePumpConfig();
        };
    }

    var ids = [
        'timer1-on-min', 'timer1-on-sec', 'timer1-off-min', 'timer1-off-sec',
        'timer2-on-min', 'timer2-on-sec', 'timer2-off-min', 'timer2-off-sec',
        'wash-on-min', 'wash-on-sec', 'wash-off-min', 'wash-off-sec',
        'timer1-start-phase', 'timer2-start-phase', 'wash-start-phase',
        'pump-auto-start', 'wash-auto-start', 'pump-min-dwell'
    ];
    for (var i = 0; i < ids.length; i++) {
        var input = pumpEl(ids[i]);
        if (!input) continue;
        input.oninput = markPumpDirty;
        input.onchange = markPumpDirty;
    }
}

function wirePumpActions() {
    var waterStartBtn = pumpEl('water-start-btn');
    var waterStopBtn = pumpEl('water-stop-btn');
    var washStartBtn = pumpEl('wash-start-btn');
    var washStopBtn = pumpEl('wash-stop-btn');
    var legacyStartBtn = pumpEl('pump-start-btn');
    var legacyStopBtn = pumpEl('pump-stop-btn');

    if (waterStartBtn) waterStartBtn.onclick = function () { startSubsystem('water'); };
    if (waterStopBtn) waterStopBtn.onclick = function () { stopSubsystem('water'); };
    if (washStartBtn) washStartBtn.onclick = function () { startSubsystem('wash'); };
    if (washStopBtn) washStopBtn.onclick = function () { stopSubsystem('wash'); };
    if (legacyStartBtn) legacyStartBtn.onclick = function () { startSubsystem('water'); };
    if (legacyStopBtn) legacyStopBtn.onclick = function () { stopSubsystem('water'); };
}

function switchDashboardTab(tabId) {
    tabId = tabId || 'water';
    var tabBtns = document.querySelectorAll('.mobile-tab-btn');
    for (var i = 0; i < tabBtns.length; i++) {
        var btn = tabBtns[i];
        if (btn.getAttribute('data-tab') === tabId) {
            btn.classList.add('active');
        } else {
            btn.classList.remove('active');
        }
    }
    var panels = document.querySelectorAll('.dashboard-tab-panel');
    for (var j = 0; j < panels.length; j++) {
        var panel = panels[j];
        if (panel.getAttribute('data-tab-content') === tabId) {
            panel.classList.add('active');
        } else {
            panel.classList.remove('active');
        }
    }
}

function switchPumpTab(tabId) {
    switchDashboardTab(tabId);
}

function switchSettingsTabInBlock(block, tabId) {
    if (!block || !tabId) return;
    var btns = block.querySelectorAll('.settings-segment-btn');
    for (var i = 0; i < btns.length; i++) {
        var active = btns[i].getAttribute('data-settings-tab') === tabId;
        btns[i].classList.toggle('active', active);
        btns[i].setAttribute('aria-selected', active ? 'true' : 'false');
        btns[i].setAttribute('tabindex', active ? '0' : '-1');
    }
    var panels = block.querySelectorAll('.settings-tab-panel');
    for (var j = 0; j < panels.length; j++) {
        var show = panels[j].getAttribute('data-settings-panel') === tabId;
        panels[j].classList.toggle('active', show);
        panels[j].hidden = !show;
        panels[j].setAttribute('aria-hidden', show ? 'false' : 'true');
    }
}

function initSettingsTabs() {
    var bars = document.querySelectorAll('.settings-segment-bar');
    for (var i = 0; i < bars.length; i++) {
        var bar = bars[i];
        var block = bar.closest('.settings-system-block');
        if (!block) continue;

        var selectedBtn = bar.querySelector('.settings-segment-btn[aria-selected="true"]') ||
            bar.querySelector('.settings-segment-btn');
        if (selectedBtn) {
            switchSettingsTabInBlock(block, selectedBtn.getAttribute('data-settings-tab'));
        }

        if (bar.getAttribute('data-settings-tabs-bound') === 'true') continue;

        (function (settingsBar, settingsBlock) {
            settingsBar.addEventListener('click', function (event) {
                var button = event.target.closest('.settings-segment-btn');
                if (!button || !settingsBar.contains(button)) return;
                switchSettingsTabInBlock(settingsBlock, button.getAttribute('data-settings-tab'));
            });

            settingsBar.addEventListener('keydown', function (event) {
                var buttons = Array.prototype.slice.call(
                    settingsBar.querySelectorAll('.settings-segment-btn')
                );
                var current = document.activeElement;
                var currentIndex = buttons.indexOf(current);
                if (currentIndex < 0 || buttons.length < 2) return;

                var nextIndex = currentIndex;
                if (event.key === 'ArrowRight' || event.key === 'ArrowDown') {
                    nextIndex = (currentIndex + 1) % buttons.length;
                } else if (event.key === 'ArrowLeft' || event.key === 'ArrowUp') {
                    nextIndex = (currentIndex - 1 + buttons.length) % buttons.length;
                } else if (event.key === 'Home') {
                    nextIndex = 0;
                } else if (event.key === 'End') {
                    nextIndex = buttons.length - 1;
                } else {
                    return;
                }

                event.preventDefault();
                var nextButton = buttons[nextIndex];
                switchSettingsTabInBlock(settingsBlock, nextButton.getAttribute('data-settings-tab'));
                nextButton.focus();
            });
        })(bar, block);

        bar.setAttribute('data-settings-tabs-bound', 'true');
    }
}

function updateCoolingSettingsPreview() {
    var threshInput = pumpEl('cooling-threshold-input');
    var hystInput = pumpEl('cooling-hysteresis-input');
    var preview = pumpEl('cooling-threshold-preview');
    if (!preview) return;
    var threshRaw = threshInput ? threshInput.value.trim() : '';
    var hystRaw = hystInput ? hystInput.value.trim() : '';
    var thresh = threshRaw !== '' ? Number(threshRaw) : NaN;
    var hyst = hystRaw !== '' ? Number(hystRaw) : NaN;
    if (!isFinite(thresh) || !isFinite(hyst)) {
        preview.textContent = 'เมื่ออุณหภูมิ ≥ -- °C เปิดพัด · ปิดที่ ≤ -- °C';
        return;
    }
    var offTemp = Math.round((thresh - hyst) * 10) / 10;
    preview.textContent = 'เมื่ออุณหภูมิ ≥ ' + thresh.toFixed(1) + ' °C เปิดพัด · ปิดที่ ≤ ' + offTemp.toFixed(1) + ' °C';
}

function updateCoolingLockoutHint() {
    var minOffInput = pumpEl('cooling-min-off');
    var hint = pumpEl('cooling-lockout-hint');
    if (!hint || !minOffInput) return;
    var sec = parseInt(minOffInput.value, 10);
    if (!isFinite(sec) || sec <= 0) {
        hint.textContent = '';
        return;
    }
    if (sec >= 60 && sec % 60 === 0) {
        hint.textContent = '= ' + (sec / 60) + ' นาที';
    } else if (sec >= 60) {
        hint.textContent = '= ' + Math.floor(sec / 60) + ' นาที ' + (sec % 60) + ' วินาที';
    } else {
        hint.textContent = '';
    }
}

function startSubsystem(system) {
    requestSubsystemControl(system, 'start', 'กำลังสั่ง Start...', 'เริ่มระบบ' + (system === 'wash' ? 'ฉีดล้าง' : 'น้ำ') + 'แล้ว');
}

function stopSubsystem(system) {
    requestSubsystemControl(system, 'stop', 'กำลังสั่ง Stop...', 'หยุดระบบ' + (system === 'wash' ? 'ฉีดล้าง' : 'น้ำ') + 'แล้ว');
}

function startPump() { startSubsystem('water'); }
function stopPump() { stopSubsystem('water'); }

function requestSubsystemControl(system, action, pendingText, successText) {
    if (system === 'wash') {
        if (washPending) return;
        washPending = true;
    } else {
        if (waterPending) return;
        waterPending = true;
    }
    pumpPending = waterPending || washPending;
    var stateEl = pumpEl(system + '-action-state');
    if (stateEl) setText(stateEl.id, pendingText);
    updatePumpButtons();
    apiPost('/api/pump/control', { system: system, action: action }, function (err, data) {
        if (system === 'wash') {
            washPending = false;
        } else {
            waterPending = false;
        }
        pumpPending = waterPending || washPending;
        if (err || !data || !data.ok) {
            if (stateEl) setText(stateEl.id, '');
            setPumpAlert(system + '-status-error', (data && data.message) ? data.message : 'สั่งงานไม่สำเร็จ');
            updatePumpButtons();
            showToast('สั่งงานไม่สำเร็จ', 'error');
            return;
        }
        if (data.status) applyPumpStatus(data.status, true);
        else if (data.system_water || data.system_wash) applyPumpStatus(data, true);
        if (stateEl) setText(stateEl.id, data.already_running ? 'ระบบทำงานอยู่แล้ว' : (data.already_stopped ? 'ระบบหยุดอยู่แล้ว' : ''));
        setPumpAlert(system + '-status-error', '');
        updatePumpButtons();
        showToast(successText, 'success');
        syncPumpStatus(true);
    });
}

function checkPumpDirty() {
    if (!pumpConfig) return false;
    var t1OnMin = parseInt(pumpEl('timer1-on-min').value, 10) || 0;
    var t1OnSec = parseInt(pumpEl('timer1-on-sec').value, 10) || 0;
    var t1OnTotal = (t1OnMin * 60) + t1OnSec;

    var t1OffMin = parseInt(pumpEl('timer1-off-min').value, 10) || 0;
    var t1OffSec = parseInt(pumpEl('timer1-off-sec').value, 10) || 0;
    var t1OffTotal = (t1OffMin * 60) + t1OffSec;

    var t2OnMin = parseInt(pumpEl('timer2-on-min').value, 10) || 0;
    var t2OnSec = parseInt(pumpEl('timer2-on-sec').value, 10) || 0;
    var t2OnTotal = (t2OnMin * 60) + t2OnSec;

    var t2OffMin = parseInt(pumpEl('timer2-off-min').value, 10) || 0;
    var t2OffSec = parseInt(pumpEl('timer2-off-sec').value, 10) || 0;
    var t2OffTotal = (t2OffMin * 60) + t2OffSec;

    var washOnMin = pumpEl('wash-on-min') ? (parseInt(pumpEl('wash-on-min').value, 10) || 0) : 0;
    var washOnSec = pumpEl('wash-on-sec') ? (parseInt(pumpEl('wash-on-sec').value, 10) || 0) : 0;
    var washOnTotal = (washOnMin * 60) + washOnSec;

    var washOffMin = pumpEl('wash-off-min') ? (parseInt(pumpEl('wash-off-min').value, 10) || 0) : 0;
    var washOffSec = pumpEl('wash-off-sec') ? (parseInt(pumpEl('wash-off-sec').value, 10) || 0) : 0;
    var washOffTotal = (washOffMin * 60) + washOffSec;

    var t1Start = pumpEl('timer1-start-phase') ? pumpEl('timer1-start-phase').value : 'on';
    var t2Start = pumpEl('timer2-start-phase') ? pumpEl('timer2-start-phase').value : 'on';
    var washStart = pumpEl('wash-start-phase') ? pumpEl('wash-start-phase').value : 'on';

    var waterAutoStart = pumpEl('pump-auto-start') ? !!pumpEl('pump-auto-start').checked : false;
    var washAutoStart = pumpEl('wash-auto-start') ? !!pumpEl('wash-auto-start').checked : false;

    var minDwellEl = pumpEl('pump-min-dwell');
    var minDwellVal = minDwellEl ? (parseInt(minDwellEl.value, 10) || 0) : 30;

    var cfgW1On = pumpConfig.water_set1_on != null ? pumpConfig.water_set1_on : pumpConfig.timer1_on_sec;
    var cfgW1Off = pumpConfig.water_set1_off != null ? pumpConfig.water_set1_off : pumpConfig.timer1_off_sec;
    var cfgW2On = pumpConfig.water_set2_on != null ? pumpConfig.water_set2_on : pumpConfig.timer2_on_sec;
    var cfgW2Off = pumpConfig.water_set2_off != null ? pumpConfig.water_set2_off : pumpConfig.timer2_off_sec;
    var cfgWashOn = pumpConfig.wash_on != null ? pumpConfig.wash_on : 10;
    var cfgWashOff = pumpConfig.wash_off != null ? pumpConfig.wash_off : 60;

    var cfgW1Start = (pumpConfig.water_set1_start_phase || pumpConfig.timer1_start_phase || 'on').toLowerCase();
    var cfgW2Start = (pumpConfig.water_set2_start_phase || pumpConfig.timer2_start_phase || 'on').toLowerCase();
    var cfgWashStart = (pumpConfig.wash_start_phase || 'on').toLowerCase();

    var cfgWaterAuto = pumpConfig.water_auto_start != null ? !!pumpConfig.water_auto_start : !!pumpConfig.auto_start;
    var cfgWashAuto = !!pumpConfig.wash_auto_start;

    if (t1OnTotal !== Number(cfgW1On)) return true;
    if (t1OffTotal !== Number(cfgW1Off)) return true;
    if (t2OnTotal !== Number(cfgW2On)) return true;
    if (t2OffTotal !== Number(cfgW2Off)) return true;
    if (washOnTotal !== Number(cfgWashOn)) return true;
    if (washOffTotal !== Number(cfgWashOff)) return true;

    if (t1Start.toLowerCase() !== cfgW1Start) return true;
    if (t2Start.toLowerCase() !== cfgW2Start) return true;
    if (washStart.toLowerCase() !== cfgWashStart) return true;

    if (waterAutoStart !== cfgWaterAuto) return true;
    if (washAutoStart !== cfgWashAuto) return true;
    if (minDwellVal !== Number(pumpConfig.min_dwell_sec != null ? pumpConfig.min_dwell_sec : 30)) return true;

    return false;
}

function markPumpDirty() {
    var dirty = checkPumpDirty();
    pumpDirty = dirty;
    var warning = pumpEl('pump-unsaved-warning');
    var saveBar = pumpEl('pump-settings-save-bar');
    if (saveBar) saveBar.classList.toggle('is-dirty', dirty);
    if (warning) {
        if (dirty) {
            warning.classList.remove('hidden');
            setText('pump-config-state', 'มีแก้ไขที่ยังไม่ได้บันทึก');
        } else {
            warning.classList.add('hidden');
            if (pumpConfig && pumpConfig.settings_status) {
                var autoStart = pumpLastStatus ? pumpLastStatus.auto_start : pumpConfig.auto_start;
                setHtml('pump-config-state', renderSettingsStatus(pumpConfig.settings_status, autoStart));
            } else {
                setText('pump-config-state', 'โหลดค่าแล้ว');
            }
        }
    }
    updatePumpConfigSaveButton();
}

function setPumpClean(label) {
    pumpDirty = false;
    var warning = pumpEl('pump-unsaved-warning');
    var saveBar = pumpEl('pump-settings-save-bar');
    if (saveBar) saveBar.classList.remove('is-dirty');
    if (warning) warning.classList.add('hidden');
    if (pumpConfig && (!label || label.indexOf('โหลดค่า') !== -1 || label.indexOf('บันทึก') !== -1)) {
        var statusStr = pumpConfig.settings_status || 'loaded';
        var autoStart = pumpConfig.auto_start !== undefined ? !!pumpConfig.auto_start : (pumpLastStatus ? pumpLastStatus.auto_start : false);
        setHtml('pump-config-state', renderSettingsStatus(statusStr, autoStart));
    } else {
        setText('pump-config-state', label || 'บันทึกแล้ว');
    }
    updatePumpConfigSaveButton();
}

function updatePumpConfigSaveButton() {
    updatePumpButtons();
}

function loadPumpConfig() {
    var requestEditVersion = pumpEditVersion;
    setText('pump-config-state', 'กำลังโหลดค่า...');
    setPumpAlert('pump-config-error', '');
    pumpLoadingConfig = true;
    updatePumpConfigSaveButton();
    apiGet('/api/pump/config', function (err, data) {
        pumpLoadingConfig = false;
        if (isUnauthorizedError(err)) { handleUnauthorized(); return; }
        if (err || !data || !data.ok) {
            pumpConfigLoaded = false;
            setText('pump-config-state', 'โหลดค่าไม่สำเร็จ');
            setPumpAlert('pump-config-error', 'โหลดค่าตั้งเวลาไม่สำเร็จ กรุณาโหลดค่าให้สำเร็จก่อนบันทึก');
            updatePumpConfigSaveButton();
            return;
        }
        pumpConfig = data;
        pumpRelayPolarity = data.relay_polarity || null;
        pumpConfigLoaded = true;
        if (pumpEditVersion !== requestEditVersion) {
            setText('pump-config-state', 'มีแก้ไขที่ยังไม่ได้บันทึก');
            updatePumpConfigSaveButton();
            return;
        }
        setDurationFields('timer1-on', data.water_set1_on != null ? data.water_set1_on : data.timer1_on_sec);
        setDurationFields('timer1-off', data.water_set1_off != null ? data.water_set1_off : data.timer1_off_sec);
        setDurationFields('timer2-on', data.water_set2_on != null ? data.water_set2_on : data.timer2_on_sec);
        setDurationFields('timer2-off', data.water_set2_off != null ? data.water_set2_off : data.timer2_off_sec);
        setDurationFields('wash-on', data.wash_on != null ? data.wash_on : 10);
        setDurationFields('wash-off', data.wash_off != null ? data.wash_off : 60);

        setStartPhaseField('timer1-start-phase', data.water_set1_start_phase || data.timer1_start_phase);
        setStartPhaseField('timer2-start-phase', data.water_set2_start_phase || data.timer2_start_phase);
        setStartPhaseField('wash-start-phase', data.wash_start_phase || 'on');

        var minDwell = pumpEl('pump-min-dwell');
        if (minDwell) minDwell.value = (data.min_dwell_sec != null) ? data.min_dwell_sec : 30;

        var waterAutoStart = pumpEl('pump-auto-start');
        if (waterAutoStart) waterAutoStart.checked = !!(data.water_auto_start != null ? data.water_auto_start : data.auto_start);

        var washAutoStart = pumpEl('wash-auto-start');
        if (washAutoStart) washAutoStart.checked = !!data.wash_auto_start;

        clearPumpValidation();
        setPumpClean('โหลดค่าแล้ว');
    });
}

function setDurationFields(prefix, totalSec) {
    totalSec = Number(totalSec || 0);
    var minEl = pumpEl(prefix + '-min');
    var secEl = pumpEl(prefix + '-sec');
    if (minEl) minEl.value = Math.floor(totalSec / 60);
    if (secEl) secEl.value = totalSec % 60;
}

function setStartPhaseField(id, value) {
    var el = pumpEl(id);
    if (!el) return;
    el.value = (value === 'off' || value === 'OFF') ? 'off' : 'on';
}

function readDuration(prefix, label) {
    var minEl = pumpEl(prefix + '-min');
    var secEl = pumpEl(prefix + '-sec');
    var errEl = pumpEl(prefix + '-error');
    var rawMin = minEl ? minEl.value.trim() : '';
    var rawSec = secEl ? secEl.value.trim() : '';
    var fail = function (message) {
        if (errEl) errEl.textContent = message;
        if (minEl) minEl.classList.add('invalid');
        if (secEl) secEl.classList.add('invalid');
        return { ok: false, value: 0 };
    };

    if (rawMin === '' || rawSec === '') {
        return fail(label + ': กรุณากรอกนาทีและวินาที');
    }
    if (!/^\d+$/.test(rawMin) || !/^\d+$/.test(rawSec)) {
        return fail(label + ': ต้องเป็นจำนวนเต็ม 0 หรือมากกว่า');
    }

    var minutes = parseInt(rawMin, 10);
    var seconds = parseInt(rawSec, 10);
    if (seconds > 59) {
        return fail(label + ': วินาทีต้องอยู่ระหว่าง 0..59');
    }

    var total = (minutes * 60) + seconds;
    if (total < 5 || total > 86400) {
        return fail(label + ': ระยะเวลาต้องอยู่ระหว่าง 5..86400 วินาที');
    }

    if (errEl) errEl.textContent = '';
    if (minEl) minEl.classList.remove('invalid');
    if (secEl) secEl.classList.remove('invalid');
    return { ok: true, value: total };
}

function readStartPhase(id, label) {
    var el = pumpEl(id);
    var errEl = pumpEl(id + '-error');
    var value = el ? el.value : '';
    if (value !== 'on' && value !== 'off') {
        if (errEl) errEl.textContent = label + ': เลือก ON หรือ OFF';
        if (el) el.classList.add('invalid');
        return { ok: false, value: 'on' };
    }
    if (errEl) errEl.textContent = '';
    if (el) el.classList.remove('invalid');
    return { ok: true, value: value };
}

function clearPumpValidation() {
    var ids = ['timer1-on', 'timer1-off', 'timer2-on', 'timer2-off', 'wash-on', 'wash-off'];
    for (var i = 0; i < ids.length; i++) {
        var errEl = pumpEl(ids[i] + '-error');
        var minEl = pumpEl(ids[i] + '-min');
        var secEl = pumpEl(ids[i] + '-sec');
        if (errEl) errEl.textContent = '';
        if (minEl) minEl.classList.remove('invalid');
        if (secEl) secEl.classList.remove('invalid');
    }
    var startIds = ['timer1-start-phase', 'timer2-start-phase', 'wash-start-phase'];
    for (var j = 0; j < startIds.length; j++) {
        var startEl = pumpEl(startIds[j]);
        var startErr = pumpEl(startIds[j] + '-error');
        if (startErr) startErr.textContent = '';
        if (startEl) startEl.classList.remove('invalid');
    }
    var minDwellEl = pumpEl('pump-min-dwell');
    var minDwellErr = pumpEl('pump-min-dwell-error');
    if (minDwellErr) minDwellErr.textContent = '';
    if (minDwellEl) minDwellEl.classList.remove('invalid');
}

function validatePumpConfig() {
    clearPumpValidation();
    var timer1On = readDuration('timer1-on', 'Water Set 1 ช่วงเปิด');
    var timer1Off = readDuration('timer1-off', 'Water Set 1 ช่วงปิด');
    var timer2On = readDuration('timer2-on', 'Water Set 2 ช่วงเปิด');
    var timer2Off = readDuration('timer2-off', 'Water Set 2 ช่วงปิด');
    var washOn = readDuration('wash-on', 'Wash Spray ช่วงเปิด');
    var washOff = readDuration('wash-off', 'Wash Spray ช่วงปิด');

    var timer1Start = readStartPhase('timer1-start-phase', 'Water Set 1 เริ่มรอบ');
    var timer2Start = readStartPhase('timer2-start-phase', 'Water Set 2 เริ่มรอบ');
    var washStart = readStartPhase('wash-start-phase', 'Wash Spray เริ่มรอบ');

    if (!timer1On.ok || !timer1Off.ok || !timer2On.ok || !timer2Off.ok || !washOn.ok || !washOff.ok ||
        !timer1Start.ok || !timer2Start.ok || !washStart.ok) {
        return null;
    }

    var minDwellEl = pumpEl('pump-min-dwell');
    var minDwellErr = pumpEl('pump-min-dwell-error');
    var minDwellRaw = minDwellEl ? minDwellEl.value.trim() : '30';
    var minDwellVal = parseInt(minDwellRaw, 10);
    if (!/^\d+$/.test(minDwellRaw) || minDwellVal < 0 || minDwellVal > 3600) {
        if (minDwellErr) minDwellErr.textContent = 'ค่าระยะหน่วงต้องเป็นตัวเลข 0-3600';
        if (minDwellEl) minDwellEl.classList.add('invalid');
        return null;
    }
    if (minDwellErr) minDwellErr.textContent = '';
    if (minDwellEl) minDwellEl.classList.remove('invalid');

    var waterAuto = !!(pumpEl('pump-auto-start') && pumpEl('pump-auto-start').checked);
    var washAuto = !!(pumpEl('wash-auto-start') && pumpEl('wash-auto-start').checked);

    return {
        water_set1_on: timer1On.value,
        water_set1_off: timer1Off.value,
        water_set1_start_phase: timer1Start.value,
        water_set2_on: timer2On.value,
        water_set2_off: timer2Off.value,
        water_set2_start_phase: timer2Start.value,
        wash_on: washOn.value,
        wash_off: washOff.value,
        wash_start_phase: washStart.value,
        water_auto_start: waterAuto,
        wash_auto_start: washAuto,

        timer1_on_sec: timer1On.value,
        timer1_off_sec: timer1Off.value,
        timer1_start_phase: timer1Start.value,
        timer2_on_sec: timer2On.value,
        timer2_off_sec: timer2Off.value,
        timer2_start_phase: timer2Start.value,
        auto_start: waterAuto,
        relay_polarity: pumpRelayPolarity || 'active_low',
        min_dwell_sec: minDwellVal
    };
}

function savePumpConfig() {
    var saveBtn = pumpEl('pump-save-config');
    if (!pumpConfigLoaded) {
        setText('pump-config-state', 'ยังบันทึกไม่ได้');
        setPumpAlert('pump-config-error', 'กรุณาโหลดค่าจากอุปกรณ์ให้สำเร็จก่อนบันทึก');
        updatePumpConfigSaveButton();
        return;
    }

    var requestEditVersion = pumpEditVersion;
    var payload = validatePumpConfig();
    if (!payload) {
        setPumpAlert('pump-config-error', 'ตรวจสอบค่าตั้งเวลาแล้วลองบันทึกอีกครั้ง');
        return;
    }

    setPumpAlert('pump-config-error', '');
    pumpSavingConfig = true;
    updatePumpConfigSaveButton();
    setText('pump-config-state', 'กำลังบันทึก...');

    apiPost('/api/pump/config', payload, function (err, data) {
        pumpSavingConfig = false;
        if (err || !data || !data.ok) {
            setText('pump-config-state', 'บันทึกไม่สำเร็จ');
            setPumpAlert('pump-config-error', (data && data.message) ? data.message : 'บันทึกค่าตั้งเวลาไม่สำเร็จ');
            updatePumpConfigSaveButton();
            return;
        }
        pumpConfig = data.config || data;
        if (pumpConfig.relay_polarity) pumpRelayPolarity = pumpConfig.relay_polarity;
        pumpConfigLoaded = true;
        if (pumpEditVersion === requestEditVersion) {
            setPumpClean('บันทึกแล้ว');
        } else {
            setText('pump-config-state', 'มีแก้ไขที่ยังไม่ได้บันทึก');
            updatePumpConfigSaveButton();
        }
        showToast('บันทึกค่าระบบปั๊มเรียบร้อย', 'success');
        if (data.status) applyPumpStatus(data.status, true);
        syncPumpStatus(true);
    });
}

function submitPumpConfig() {
    savePumpConfig();
}

function syncPumpStatus(force) {
    if (window.location.pathname !== '/dashboard') return;
    if (document.hidden && !force) return;
    if (pumpStatusRequestInFlight) return;
    pumpStatusRequestInFlight = true;
    apiGet('/api/status', function (err, data) {
        pumpStatusRequestInFlight = false;
        if (isUnauthorizedError(err)) { handleUnauthorized(); return; }
        if (err || !data || !data.ok) {
            handlePumpStatusFailure();
            return;
        }
        applyPumpStatus(data, true);
    });
}

function renderDashboard(status) {
    applyPumpStatus(status, true);
}

function applyPumpStatus(status, authoritative) {
    pumpLastStatus = status;
    if (authoritative) {
        pumpLastSyncMs = Date.now();
        pumpAwaitingRolloverSync = false;
        updatePumpCountdownDeadline(status);
    }

    var sysWater = (status && status.system_water) ? status.system_water : {
        running: !!(status && status.running),
        relay_energized: !!(status && (status.relay1_energized !== undefined ? status.relay1_energized : status.relay_energized)),
        float_state: (status && status.float_state) ? (status.float_state.toUpperCase() === 'ON' ? 'ON' : 'OFF') : 'OFF',
        active_set: (status && status.active_timer === 'timer2') ? 2 : 1,
        phase: (status && status.phase) ? status.phase.toUpperCase() : 'IDLE',
        countdown_sec: (status && status.countdown_sec) ? Number(status.countdown_sec) : 0,
        total_duration_sec: 0,
        auto_start: !!(status && status.auto_start)
    };

    var sysWash = (status && status.system_wash) ? status.system_wash : {
        running: false,
        relay_energized: !!(status && status.relay2_energized),
        phase: 'IDLE',
        countdown_sec: 0,
        total_duration_sec: 0,
        auto_start: false
    };

    if (authoritative) {
        updateWaterCountdownDeadline(sysWater);
        updateWashCountdownDeadline(sysWash);
    }

    var waterActiveSet = Number(sysWater.active_set) === 2 ? 2 : 1;

    // Render Water System Card
    var waterSync = pumpEl('water-sync-state');
    if (waterSync) {
        var waterSyncClass = status.fault ? 'runtime-eyebrow error' : (status.initial_stabilizing ? 'runtime-eyebrow warn' : 'runtime-eyebrow good');
        var waterSyncHtml = status.fault ? renderSvgIcon('icon-x-circle') + ' Fault' : (status.initial_stabilizing ? renderSvgIcon('icon-alert-triangle') + ' Stabilizing...' : renderSvgIcon('icon-check-circle') + ' Synced');
        if (waterSync.className !== waterSyncClass) waterSync.className = waterSyncClass;
        if (waterSync.innerHTML !== waterSyncHtml) waterSync.innerHTML = waterSyncHtml;
    }
    setText('water-active-set', 'Set ' + waterActiveSet);
    var waterFloatOn = sysWater.float_state === 'ON';
    setText('water-float-badge', 'Float: ' + (waterFloatOn ? 'ON' : 'OFF'));
    var waterFloatBadgeEl = pumpEl('water-float-badge');
    if (waterFloatBadgeEl) waterFloatBadgeEl.classList.toggle('badge-on', waterFloatOn);
    var waterRelayOn = !!sysWater.relay_energized;
    setText('water-relay-badge', 'Relay 1: ' + (waterRelayOn ? 'ON' : 'OFF'));
    var waterRelayBadgeEl = pumpEl('water-relay-badge');
    if (waterRelayBadgeEl) waterRelayBadgeEl.classList.toggle('badge-on', waterRelayOn);

    var waterBadge = pumpEl('water-status-badge');
    if (waterBadge) {
        var desiredWaterBadgeClass = !sysWater.running ? 'badge-stopped' :
            (sysWater.phase === 'ON' ? 'badge-active' : 'badge-paused');
        if (!waterBadge.classList.contains(desiredWaterBadgeClass)) {
            waterBadge.classList.remove('badge-active', 'badge-paused', 'badge-offline', 'badge-stopped');
            waterBadge.classList.add(desiredWaterBadgeClass);
        }
    }

    var waterLed = pumpEl('water-led-dot');
    if (waterLed) {
        var desiredWaterLedClass = !sysWater.running ? 'pump-led-dot status-stopped' :
            (sysWater.phase === 'ON' ? 'pump-led-dot status-success' : 'pump-led-dot status-warning');
        if (waterLed.className !== desiredWaterLedClass) waterLed.className = desiredWaterLedClass;
    }

    var waterPhaseTitle = !sysWater.running ? 'หยุดทำงาน' :
        (sysWater.phase === 'ON' ? 'กำลังทำงาน (ON)' : (sysWater.phase === 'OFF' ? 'พักการทำงาน (OFF)' : 'หยุดทำงาน'));
    setText('water-phase-title', waterPhaseTitle);

    var waterDesc = !sysWater.running ? 'ระบบควบคุมน้ำหยุดทำงาน' :
        (sysWater.phase === 'ON' ? 'ระบบกำลังจ่ายน้ำ (Set ' + waterActiveSet + ')' :
            (sysWater.phase === 'OFF' ? 'ระบบอยู่ในช่วงพักการจ่ายน้ำ (Set ' + waterActiveSet + ')' : 'ทำงานตามสถานะสวิตช์ลูกลอย'));
    setText('water-state-desc', waterDesc);
    setText('water-countdown', formatPumpCountdown(sysWater.countdown_sec));

    var waterCountdown = pumpEl('water-countdown');
    var waterBarFill = pumpEl('water-progress-bar-fill');
    applySystemPhaseVisuals(waterCountdown, waterBarFill, sysWater);
    if (waterBarFill) {
        var waterTotal = getWaterTotalDurationSec(sysWater);
        var waterPct = (waterTotal > 0) ? Math.min(100, Math.max(0, (sysWater.countdown_sec / waterTotal) * 100)) : 0;
        waterBarFill.style.transform = 'translateX(' + (waterPct - 100).toFixed(1) + '%)';
    }

    // Render Wash System Card
    var washSync = pumpEl('wash-sync-state');
    if (washSync) {
        var washSyncClass = status.fault ? 'runtime-eyebrow error' : (status.initial_stabilizing ? 'runtime-eyebrow warn' : 'runtime-eyebrow good');
        var washSyncHtml = status.fault ? renderSvgIcon('icon-x-circle') + ' Fault' : (status.initial_stabilizing ? renderSvgIcon('icon-alert-triangle') + ' Stabilizing...' : renderSvgIcon('icon-check-circle') + ' Synced');
        if (washSync.className !== washSyncClass) washSync.className = washSyncClass;
        if (washSync.innerHTML !== washSyncHtml) washSync.innerHTML = washSyncHtml;
    }
    var washRelayOn = !!sysWash.relay_energized;
    setText('wash-relay-badge', 'Relay 2: ' + (washRelayOn ? 'ON' : 'OFF'));
    var washRelayBadgeEl = pumpEl('wash-relay-badge');
    if (washRelayBadgeEl) washRelayBadgeEl.classList.toggle('badge-on', washRelayOn);

    var washBadge = pumpEl('wash-status-badge');
    if (washBadge) {
        var desiredWashBadgeClass = !sysWash.running ? 'badge-stopped' :
            (sysWash.phase === 'ON' ? 'badge-active' : 'badge-paused');
        if (!washBadge.classList.contains(desiredWashBadgeClass)) {
            washBadge.classList.remove('badge-active', 'badge-paused', 'badge-offline', 'badge-stopped');
            washBadge.classList.add(desiredWashBadgeClass);
        }
    }

    var washLed = pumpEl('wash-led-dot');
    if (washLed) {
        var desiredWashLedClass = !sysWash.running ? 'pump-led-dot status-stopped' :
            (sysWash.phase === 'ON' ? 'pump-led-dot status-success' : 'pump-led-dot status-warning');
        if (washLed.className !== desiredWashLedClass) washLed.className = desiredWashLedClass;
    }

    var washPhaseTitle = !sysWash.running ? 'หยุดทำงาน' :
        (sysWash.phase === 'ON' ? 'กำลังทำงาน (ON)' : (sysWash.phase === 'OFF' ? 'พักการทำงาน (OFF)' : 'หยุดทำงาน'));
    setText('wash-phase-title', washPhaseTitle);

    var washDesc = !sysWash.running ? 'ระบบฉีดล้างหยุดทำงาน' :
        (sysWash.phase === 'ON' ? 'ระบบกำลังฉีดล้าง' :
            (sysWash.phase === 'OFF' ? 'ระบบอยู่ในช่วงพักการฉีดล้าง' : 'ทำงานตามรอบเวลาฉีดล้าง'));
    setText('wash-state-desc', washDesc);
    setText('wash-countdown', formatPumpCountdown(sysWash.countdown_sec));

    var washCountdown = pumpEl('wash-countdown');
    var washBarFill = pumpEl('wash-progress-bar-fill');
    applySystemPhaseVisuals(washCountdown, washBarFill, sysWash);
    if (washBarFill) {
        var washTotal = getWashTotalDurationSec(sysWash);
        var washPct = (washTotal > 0) ? Math.min(100, Math.max(0, (sysWash.countdown_sec / washTotal) * 100)) : 0;
        washBarFill.style.transform = 'translateX(' + (washPct - 100).toFixed(1) + '%)';
    }

    renderPumpCountdown();

    var cooldownSource = sysWater.cooldown_remaining_sec !== undefined
        ? sysWater.cooldown_remaining_sec
        : status.cooldown_remaining_sec;
    var cooldown = Number(cooldownSource || 0);
    var cooldownEl = pumpEl('pump-cooldown-badge');
    if (cooldownEl) {
        if (cooldown > 0) {
            cooldownEl.textContent = 'Cooldown ' + cooldown + ' วิ';
            cooldownEl.classList.remove('hidden');
        } else {
            cooldownEl.classList.add('hidden');
            cooldownEl.textContent = '';
        }
    }

    updateBadgeCardState('pump-float-card', status.float_state === 'on');
    updateBadgeCardState('pump-relay-1-card', relayEnergizedFor('relay1', status));
    updateBadgeCardState('pump-relay-2-card', relayEnergizedFor('relay2', status));

    var t1Card = pumpEl('water-set1-card-panel');
    var t2Card = pumpEl('water-set2-card-panel');
    if (t1Card && t2Card) {
        t1Card.classList.toggle('active-timer', waterActiveSet === 1);
        t2Card.classList.toggle('active-timer', waterActiveSet === 2);
        t1Card.setAttribute('data-runtime-active', waterActiveSet === 1 ? 'true' : 'false');
        t2Card.setAttribute('data-runtime-active', waterActiveSet === 2 ? 'true' : 'false');
    }

    var waterBlock = document.querySelector('.settings-system-water');
    if (waterBlock) {
        var waterSetButtons = waterBlock.querySelectorAll('.settings-segment-btn');
        for (var waterButtonIndex = 0; waterButtonIndex < waterSetButtons.length; waterButtonIndex++) {
            var waterButton = waterSetButtons[waterButtonIndex];
            var runtimeActive = waterButton.getAttribute('data-settings-tab') === 'water-set' + waterActiveSet;
            waterButton.classList.toggle('runtime-active', runtimeActive);
            waterButton.setAttribute('data-runtime-active', runtimeActive ? 'true' : 'false');
            var runtimeIndicator = waterButton.querySelector('.settings-runtime-indicator');
            if (runtimeIndicator) runtimeIndicator.hidden = !runtimeActive;
        }
    }

    if (!pumpDirty && pumpConfig) {
        var statusStr = pumpConfig.settings_status || 'loaded';
        setHtml('pump-config-state', renderSettingsStatus(statusStr, status.auto_start));
    }
    setPumpAlert('water-status-error', status.fault ? 'พบ relay fault ระบบบังคับปิด Relay 1' : '');
    setPumpAlert('wash-status-error', status.fault ? 'พบ relay fault ระบบบังคับปิด Relay 2' : '');

    updatePumpButtons();
}

function handlePumpStatusFailure() {
    var now = Date.now();
    var stale = !pumpLastSyncMs || (now - pumpLastSyncMs > PUMP_STALE_MS);
    var sync = pumpEl('water-sync-state');
    if (sync) {
        sync.className = stale ? 'runtime-eyebrow error' : 'runtime-eyebrow warn';
        sync.innerHTML = stale ? renderSvgIcon('icon-wifi-off') + ' Stale' : renderSvgIcon('icon-sync') + ' Syncing...';
    }
    var washSync = pumpEl('wash-sync-state');
    if (washSync) {
        washSync.className = stale ? 'runtime-eyebrow error' : 'runtime-eyebrow warn';
        washSync.innerHTML = stale ? renderSvgIcon('icon-wifi-off') + ' Stale' : renderSvgIcon('icon-sync') + ' Syncing...';
    }
    if (stale) {
        var pumpStatusMessage = 'ไม่สามารถอ่านสถานะปั๊มได้ชั่วคราว ปิดปุ่ม Start/Stop จนกว่าจะซิงค์สำเร็จ';
        setPumpAlert('water-status-error', pumpStatusMessage);
        setPumpAlert('wash-status-error', pumpStatusMessage);
        setText('water-active-set', '--');
        setText('water-phase-title', '--');
        setText('water-float-badge', 'Float: --');
        setText('water-relay-badge', 'Relay 1: --');
        setText('water-countdown', '--:--');
        setText('water-state-desc', 'ไม่สามารถอ่านสถานะจากอุปกรณ์ได้ (การเชื่อมต่อขาดหาย)');
        setText('wash-phase-title', '--');
        setText('wash-relay-badge', 'Relay 2: --');
        setText('wash-countdown', '--:--');
        setText('wash-state-desc', 'ไม่สามารถอ่านสถานะจากอุปกรณ์ได้ (การเชื่อมต่อขาดหาย)');
        var cooldownEl = pumpEl('pump-cooldown-badge');
        if (cooldownEl) {
            cooldownEl.classList.add('hidden');
            cooldownEl.textContent = '';
        }

        updateBadgeCardState('pump-float-card', false);
        updateBadgeCardState('pump-relay-1-card', false);
        updateBadgeCardState('pump-relay-2-card', false);

        ['water-float-badge', 'water-relay-badge', 'wash-relay-badge'].forEach(function (id) {
            var el = pumpEl(id);
            if (el) el.classList.remove('badge-on');
        });

        var t1Card = pumpEl('water-set1-card-panel');
        var t2Card = pumpEl('water-set2-card-panel');
        if (t1Card) t1Card.classList.remove('active-timer');
        if (t2Card) t2Card.classList.remove('active-timer');
        if (t1Card) t1Card.setAttribute('data-runtime-active', 'false');
        if (t2Card) t2Card.setAttribute('data-runtime-active', 'false');
        var waterBlock = document.querySelector('.settings-system-water');
        if (waterBlock) {
            var waterSetButtons = waterBlock.querySelectorAll('.settings-segment-btn');
            for (var i = 0; i < waterSetButtons.length; i++) {
                waterSetButtons[i].classList.remove('runtime-active');
                waterSetButtons[i].setAttribute('data-runtime-active', 'false');
                var runtimeIndicator = waterSetButtons[i].querySelector('.settings-runtime-indicator');
                if (runtimeIndicator) runtimeIndicator.hidden = true;
            }
        }

        var waterLed = pumpEl('water-led-dot');
        if (waterLed) waterLed.className = 'pump-led-dot status-stopped';
        var washLed = pumpEl('wash-led-dot');
        if (washLed) washLed.className = 'pump-led-dot status-stopped';
        var wBadge = pumpEl('water-status-badge');
        if (wBadge) {
            wBadge.classList.remove('badge-active', 'badge-paused', 'badge-offline');
            wBadge.classList.add('badge-stopped');
        }
        var wLed = pumpEl('water-led-dot');
        if (wLed) wLed.className = 'pump-led-dot status-stopped';

        var wsBadge = pumpEl('wash-status-badge');
        if (wsBadge) {
            wsBadge.classList.remove('badge-active', 'badge-paused', 'badge-offline');
            wsBadge.classList.add('badge-stopped');
        }
        var wsLed = pumpEl('wash-led-dot');
        if (wsLed) wsLed.className = 'pump-led-dot status-stopped';

        applySystemPhaseVisuals(pumpEl('water-countdown'), pumpEl('water-progress-bar-fill'), null);
        applySystemPhaseVisuals(pumpEl('wash-countdown'), pumpEl('wash-progress-bar-fill'), null);
    }
    updatePumpButtons();
}

function updatePumpButtons() {
    var waterStartBtn = pumpEl('water-start-btn');
    var waterStopBtn = pumpEl('water-stop-btn');
    var washStartBtn = pumpEl('wash-start-btn');
    var washStopBtn = pumpEl('wash-stop-btn');
    var legacyStartBtn = pumpEl('pump-start-btn');
    var legacyStopBtn = pumpEl('pump-stop-btn');

    var stale = !pumpLastSyncMs || (Date.now() - pumpLastSyncMs > PUMP_STALE_MS);
    var baseDisabled = stale || !pumpLastStatus || pumpLastStatus.config_valid === false;
    var waterDisabled = waterPending || baseDisabled;
    var washDisabled = washPending || baseDisabled;

    var waterRunning = pumpLastStatus && ((pumpLastStatus.system_water && pumpLastStatus.system_water.running) || pumpLastStatus.running);
    var washRunning = pumpLastStatus && pumpLastStatus.system_wash && pumpLastStatus.system_wash.running;

    if (waterStartBtn) waterStartBtn.disabled = waterDisabled || !!waterRunning;
    if (waterStopBtn) waterStopBtn.disabled = waterDisabled || !waterRunning;
    if (washStartBtn) washStartBtn.disabled = washDisabled || !!washRunning;
    if (washStopBtn) washStopBtn.disabled = washDisabled || !washRunning;
    if (legacyStartBtn) legacyStartBtn.disabled = waterDisabled || !!waterRunning;
    if (legacyStopBtn) legacyStopBtn.disabled = waterDisabled || !waterRunning;

    var formDisabled = !pumpConfigLoaded || pumpLoadingConfig || pumpSavingConfig || waterPending || washPending || pumpPending || stale;
    var ids = [
        'timer1-on-min', 'timer1-on-sec', 'timer1-off-min', 'timer1-off-sec',
        'timer2-on-min', 'timer2-on-sec', 'timer2-off-min', 'timer2-off-sec',
        'wash-on-min', 'wash-on-sec', 'wash-off-min', 'wash-off-sec',
        'timer1-start-phase', 'timer2-start-phase', 'wash-start-phase',
        'pump-auto-start', 'wash-auto-start', 'pump-min-dwell'
    ];
    for (var i = 0; i < ids.length; i++) {
        var input = pumpEl(ids[i]);
        if (input) {
            input.disabled = formDisabled;
        }
    }

    var saveBtn = pumpEl('pump-save-config');
    if (saveBtn) {
        if (pumpSavingConfig) {
            setLoading(saveBtn, true);
        } else {
            setLoading(saveBtn, false);
            saveBtn.disabled = formDisabled || !pumpDirty;
        }
    }
}

function startPumpLiveTimers() {
    stopPumpLiveTimers();
    if (document.hidden) return;
    pumpPollTimer = setInterval(function () { syncPumpStatus(false); }, PUMP_STATUS_POLL_MS);
    pumpTickTimer = setInterval(tickPumpCountdown, PUMP_COUNTDOWN_TICK_MS);
}

function stopPumpLiveTimers() {
    if (pumpPollTimer) clearInterval(pumpPollTimer);
    if (pumpTickTimer) clearInterval(pumpTickTimer);
    pumpPollTimer = null;
    pumpTickTimer = null;
}

function handlePumpVisibilityChange() {
    if (document.hidden) {
        stopPumpLiveTimers();
    } else {
        startPumpLiveTimers();
        syncPumpStatus(true);
        refreshStatus(true);
    }
}

function tickPumpCountdown() {
    if (!pumpLastStatus) return;
    var waterRunning = pumpLastStatus.running || (pumpLastStatus.system_water && pumpLastStatus.system_water.running);
    var washRunning = pumpLastStatus.system_wash && pumpLastStatus.system_wash.running;

    if (waterRunning || washRunning) {
        var waterExpired = waterRunning && getWaterCountdownSec() <= 0;
        var washExpired = washRunning && getWashCountdownSec() <= 0;
        var advanced = false;
        if (waterExpired) {
            advanced = predictWaterPhaseAdvance();
        }
        if (!advanced && washExpired) {
            advanced = predictWashPhaseAdvance();
        }
        if ((waterExpired || washExpired) && !advanced) {
            requestPumpRolloverSync();
        }
        renderPumpCountdown();
    }
    if (pumpLastSyncMs && Date.now() - pumpLastSyncMs > PUMP_STALE_MS) {
        handlePumpStatusFailure();
    }
}

function pumpStatusKey(status) {
    if (!status) return '';
    return [
        status.running ? '1' : '0',
        status.initial_stabilizing ? '1' : '0',
        status.active_timer || '',
        status.active_relay || '',
        status.phase || ''
    ].join('|');
}

function updatePumpCountdownDeadline(status) {
    var countdown = Number(status && status.countdown_sec || 0);
    var key = pumpStatusKey(status);
    if (!status || !status.running || status.phase === 'idle' || countdown <= 0) {
        pumpCountdownDeadlineMs = null;
        pumpCountdownStatusKey = key;
        pumpDisplayedCountdownSec = countdown;
        return;
    }

    var nextDeadline = Date.now() + (countdown * 1000);
    var shouldReset = !pumpCountdownDeadlineMs ||
        pumpCountdownStatusKey !== key ||
        Math.abs(nextDeadline - pumpCountdownDeadlineMs) > PUMP_DEADLINE_DRIFT_MS;

    if (shouldReset) {
        pumpCountdownDeadlineMs = nextDeadline;
    }
    pumpCountdownStatusKey = key;
    pumpDisplayedCountdownSec = getPumpCountdownSec();
}

function getPumpCountdownSec() {
    if (!pumpCountdownDeadlineMs) {
        return pumpDisplayedCountdownSec;
    }
    return Math.max(0, Math.ceil((pumpCountdownDeadlineMs - Date.now()) / 1000));
}

function readPumpConfigDuration(setNum, phaseKey) {
    if (!pumpConfig) return null;
    var phase = String(phaseKey || '').toLowerCase();
    if (phase !== 'on' && phase !== 'off') return null;
    var legacyKey = 'timer' + setNum + '_' + phase + '_sec';
    var modernKey = 'water_set' + setNum + '_' + phase;
    var val = pumpConfig[modernKey];
    if (val == null) val = pumpConfig[legacyKey];
    val = Number(val);
    return (isFinite(val) && val > 0) ? val : null;
}

function readWashConfigDuration(phaseKey) {
    if (!pumpConfig) return null;
    var phase = String(phaseKey || '').toLowerCase();
    if (phase !== 'on' && phase !== 'off') return null;
    var val = pumpConfig['wash_' + phase + '_sec'];
    if (val == null && phase === 'on') val = pumpConfig.wash_on_sec;
    if (val == null && phase === 'off') val = pumpConfig.wash_off_sec;
    val = Number(val);
    return (isFinite(val) && val > 0) ? val : null;
}

function getWaterTotalDurationSec(sysWater) {
    if (sysWater && Number(sysWater.total_duration_sec) > 0) {
        return Number(sysWater.total_duration_sec);
    }
    if (!pumpConfig || !sysWater || !sysWater.phase || sysWater.phase.toUpperCase() === 'IDLE') return 0;
    var setNum = sysWater.active_set === 2 ? 2 : 1;
    var phaseKey = sysWater.phase.toLowerCase();
    var val = readPumpConfigDuration(setNum, phaseKey);
    return val || 0;
}

function getWashTotalDurationSec(sysWash) {
    if (sysWash && Number(sysWash.total_duration_sec) > 0) {
        return Number(sysWash.total_duration_sec);
    }
    if (!pumpConfig || !sysWash || !sysWash.phase || sysWash.phase.toUpperCase() === 'IDLE') return 0;
    var phaseKey = sysWash.phase.toLowerCase();
    var val = readWashConfigDuration(phaseKey);
    return val || 0;
}

function updateWaterCountdownDeadline(sysWater) {
    var countdown = Number(sysWater && sysWater.countdown_sec || 0);
    var key = [
        sysWater && sysWater.running ? '1' : '0',
        sysWater && sysWater.active_set || '',
        sysWater && sysWater.phase || ''
    ].join('|');
    if (!sysWater || !sysWater.running || !sysWater.phase || sysWater.phase.toUpperCase() === 'IDLE') {
        waterCountdownDeadlineMs = null;
        waterCountdownStatusKey = key;
        waterDisplayedCountdownSec = countdown;
        return;
    }
    if (countdown <= 0) {
        var fallback = getWaterTotalDurationSec(sysWater);
        if (fallback > 0) countdown = fallback;
    }
    if (countdown <= 0) {
        waterCountdownDeadlineMs = null;
        waterCountdownStatusKey = key;
        waterDisplayedCountdownSec = 0;
        return;
    }
    var nextDeadline = Date.now() + (countdown * 1000);
    var shouldReset = !waterCountdownDeadlineMs ||
        waterCountdownStatusKey !== key ||
        Math.abs(nextDeadline - waterCountdownDeadlineMs) > PUMP_DEADLINE_DRIFT_MS;
    if (shouldReset) {
        waterCountdownDeadlineMs = nextDeadline;
    }
    waterCountdownStatusKey = key;
    waterDisplayedCountdownSec = getWaterCountdownSec();
}

function getWaterCountdownSec() {
    if (!waterCountdownDeadlineMs) {
        return waterDisplayedCountdownSec;
    }
    return Math.max(0, Math.ceil((waterCountdownDeadlineMs - Date.now()) / 1000));
}

function updateWashCountdownDeadline(sysWash) {
    var countdown = Number(sysWash && sysWash.countdown_sec || 0);
    var key = [
        sysWash && sysWash.running ? '1' : '0',
        sysWash && sysWash.phase || ''
    ].join('|');
    if (!sysWash || !sysWash.running || !sysWash.phase || sysWash.phase.toUpperCase() === 'IDLE') {
        washCountdownDeadlineMs = null;
        washCountdownStatusKey = key;
        washDisplayedCountdownSec = countdown;
        return;
    }
    if (countdown <= 0) {
        var fallback = getWashTotalDurationSec(sysWash);
        if (fallback > 0) countdown = fallback;
    }
    if (countdown <= 0) {
        washCountdownDeadlineMs = null;
        washCountdownStatusKey = key;
        washDisplayedCountdownSec = 0;
        return;
    }
    var nextDeadline = Date.now() + (countdown * 1000);
    var shouldReset = !washCountdownDeadlineMs ||
        washCountdownStatusKey !== key ||
        Math.abs(nextDeadline - washCountdownDeadlineMs) > PUMP_DEADLINE_DRIFT_MS;
    if (shouldReset) {
        washCountdownDeadlineMs = nextDeadline;
    }
    washCountdownStatusKey = key;
    washDisplayedCountdownSec = getWashCountdownSec();
}

function getWashCountdownSec() {
    if (!washCountdownDeadlineMs) {
        return washDisplayedCountdownSec;
    }
    return Math.max(0, Math.ceil((washCountdownDeadlineMs - Date.now()) / 1000));
}

function renderPumpCountdown() {
    var stale = !pumpLastSyncMs || (Date.now() - pumpLastSyncMs > PUMP_STALE_MS);
    if (stale) {
        setText('water-countdown', '--:--');
        setText('wash-countdown', '--:--');
        var wBarStale = pumpEl('water-progress-bar-fill');
        var washBarStale = pumpEl('wash-progress-bar-fill');
        applySystemPhaseVisuals(pumpEl('water-countdown'), wBarStale, null);
        applySystemPhaseVisuals(pumpEl('wash-countdown'), washBarStale, null);
        if (wBarStale) wBarStale.style.transform = 'translateX(-100%)';
        if (washBarStale) washBarStale.style.transform = 'translateX(-100%)';
        var ring = document.getElementById('pump-progress-ring-circle');
        var bar = document.getElementById('pump-progress-bar-fill');
        var pctText = document.getElementById('pump-progress-pct');
        if (ring) {
            var circumference = 2 * Math.PI * 62;
            ring.style.strokeDasharray = circumference;
            ring.style.strokeDashoffset = circumference;
        }
        if (bar) {
            bar.style.transform = 'translateX(-100%)';
        }
        if (pctText) {
            pctText.textContent = '--%';
        }
        return;
    }

    // Render Water System Countdown & Progress Fill
    waterDisplayedCountdownSec = getWaterCountdownSec();
    setText('water-countdown', formatPumpCountdown(waterDisplayedCountdownSec));
    var sysWater = pumpLastStatus && pumpLastStatus.system_water;
    var waterCountdown = pumpEl('water-countdown');
    var waterBarFill = pumpEl('water-progress-bar-fill');
    applySystemPhaseVisuals(waterCountdown, waterBarFill, sysWater);
    if (waterBarFill && sysWater) {
        var waterTotal = getWaterTotalDurationSec(sysWater);
        var waterPct = (waterTotal > 0) ? Math.min(100, Math.max(0, (waterDisplayedCountdownSec / waterTotal) * 100)) : 0;
        waterBarFill.style.transform = 'translateX(' + (waterPct - 100).toFixed(1) + '%)';
    }

    // Render Wash System Countdown & Progress Fill
    washDisplayedCountdownSec = getWashCountdownSec();
    setText('wash-countdown', formatPumpCountdown(washDisplayedCountdownSec));
    var sysWash = pumpLastStatus && pumpLastStatus.system_wash;
    var washCountdown = pumpEl('wash-countdown');
    var washBarFill = pumpEl('wash-progress-bar-fill');
    applySystemPhaseVisuals(washCountdown, washBarFill, sysWash);
    if (washBarFill && sysWash) {
        var washTotal = getWashTotalDurationSec(sysWash);
        var washPct = (washTotal > 0) ? Math.min(100, Math.max(0, (washDisplayedCountdownSec / washTotal) * 100)) : 0;
        washBarFill.style.transform = 'translateX(' + (washPct - 100).toFixed(1) + '%)';
    }

    pumpDisplayedCountdownSec = getPumpCountdownSec();

    var ring = document.getElementById('pump-progress-ring-circle');
    var bar = document.getElementById('pump-progress-bar-fill');
    var pctText = document.getElementById('pump-progress-pct');

    var pct = 0;
    if (pumpLastStatus && pumpLastStatus.running && pumpLastStatus.active_timer && pumpLastStatus.phase) {
        var totalSec = getPumpDurationSec(pumpLastStatus.active_timer, pumpLastStatus.phase);
        if (totalSec > 0) {
            pct = pumpDisplayedCountdownSec / totalSec;
            if (pct < 0) pct = 0;
            if (pct > 1) pct = 1;
        }
    }

    if (ring) {
        var radius = 62;
        var circumference = 2 * Math.PI * radius;
        ring.style.strokeDasharray = circumference;
        ring.style.strokeDashoffset = circumference * (1 - pct);
    }

    if (bar) {
        bar.style.transform = 'translateX(' + (pct * 100 - 100).toFixed(1) + '%)';
        if (pumpLastStatus) {
            if (pumpLastStatus.phase === 'on') {
                bar.style.background = 'var(--secondary)';
            } else if (pumpLastStatus.phase === 'off') {
                bar.style.background = 'var(--tertiary)';
            } else {
                bar.style.background = 'var(--accent)';
            }
        }
    }

    if (pctText) {
        pctText.textContent = (pct * 100).toFixed(1) + '%';
    }
}

function getPumpDurationSec(timer, phase) {
    if (!pumpConfig || (timer !== 'timer1' && timer !== 'timer2')) return null;
    if (phase !== 'on' && phase !== 'off') return null;
    var value = Number(pumpConfig[timer + '_' + phase + '_sec']);
    if (!isFinite(value) || value <= 0) return null;
    return value;
}

function predictPumpPhaseAdvance() {
    if (!pumpLastStatus || !pumpLastStatus.running || pumpLastStatus.initial_stabilizing) return false;
    if (pumpLastStatus.phase !== 'on' && pumpLastStatus.phase !== 'off') return false;

    var nextPhase = pumpLastStatus.phase === 'on' ? 'off' : 'on';
    var nextDuration = getPumpDurationSec(pumpLastStatus.active_timer, nextPhase);
    if (!nextDuration) return false;

    pumpLastStatus.phase = nextPhase;
    pumpLastStatus.countdown_sec = nextDuration;
    pumpLastStatus.relay_energized = nextPhase === 'on';
    if (pumpLastStatus.active_relay === 'relay1') {
        pumpLastStatus.relay1_energized = nextPhase === 'on';
        pumpLastStatus.relay2_energized = false;
    } else if (pumpLastStatus.active_relay === 'relay2') {
        pumpLastStatus.relay1_energized = false;
        pumpLastStatus.relay2_energized = nextPhase === 'on';
    }
    pumpCountdownStatusKey = pumpStatusKey(pumpLastStatus);
    pumpCountdownDeadlineMs = Date.now() + (nextDuration * 1000);
    pumpDisplayedCountdownSec = nextDuration;

    updateBadgeCardState('pump-relay-1-card', relayEnergizedFor('relay1', pumpLastStatus));
    updateBadgeCardState('pump-relay-2-card', relayEnergizedFor('relay2', pumpLastStatus));
    return true;
}

function syncRootFieldsFromWater(sysWater) {
    if (!pumpLastStatus || !sysWater) return;
    pumpLastStatus.running = !!sysWater.running;
    pumpLastStatus.phase = sysWater.phase ? sysWater.phase.toLowerCase() : 'idle';
    pumpLastStatus.countdown_sec = Number(sysWater.countdown_sec || 0);
    pumpLastStatus.relay1_energized = !!sysWater.relay_energized;
    pumpLastStatus.relay_energized = !!sysWater.relay_energized;
    pumpLastStatus.active_timer = sysWater.active_set === 2 ? 'timer2' : 'timer1';
    pumpLastStatus.active_relay = 'relay1';
    if (sysWater.float_state) {
        pumpLastStatus.float_state = sysWater.float_state;
    }
}

function predictWaterPhaseAdvance() {
    if (!pumpLastStatus) return false;
    if (!pumpLastStatus.system_water) {
        pumpLastStatus.system_water = {};
    }
    var sys = pumpLastStatus.system_water;
    if (!sys.running || sys.phase === 'IDLE') return false;
    if (sys.phase !== 'ON' && sys.phase !== 'OFF') return false;

    var nextPhase = sys.phase === 'ON' ? 'OFF' : 'ON';
    var setNum = sys.active_set === 2 ? 2 : 1;
    var nextDuration = readPumpConfigDuration(setNum, nextPhase.toLowerCase());
    if (!nextDuration) return false;

    sys.phase = nextPhase;
    sys.countdown_sec = nextDuration;
    sys.total_duration_sec = nextDuration;
    sys.relay_energized = nextPhase === 'ON';
    syncRootFieldsFromWater(sys);
    updatePumpCountdownDeadline(pumpLastStatus);
    updateWaterCountdownDeadline(sys);
    applyPumpStatus(pumpLastStatus, false);
    return true;
}

function predictWashPhaseAdvance() {
    if (!pumpLastStatus) return false;
    if (!pumpLastStatus.system_wash) {
        pumpLastStatus.system_wash = {};
    }
    var sys = pumpLastStatus.system_wash;
    if (!sys.running || sys.phase === 'IDLE') return false;
    if (sys.phase !== 'ON' && sys.phase !== 'OFF') return false;

    var nextPhase = sys.phase === 'ON' ? 'OFF' : 'ON';
    var nextDuration = readWashConfigDuration(nextPhase.toLowerCase());
    if (!nextDuration) return false;

    sys.phase = nextPhase;
    sys.countdown_sec = nextDuration;
    sys.total_duration_sec = nextDuration;
    sys.relay_energized = nextPhase === 'ON';
    pumpLastStatus.relay2_energized = nextPhase === 'ON';
    updateWashCountdownDeadline(sys);
    applyPumpStatus(pumpLastStatus, false);
    return true;
}

function requestPumpRolloverSync() {
    if (pumpAwaitingRolloverSync) return;
    pumpAwaitingRolloverSync = true;
    syncPumpStatus(true);
}

function formatPumpCountdown(value) {
    if (value === null || value === undefined || isNaN(value)) return '--:--';
    var seconds = Math.max(0, Math.floor(Number(value)));
    var minutes = Math.floor(seconds / 60);
    var rem = seconds % 60;
    return minutes + ':' + (rem < 10 ? '0' : '') + rem;
}

function renderSvgIcon(iconName, colorClass) {
    var cls = 'stat-icon' + (colorClass ? ' ' + colorClass : '');
    return '<svg class="' + cls + '" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><use href="#' + iconName + '"></use></svg>';
}

function renderPumpTimer(value) {
    if (value === 'timer1') return 'Timer 1';
    if (value === 'timer2') return 'Timer 2';
    return '--';
}

function renderPumpTimerBadge(value) {
    if (value === 'timer1') return renderSvgIcon('icon-timer', 'status-success') + ' <span class="status-success">Timer 1</span>';
    if (value === 'timer2') return renderSvgIcon('icon-timer', 'status-success') + ' <span class="status-success">Timer 2</span>';
    return '--';
}

function renderPumpStateDesc(status) {
    if (!status) return '--';
    if (status.fault) {
        return 'พบข้อผิดพลาดของระบบรีเลย์ (Relay Fault) บังคับปิดปั๊มทั้งหมดเพื่อความปลอดภัย';
    }
    if (status.initial_stabilizing) {
        return 'ระบบกำลังรอสัญญาณเซนเซอร์และสถานะปั๊มให้เสถียรชั่วคราว...';
    }
    if (!status.running) {
        if (status.active_timer && status.active_timer !== 'none') {
            return 'ระบบหยุดทำงานชั่วคราว (สแตนด์บายรอบ ' + renderPumpTimer(status.active_timer) + ')';
        }
        return 'ระบบหยุดทำงานควบคุมปั๊มทั้งหมดแล้ว';
    }
    if (status.active_timer === 'timer1') {
        if (status.phase === 'on') {
            return 'รอบเวลา Timer 1: ปั๊มกำลังทำงาน';
        }
        if (status.phase === 'off') {
            return 'รอบเวลา Timer 1: ปั๊มหยุดพัก';
        }
    }
    if (status.active_timer === 'timer2') {
        if (status.phase === 'on') {
            return 'รอบเวลา Timer 2: ปั๊มกำลังทำงาน';
        }
        if (status.phase === 'off') {
            return 'รอบเวลา Timer 2: ปั๊มหยุดพัก';
        }
    }
    return 'ระบบกำลังประมวลผลการทำงาน';
}

function renderPumpRunLabel(status) {
    if (!status) return '--';
    if (status.running) {
        if (status.phase === 'off') {
            return '<span class="pump-led-dot status-warning"></span><span class="status-warning">พักการทำงาน</span>';
        }
        return '<span class="pump-led-dot status-success"></span><span class="status-success">ทำงาน</span>';
    }
    return '<span class="pump-led-dot status-danger"></span><span class="status-danger">หยุด</span>';
}

function renderPumpPhase(timer, phase, running) {
    if (!timer || timer === 'none' || !phase || phase === 'idle') {
        return renderSvgIcon('icon-pause') + ' Idle';
    }
    var timerShort = timer === 'timer1' ? 'T1' : 'T2';
    var prefix = running ? '' : 'Ready ';
    if (phase === 'on') {
        return renderSvgIcon('icon-relay-on', 'status-success') + timerShort + ' ' + prefix + '<span class="status-success">ON</span>';
    }
    if (phase === 'off') {
        return renderSvgIcon('icon-pause', 'status-danger') + timerShort + ' ' + prefix + '<span class="status-danger">OFF</span>';
    }
    return timerShort + ' - --';
}

function renderFloatState(value) {
    if (value === 'on') {
        return '<span class="status-success">ON</span> <span class="badge-subtext">→ Timer 2</span>';
    }
    if (value === 'off') {
        return '<span class="status-danger">OFF</span> <span class="badge-subtext">→ Timer 1</span>';
    }
    return '<span class="status-warning">Unknown</span>';
}

function renderRelayName(value) {
    if (value === 'relay1') return 'Relay 1';
    if (value === 'relay2') return 'Relay 2';
    return 'Relay: none';
}

function relayEnergizedFor(channel, status) {
    if (!status) return false;
    var field = channel === 'relay1' ? status.relay1_energized : status.relay2_energized;
    if (typeof field === 'boolean') return field;
    if (status.active_relay === channel) return !!status.relay_energized;
    return false;
}

function renderActiveRelayState(status) {
    if (!status || !status.active_relay || status.active_relay === 'none') return '--';
    var label = status.active_relay === 'relay1' ? 'R1' : 'R2';
    if (!status.running) {
        return renderSvgIcon('icon-relay-off', 'status-danger') + label + ' <span class="status-danger">OFF</span>';
    }
    if (status.relay_energized) {
        return renderSvgIcon('icon-relay-on', 'status-success') + label + ' <span class="status-success">ON</span>';
    }
    return renderSvgIcon('icon-relay-off', 'status-danger') + label + ' <span class="status-danger">OFF</span>';
}

function renderRelayChannelState(channel, status) {
    var energized = relayEnergizedFor(channel, status);
    if (energized) {
        return '<span class="status-success">ON</span>';
    }
    return '<span class="status-danger">OFF</span>';
}

function renderSettingsStatus(value, autoStart) {
    var prefix = '';
    if (value !== 'loaded') {
        var icon = renderSvgIcon('icon-alert-triangle', 'status-warning');
        var label = 'Error';
        if (value === 'defaults_missing') label = 'Defaults';
        else if (value === 'defaults_invalid') label = 'Invalid';
        prefix = icon + ' ' + label + ' &bull; ';
    }
    var autoIcon = autoStart ? renderSvgIcon('icon-auto', 'status-success') : renderSvgIcon('icon-stop', 'status-danger');
    return prefix + autoIcon + ' Auto-start: ' + (autoStart ? 'ON' : 'OFF');
}


/* ======== Cooling Dashboard ======== */

function wireCoolingForm() {
    var form = pumpEl('cooling-config-form');
    if (form) {
        form.onsubmit = function (e) {
            e.preventDefault();
            saveCoolingConfig();
        };
    }

    var ids = [
        'cooling-threshold-input',
        'cooling-hysteresis-input',
        'cooling-test-timeout',
        'cooling-min-off',
        'cooling-auto-enable'
    ];
    for (var i = 0; i < ids.length; i++) {
        var input = pumpEl(ids[i]);
        if (!input) continue;
        input.oninput = function () {
            markCoolingDirty();
            updateCoolingSettingsPreview();
            updateCoolingLockoutHint();
        };
        input.onchange = function () {
            markCoolingDirty();
            updateCoolingSettingsPreview();
            updateCoolingLockoutHint();
        };
    }
}

function wireCoolingActions() {
    var autoBtn = pumpEl('cooling-mode-auto');
    var forceBtn = pumpEl('cooling-mode-force-off');
    var testBtn = pumpEl('cooling-test-on');
    if (autoBtn) autoBtn.onclick = function () { setCoolingMode('auto'); };
    if (forceBtn) forceBtn.onclick = function () { setCoolingMode('force_off'); };
    if (testBtn) testBtn.onclick = function () { setCoolingMode('test_on'); };
}

function checkCoolingDirty() {
    if (!coolingConfig) return false;

    var threshInput = pumpEl('cooling-threshold-input');
    var threshVal = threshInput ? threshInput.value.trim() : '';
    if (threshVal === '') return true;
    var thresholdX10 = Math.round(Number(threshVal) * 10);

    var hystInput = pumpEl('cooling-hysteresis-input');
    var hystVal = hystInput ? hystInput.value.trim() : '';
    if (hystVal === '') return true;
    var hysteresisX10 = Math.round(Number(hystVal) * 10);

    var timeoutInput = pumpEl('cooling-test-timeout');
    var timeoutVal = timeoutInput ? timeoutInput.value.trim() : '';
    if (timeoutVal === '') return true;
    var testTimeout = parseInt(timeoutVal, 10);

    var minOffInput = pumpEl('cooling-min-off');
    var minOffVal = minOffInput ? minOffInput.value.trim() : '';
    if (minOffVal === '') return true;
    var minOff = parseInt(minOffVal, 10);

    var autoEnableInput = pumpEl('cooling-auto-enable');
    var autoEnable = autoEnableInput ? !!autoEnableInput.checked : false;

    if (thresholdX10 !== Number(coolingConfig.threshold_c_x10)) return true;
    if (hysteresisX10 !== Number(coolingConfig.hysteresis_c_x10)) return true;
    if (testTimeout !== Number(coolingConfig.test_timeout_sec)) return true;
    if (minOff !== Number(coolingConfig.compressor_min_off_sec)) return true;
    if (autoEnable !== !!coolingConfig.auto_enable) return true;

    return false;
}

function markCoolingDirty() {
    var dirty = checkCoolingDirty();
    coolingDirty = dirty;
    var warning = pumpEl('cooling-unsaved-warning');
    var saveBar = pumpEl('cooling-settings-save-bar');
    if (saveBar) saveBar.classList.toggle('is-dirty', dirty);
    if (warning) {
        if (dirty) {
            warning.classList.remove('hidden');
            setText('cooling-config-state', 'มีแก้ไขที่ยังไม่ได้บันทึก');
        } else {
            warning.classList.add('hidden');
            if (coolingConfig && coolingConfig.settings_status && coolingLastStatus) {
                setHtml('cooling-config-state', renderSettingsStatus(coolingConfig.settings_status, coolingLastStatus.auto_enable));
            } else {
                setText('cooling-config-state', 'โหลดค่า cooling แล้ว');
            }
        }
    }
    updateCoolingButtons();
}

function setCoolingClean(label) {
    coolingDirty = false;
    var warning = pumpEl('cooling-unsaved-warning');
    var saveBar = pumpEl('cooling-settings-save-bar');
    if (saveBar) saveBar.classList.remove('is-dirty');
    if (warning) warning.classList.add('hidden');
    if (coolingConfig) {
        var statusStr = coolingConfig.settings_status || 'loaded';
        var autoEnabled = coolingConfig.auto_enable !== undefined ? !!coolingConfig.auto_enable : (coolingLastStatus ? coolingLastStatus.auto_enable : false);
        setHtml('cooling-config-state', renderSettingsStatus(statusStr, autoEnabled));
    } else {
        setText('cooling-config-state', label || 'บันทึกแล้ว');
    }
    updateCoolingButtons();
}

function updateCoolingConfigSaveButton() {
    var saveBtn = pumpEl('cooling-save-config');
    if (saveBtn) {
        var stale = !coolingLastSyncMs || (Date.now() - coolingLastSyncMs > COOLING_STALE_MS);
        var formDisabled = !coolingConfigLoaded || coolingLoadingConfig || coolingSavingConfig || coolingPending || stale;
        if (coolingSavingConfig || coolingPending) {
            setLoading(saveBtn, true);
        } else {
            setLoading(saveBtn, false);
            saveBtn.disabled = formDisabled || !coolingDirty;
        }
    }
}

function loadCoolingConfig() {
    setText('cooling-config-state', 'กำลังโหลดค่า...');
    setCoolingAlert('');
    coolingLoadingConfig = true;
    updateCoolingButtons();
    apiGet('/api/cooling/config', function (err, data) {
        coolingLoadingConfig = false;
        if (isUnauthorizedError(err)) { handleUnauthorized(); return; }
        if (err || !data || !data.ok) {
            coolingConfigLoaded = false;
            setText('cooling-config-state', 'โหลดค่า cooling ไม่สำเร็จ');
            setCoolingAlert('โหลดค่า cooling ไม่สำเร็จ');
            updateCoolingButtons();
            return;
        }
        coolingConfig = data;
        coolingConfigLoaded = true;
        setCoolingConfigFields(data);
        if (data.status) applyCoolingStatus(data.status, true);
        setCoolingClean('โหลดค่า cooling แล้ว');
    });
}

function setCoolingConfigFields(data) {
    setNumericInput('cooling-threshold-input', x10ToInput(data.threshold_c_x10));
    setNumericInput('cooling-hysteresis-input', x10ToInput(data.hysteresis_c_x10));
    setNumericInput('cooling-test-timeout', data.test_timeout_sec);
    setNumericInput('cooling-min-off', data.compressor_min_off_sec);
    var auto = pumpEl('cooling-auto-enable');
    if (auto) auto.checked = !!data.auto_enable;
    updateCoolingSettingsPreview();
    updateCoolingLockoutHint();
}

function setNumericInput(id, value) {
    var el = pumpEl(id);
    if (el && value !== null && value !== undefined) el.value = value;
}

function x10ToInput(value) {
    if (value === null || value === undefined || isNaN(value)) return '';
    return (Number(value) / 10).toFixed(1);
}

function readCoolingX10(id, label, minValue, maxValue) {
    var el = pumpEl(id);
    var raw = el ? el.value.trim() : '';
    var value = Number(raw);
    if (raw === '' || !isFinite(value)) {
        return { ok: false, message: label + ': กรุณากรอกตัวเลข', value: 0 };
    }
    var x10 = Math.round(value * 10);
    if (Math.abs((value * 10) - x10) > 0.0001 || x10 < minValue || x10 > maxValue) {
        return { ok: false, message: label + ': ค่าอยู่นอกช่วงที่รองรับ', value: 0 };
    }
    return { ok: true, value: x10 };
}

function readCoolingU32(id, label, minValue, maxValue) {
    var el = pumpEl(id);
    var raw = el ? el.value.trim() : '';
    if (!/^\d+$/.test(raw)) {
        return { ok: false, message: label + ': ต้องเป็นจำนวนเต็ม', value: 0 };
    }
    var value = parseInt(raw, 10);
    if (value < minValue || value > maxValue) {
        return { ok: false, message: label + ': ค่าอยู่นอกช่วงที่รองรับ', value: 0 };
    }
    return { ok: true, value: value };
}

function buildCoolingConfigPayload() {
    if (!coolingConfig) return null;
    var threshold = readCoolingX10('cooling-threshold-input', 'อุณหภูมิเป้าหมาย', -550, 1250);
    var hysteresis = readCoolingX10('cooling-hysteresis-input', 'ช่วงอุณหภูมิเบี่ยงเบน', 1, 500);
    var testTimeout = readCoolingU32('cooling-test-timeout', 'ระยะเวลาเปิดทดสอบ', 1, 3600);
    var minOff = readCoolingU32('cooling-min-off', 'เวลาล็อกป้องกันซ้ำ', 120, 86400);
    if (!threshold.ok) return threshold;
    if (!hysteresis.ok) return hysteresis;
    if (!testTimeout.ok) return testTimeout;
    if (!minOff.ok) return minOff;

    return {
        ok: true,
        payload: {
            threshold_c_x10: threshold.value,
            hysteresis_c_x10: hysteresis.value,
            auto_enable: !!(pumpEl('cooling-auto-enable') && pumpEl('cooling-auto-enable').checked),
            mode: coolingConfig.mode === 'auto' ? 'auto' : 'force_off',
            test_timeout_sec: testTimeout.value,
            compressor_min_off_sec: minOff.value,
            cooling_relay_polarity: coolingConfig.cooling_relay_polarity || 'active_low'
        }
    };
}

function saveCoolingConfig() {
    if (!coolingConfigLoaded || coolingPending) return;
    var built = buildCoolingConfigPayload();
    if (!built || !built.ok) {
        setCoolingAlert((built && built.message) ? built.message : 'ตรวจสอบค่า cooling แล้วลองบันทึกอีกครั้ง');
        return;
    }

    coolingSavingConfig = true;
    updateCoolingButtons();
    setCoolingAlert('');
    setText('cooling-config-state', 'กำลังบันทึก cooling...');
    apiPost('/api/cooling/config', built.payload, function (err, data) {
        coolingSavingConfig = false;
        if (err || !data || !data.ok) {
            setText('cooling-config-state', 'บันทึก cooling ไม่สำเร็จ');
            setCoolingAlert((data && data.message) ? data.message : 'บันทึก cooling ไม่สำเร็จ');
            updateCoolingButtons();
            return;
        }
        coolingConfig = data.payload || data;
        coolingConfigLoaded = true;
        setCoolingClean('บันทึก cooling แล้ว');
        showToast('บันทึกค่า cooling เรียบร้อย', 'success');
        if (data.status) applyCoolingStatus(data.status, true);
        syncCoolingStatus(true);
    });
}

function setCoolingMode(mode) {
    if (coolingPending) return;
    coolingPending = true;
    var label = mode === 'auto' ? 'อัตโนมัติ (Auto)' : (mode === 'test_on' ? 'เปิดทดสอบ (Test ON)' : 'บังคับปิด (Force OFF)');
    setText('cooling-mode-action-state', 'กำลังสั่ง ' + label + '...');
    updateCoolingButtons();
    apiPost('/api/cooling/mode', { mode: mode }, function (err, data) {
        coolingPending = false;
        if (err || !data || !data.ok) {
            setText('cooling-mode-action-state', '');
            setCoolingAlert((data && data.message) ? data.message : 'สั่งงาน cooling ไม่สำเร็จ');
            updateCoolingButtons();
            showToast('สั่งงาน cooling ไม่สำเร็จ', 'error');
            return;
        }
        coolingConfig = data;
        coolingConfigLoaded = true;
        if (data.status) applyCoolingStatus(data.status, true);
        setText('cooling-mode-action-state', '');
        setCoolingAlert('');
        updateCoolingButtons();
        showToast('สั่งงาน cooling เรียบร้อย', 'success');
        syncCoolingStatus(true);
    });
}

function syncCoolingStatus(force) {
    if (window.location.pathname !== '/cooling') return;
    if (document.hidden && !force) return;
    if (coolingStatusRequestInFlight) return;
    coolingStatusRequestInFlight = true;
    apiGet('/api/cooling/status', function (err, data) {
        coolingStatusRequestInFlight = false;
        if (isUnauthorizedError(err)) { handleUnauthorized(); return; }
        if (err || !data || !data.ok) {
            handleCoolingStatusFailure();
            return;
        }
        applyCoolingStatus(data, true);
    });
}

function applyCoolingStatus(status, authoritative) {
    coolingLastStatus = status;
    if (authoritative) coolingLastSyncMs = Date.now();

    setHtml('cooling-temperature', renderCoolingTemperature(status));
    setHtml('cooling-relay-state', status.relay_energized
        ? '<span class="status-success">ON</span>'
        : '<span class="status-danger">OFF</span>');

    updateBadgeCardState('cooling-relay-card', status.relay_energized);
    updateBadgeCardState('cooling-sensor-card', status.sensor_state === 'ok');

    setHtml('cooling-mode-state', renderCoolingMode(status.mode));
    setHtml('cooling-sensor-state', renderCoolingSensor(status.sensor_state));
    setHtml('cooling-fault-state', renderCoolingFault(status));
    if (typeof status.threshold_c === 'number') {
        setText('cooling-start-temp', renderCoolingCelsius(status.threshold_c));
        if (typeof status.hysteresis_c === 'number') {
            setText('cooling-stop-temp', renderCoolingCelsius(status.threshold_c - status.hysteresis_c));
        } else {
            setText('cooling-stop-temp', '--');
        }
    } else {
        setText('cooling-start-temp', '--');
        setText('cooling-stop-temp', '--');
    }
    setHtml('cooling-auto-enable-state', status.auto_enable
        ? renderSvgIcon('icon-auto', 'status-success') + ' <span class="status-success">ON</span>'
        : renderSvgIcon('icon-stop', 'status-danger') + ' <span class="status-danger">OFF</span>');
    setHtml('cooling-blocked-reason', renderCoolingBlocked(status.blocked_reason));
    setText('cooling-lockout', status.lockout_active ? formatPumpCountdown(status.lockout_remaining_sec) : '--');
    setText('cooling-test-remaining', status.test_remaining_sec ? formatPumpCountdown(status.test_remaining_sec) : '--');
    var lockoutTestStr = '--';
    if (status.test_remaining_sec > 0) {
        lockoutTestStr = 'TEST ' + formatPumpCountdown(status.test_remaining_sec);
    } else if (status.lockout_active) {
        lockoutTestStr = 'LOCK ' + formatPumpCountdown(status.lockout_remaining_sec);
    }
    setText('cooling-lockout-test-state', lockoutTestStr);

    if (!coolingDirty && coolingConfig) {
        var statusStr = coolingConfig.settings_status || 'loaded';
        setHtml('cooling-config-state', renderSettingsStatus(statusStr, status.auto_enable));
    }

    var sync = pumpEl('cooling-sync-state');
    if (sync) {
        sync.className = status.fault || status.sensor_state === 'fault'
            ? 'runtime-eyebrow error'
            : (status.lockout_active || status.mode === 'test_on' ? 'runtime-eyebrow warn' : 'runtime-eyebrow good');
        sync.innerHTML = status.fault || status.sensor_state === 'fault'
            ? renderSvgIcon('icon-x-circle') + ' Sensor Fault (Relay OFF)'
            : (status.lockout_active ? renderSvgIcon('icon-timer') + ' Compressor Lockout' : renderSvgIcon('icon-check-circle') + ' Synced');
    }
    setCoolingAlert(status.fault ? 'พบ sensor fault ระบบบังคับปิด cooling relay แล้ว' : '');
    updateCoolingButtons();
}

function handleCoolingStatusFailure() {
    var stale = !coolingLastSyncMs || (Date.now() - coolingLastSyncMs > COOLING_STALE_MS);
    var sync = pumpEl('cooling-sync-state');
    if (sync) {
        sync.className = stale ? 'runtime-eyebrow error' : 'runtime-eyebrow warn';
        sync.innerHTML = stale ? renderSvgIcon('icon-wifi-off') + ' Stale' : renderSvgIcon('icon-sync') + ' Syncing...';
    }
    if (stale) {
        setCoolingAlert('ไม่สามารถอ่านสถานะ cooling ได้ชั่วคราว');
        setHtml('cooling-temperature', '--');
        setHtml('cooling-relay-state', '--');

        updateBadgeCardState('cooling-relay-card', false);
        updateBadgeCardState('cooling-sensor-card', false);

        setHtml('cooling-mode-state', '--');
        setHtml('cooling-sensor-state', '--');
        setHtml('cooling-fault-state', '--');
        setText('cooling-start-temp', '--');
        setText('cooling-stop-temp', '--');
        setHtml('cooling-auto-enable-state', '--');
        setHtml('cooling-blocked-reason', '--');
        setText('cooling-lockout', '--');
        setText('cooling-test-remaining', '--');
        setText('cooling-lockout-test-state', '--');
    }
    updateCoolingButtons();
}

function updateCoolingButtons() {
    var stale = !coolingLastSyncMs || (Date.now() - coolingLastSyncMs > COOLING_STALE_MS);
    var disabled = coolingPending || stale || !coolingLastStatus || coolingLastStatus.config_valid === false;
    var autoBtn = pumpEl('cooling-mode-auto');
    var forceBtn = pumpEl('cooling-mode-force-off');
    var testBtn = pumpEl('cooling-test-on');
    var currentMode = coolingLastStatus ? coolingLastStatus.mode : null;

    if (autoBtn) autoBtn.disabled = disabled || (currentMode === 'auto');
    if (forceBtn) forceBtn.disabled = disabled || (currentMode === 'force_off');
    if (testBtn) testBtn.disabled = disabled || (currentMode === 'test_on');

    var formDisabled = !coolingConfigLoaded || coolingLoadingConfig || coolingSavingConfig || coolingPending || stale;
    var ids = [
        'cooling-threshold-input',
        'cooling-hysteresis-input',
        'cooling-test-timeout',
        'cooling-min-off',
        'cooling-auto-enable'
    ];
    for (var i = 0; i < ids.length; i++) {
        var input = pumpEl(ids[i]);
        if (input) {
            input.disabled = formDisabled;
        }
    }

    updateCoolingConfigSaveButton();
}

function startCoolingStatusTimer() {
    stopCoolingStatusTimer();
    if (document.hidden) return;
    coolingPollTimer = setInterval(function () { syncCoolingStatus(false); }, COOLING_STATUS_POLL_MS);
}

function stopCoolingStatusTimer() {
    if (coolingPollTimer) clearInterval(coolingPollTimer);
    coolingPollTimer = null;
}

function handleCoolingVisibilityChange() {
    if (document.hidden) {
        stopCoolingStatusTimer();
    } else {
        startCoolingStatusTimer();
        syncCoolingStatus(true);
        refreshStatus(true);
    }
}

function renderCoolingTemperature(status) {
    if (!status || !status.temperature_valid) return '--';
    var value = Number(status.temperature_c);
    if (!isFinite(value)) return '--';
    return value.toFixed(1) + ' °C';
}

function renderCoolingCelsius(value) {
    var num = Number(value);
    return isFinite(num) ? num.toFixed(1) + ' °C' : '--';
}

function renderCoolingMode(value) {
    if (value === 'auto') return renderSvgIcon('icon-auto', 'status-success') + ' Auto';
    if (value === 'test_on') return renderSvgIcon('icon-play', 'status-warning') + ' Test ON';
    if (value === 'force_off') return renderSvgIcon('icon-stop', 'status-danger') + ' Force OFF';
    return '--';
}

function renderCoolingSensor(value) {
    if (value === 'ok') return '<span class="status-success">OK</span>';
    if (value === 'fault') return '<span class="status-danger">FAULT</span>';
    return 'Unknown';
}

function renderCoolingFault(status) {
    if (!status) return '--';
    if (!status.fault) return '<span class="status-success">None</span>';
    return '<span class="status-danger">' + (status.fault_code || 'FAULT') + '</span>';
}

function renderCoolingBlocked(value) {
    if (!value || value === 'none') return '--';
    if (value === 'compressor_lockout') return 'Lockout';
    if (value === 'sensor_fault') return 'Sensor Fault';
    if (value === 'force_off') return 'Force OFF';
    if (value === 'config_invalid') return 'Config Invalid';
    return value;
}

/* ======== Hardware/Install Page ======== */

function initHardwareInstall() {
    if (window.location.pathname !== '/hardware') return;
    wireHardwareForm();
    updateHardwareSaveButton();
    loadHardwareMap();
    loadDimSetting();
}

function hardwareEl(id) {
    return document.getElementById(id);
}

function wireHardwareForm() {
    var form = hardwareEl('hardware-map-form');
    if (form) {
        form.addEventListener('submit', function (e) {
            e.preventDefault();
            saveHardwareMap();
        });
    }

    for (var i = 0; i < HARDWARE_FIELDS.length; i++) {
        var select = hardwareEl(HARDWARE_FIELDS[i].select);
        if (select) {
            select.addEventListener('change', markHardwareDirty);
        }
    }

    var confirm = hardwareEl('hardware-confirm-reboot');
    if (confirm) {
        confirm.addEventListener('change', updateHardwareSaveButton);
    }
}

function setHardwareState(text) {
    setText('hardware-map-state', text);
}

function setHardwareError(message) {
    var el = hardwareEl('hardware-map-error');
    if (!el) return;
    el.textContent = message || '';
}

function checkHardwareDirty() {
    if (!hardwareMapData) return false;
    var currentForm = readHardwareMapForm();
    if (!currentForm) return false;
    var baseline = hardwareMapData.pending_valid && hardwareMapData.pending ? hardwareMapData.pending : hardwareMapData.active;
    if (!baseline) return false;

    for (var i = 0; i < HARDWARE_FIELDS.length; i++) {
        var key = HARDWARE_FIELDS[i].key;
        if (String(currentForm[key]) !== String(baseline[key])) {
            return true;
        }
    }
    return false;
}

function markHardwareDirty() {
    var dirty = checkHardwareDirty();
    hardwareDirty = dirty;
    var banner = hardwareEl('hardware-unsaved-banner');
    if (banner) {
        if (dirty) {
            banner.classList.remove('hidden');
        } else {
            banner.classList.add('hidden');
        }
    }
    updateHardwareSaveButton();
}

function setHardwareClean() {
    hardwareDirty = false;
    hardwarePending = false;
    var banner = hardwareEl('hardware-unsaved-banner');
    if (banner) banner.classList.add('hidden');
    var confirm = hardwareEl('hardware-confirm-reboot');
    if (confirm) confirm.checked = false;
    updateHardwareSaveButton();
}

function updateHardwareSaveButton() {
    var btn = hardwareEl('hardware-save-map');
    var confirm = hardwareEl('hardware-confirm-reboot');
    var formDisabled = !hardwareMapData || hardwareLoadingMap || hardwareSavingMap || hardwarePending;

    for (var i = 0; i < HARDWARE_FIELDS.length; i++) {
        var select = hardwareEl(HARDWARE_FIELDS[i].select);
        if (select) {
            select.disabled = formDisabled;
        }
    }

    if (confirm) {
        confirm.disabled = formDisabled;
    }

    if (btn) {
        if (hardwareSavingMap || hardwarePending) {
            setLoading(btn, true);
        } else {
            setLoading(btn, false);
            btn.disabled = formDisabled || !hardwareDirty || !confirm || !confirm.checked;
        }
    }
}

function loadHardwareMap() {
    setHardwareState('กำลังโหลดผัง GPIO...');
    setHardwareError('');
    hardwareLoadingMap = true;
    updateHardwareSaveButton();
    apiGet('/api/hardware/map', function (err, data) {
        hardwareLoadingMap = false;
        if (err) {
            if (err.message === 'HTTP 401') {
                navigateTo('/login');
                return;
            }
            setHardwareState('โหลดข้อมูลล้มเหลว');
            setHardwareError('ไม่สามารถโหลดผัง GPIO ได้: ' + err.message);
            updateHardwareSaveButton();
            return;
        }
        if (!data || data.ok !== true) {
            setHardwareState('โหลดข้อมูลล้มเหลว');
            setHardwareError('เซิร์ฟเวอร์ตอบกลับผัง GPIO ไม่ถูกต้อง');
            updateHardwareSaveButton();
            return;
        }
        applyHardwareMap(data);
    });
}

function applyHardwareMap(data) {
    hardwareMapData = data;
    var effective = data.pending_valid && data.pending ? data.pending : data.active;

    renderHardwareBanner(data);
    renderHardwareSummary('hardware-active-summary', data.active, 'active');
    renderHardwareSummary('hardware-pending-summary', data.pending_valid ? data.pending : null, 'pending', data.active);
    populateHardwareSelects(data.options || {}, effective || data.active || {});
    renderHardwareWiring(data.active, data.pending_valid ? data.pending : null);
    renderLockedPins(data.board, data.locked_pins);
    applyBoardWiringFacts(data.board);

    setHardwareClean();
    setHardwareState(data.reboot_required ? 'บันทึกผังใหม่แล้ว - จำเป็นต้องรีบูตระบบ' : 'โหลดผังปัจจุบันเรียบร้อย');
}

function renderHardwareBanner(data) {
    var el = hardwareEl('hardware-reboot-banner');
    if (!el) return;
    if (!data || !data.reboot_required) {
        el.classList.add('hidden');
        el.textContent = '';
        return;
    }
    el.classList.remove('hidden');
    el.innerHTML = '<strong>จำเป็นต้องรีบูตระบบ</strong><span> บันทึกผัง GPIO ใหม่แล้ว กรุณารีบูตบอร์ด ESP32 เพื่อเริ่มใช้งานขาพินใหม่</span>';
}

function renderHardwareSummary(id, map, emptyLabel, activeMap) {
    var el = hardwareEl(id);
    if (!el) return;
    if (!map) {
        el.innerHTML = '<div class="hardware-summary-empty">ไม่มีการเปลี่ยนแปลงของผัง ' + (emptyLabel === 'active' ? 'ปัจจุบัน' : 'ใหม่') + '</div>';
        return;
    }

    var html = '';
    var count = 0;
    for (var i = 0; i < HARDWARE_FIELDS.length; i++) {
        var field = HARDWARE_FIELDS[i];
        if (activeMap && String(map[field.key]) === String(activeMap[field.key])) {
            continue;
        }
        count++;
        html += '<div class="hardware-summary-row">'
            + '<span>' + escHtml(field.label) + '</span>'
            + '<strong>GPIO ' + escHtml(String(map[field.key])) + '</strong>'
            + '</div>';
    }

    if (activeMap && count === 0) {
        el.innerHTML = '<div class="hardware-summary-empty">ไม่มีการเปลี่ยนแปลงของผัง ' + (emptyLabel === 'active' ? 'ปัจจุบัน' : 'ใหม่') + '</div>';
    } else {
        el.innerHTML = html;
    }
}

function populateHardwareSelects(options, map) {
    for (var i = 0; i < HARDWARE_FIELDS.length; i++) {
        var field = HARDWARE_FIELDS[i];
        var select = hardwareEl(field.select);
        if (!select) continue;

        var current = map && map[field.key] !== undefined ? String(map[field.key]) : '';
        var roleOptions = options[field.key] || [];
        var html = '';
        for (var j = 0; j < roleOptions.length; j++) {
            var opt = roleOptions[j];
            var value = String(opt.gpio);
            var label = opt.label || ('GPIO ' + value);
            if (opt.is_default) label += ' (ค่าเริ่มต้น)';
            html += '<option value="' + escHtml(value) + '"'
                + (value === current ? ' selected' : '')
                + '>' + escHtml(label) + '</option>';
        }
        select.innerHTML = html;
        if (current) select.value = current;
    }
}

function renderHardwareWiring(active, pending) {
    for (var i = 0; i < HARDWARE_FIELDS.length; i++) {
        var field = HARDWARE_FIELDS[i];
        var activeValue = active && active[field.key] !== undefined ? active[field.key] : '--';
        var text = 'พินปัจจุบัน GPIO ' + activeValue;
        if (pending && pending[field.key] !== undefined && String(pending[field.key]) !== String(activeValue)) {
            text += ' / รอรีบูต GPIO ' + pending[field.key];
        }
        setText(field.wire, text);
    }
}

function renderLockedPins(board, lockedPins) {
    if (!lockedPins) return;
    var map = {
        'locked-boot-btn': lockedPins.boot_btn,
        'locked-ext-btn': lockedPins.ext_btn,
        'locked-led': lockedPins.led,
        'locked-ext-led': lockedPins.ext_led,
        'locked-rgb-led': lockedPins.rgb_led,
        'locked-tft-sck': lockedPins.tft_sck,
        'locked-tft-mosi': lockedPins.tft_mosi,
        'locked-tft-cs': lockedPins.tft_cs,
        'locked-tft-dc': lockedPins.tft_dc,
        'locked-tft-rst': lockedPins.tft_rst,
        'locked-tft-led': lockedPins.tft_led
    };
    for (var id in map) {
        if (map[id] === undefined || map[id] === null) continue;
        setText(id, 'GPIO ' + map[id]);
    }
    var ledRow = document.getElementById('locked-led-row');
    var rgbRow = document.getElementById('locked-rgb-led-row');
    var hasRgb = lockedPins.rgb_led !== undefined && lockedPins.rgb_led !== null;
    var hasLed = lockedPins.led !== undefined && lockedPins.led !== null;

    if (ledRow) {
        if (hasRgb) {
            ledRow.classList.add('hidden');
        } else if (hasLed) {
            ledRow.classList.remove('hidden');
            setText('locked-led', 'GPIO ' + lockedPins.led);
        } else {
            ledRow.classList.add('hidden');
        }
    }
    if (rgbRow) {
        if (hasRgb) {
            rgbRow.classList.remove('hidden');
            setText('locked-rgb-led', 'GPIO ' + lockedPins.rgb_led);
        } else {
            rgbRow.classList.add('hidden');
        }
    }
}

function applyBoardWiringFacts(board) {
    if (!board || !board.target) return;
    var isS3 = board.target === 'esp32s3';
    var frAlt = document.getElementById('wiring-fr-alt-sentence');
    if (frAlt) frAlt.classList.toggle('hidden', isS3);
    var inputOnly = document.getElementById('wiring-input-only-bullet');
    if (inputOnly) inputOnly.classList.toggle('hidden', isS3);
}

function readHardwareMapForm() {
    var map = {};
    for (var i = 0; i < HARDWARE_FIELDS.length; i++) {
        var field = HARDWARE_FIELDS[i];
        var select = hardwareEl(field.select);
        if (!select || select.value === '') return null;
        map[field.key] = parseInt(select.value, 10);
    }
    return map;
}

function saveHardwareMap() {
    var confirm = hardwareEl('hardware-confirm-reboot');
    if (!confirm || !confirm.checked) {
        setHardwareError('กรุณาทำเครื่องหมายยืนยันว่าเข้าใจว่าต้องรีบูตระบบก่อนบันทึก');
        updateHardwareSaveButton();
        return;
    }

    var map = readHardwareMapForm();
    if (!map) {
        setHardwareError('กรุณาเลือกพิน GPIO สำหรับทุกหน้าที่การทำงาน');
        return;
    }

    hardwareSavingMap = true;
    updateHardwareSaveButton();
    setHardwareError('');
    setHardwareState('กำลังบันทึกผัง GPIO ใหม่...');

    apiPost('/api/hardware/map', {
        map: map,
        confirm_reboot_required: true
    }, function (err, data) {
        hardwareSavingMap = false;
        updateHardwareSaveButton();
        if (err) {
            setHardwareState('บันทึกข้อมูลล้มเหลว');
            setHardwareError('ไม่สามารถบันทึกผัง GPIO ได้: ' + err.message);
            return;
        }
        if (!data || data.ok !== true) {
            setHardwareState('บันทึกข้อมูลล้มเหลว');
            setHardwareError('การบันทึกถูกปฏิเสธ: ' + ((data && (data.message || data.error)) || 'ผัง GPIO ไม่ถูกต้อง'));
            return;
        }
        applyHardwareMap(data);
        showToast('บันทึกผัง GPIO รอการรีบูตเรียบร้อย', 'success');
    });
}

/* ======== Status Page ======== */

function initStatus() {
    if (window.location.pathname !== '/status') return;
    refreshFullStatus();
    statusInterval = setInterval(refreshFullStatus, 30000);
}

function clearStatusSkeletonsToOffline() {
    var ids = [
        'st-chip-model', 'st-board-name', 'st-chip-revision', 'st-chip-cores', 'st-cpu-freq',
        'st-idf-version', 'st-project-version', 'st-reset-reason',
        'st-free-heap', 'st-min-free-heap', 'st-largest-free-block', 'st-total-heap',
        'st-psram-free', 'st-psram-total',
        'st-uptime', 'st-sta-status', 'st-sta-ssid', 'st-sta-ip', 'st-sta-rssi',
        'st-sta-channel', 'st-sta-auth', 'st-mac-sta', 'st-ap-ssid', 'st-ap-ip',
        'st-ap-clients', 'st-ap-client-rssi', 'st-mac-ap', 'st-wifi-mode',
        'st-dns-status', 'st-http-static-errors', 'st-http-json-errors'
    ];
    for (var i = 0; i < ids.length; i++) {
        setText(ids[i], '--');
    }
}

function refreshFullStatus() {
    if (document.hidden) return;
    if (statusFullRequestInFlight) return;
    statusFullRequestInFlight = true;
    apiGet('/api/status', function (err, data) {
        statusFullRequestInFlight = false;
        if (err || !data || !data.ok) {
            if (err && err.message === 'HTTP 401') { navigateTo('/login'); }
            var dot = document.getElementById('sidebar-status-dot');
            var txt = document.getElementById('sidebar-status-text');
            if (dot && txt) {
                dot.className = 'status-dot off';
                txt.textContent = 'Offline';
            }
            clearStatusSkeletonsToOffline();
            return;
        }

        /* System */
        setText('st-chip-model', data.chip_model);
        setText('st-board-name', data.board_name || '--');
        setText('st-chip-revision', '' + data.chip_revision);
        setText('st-chip-cores', '' + data.chip_cores);
        setText('st-cpu-freq', data.cpu_freq_mhz + ' MHz');
        setText('st-idf-version', data.idf_version);
        setText('st-project-version', data.project_version);
        var resetText = data.reset_reason || '--';
        if (data.reset_detail) {
            resetText += ' (' + escHtml(data.reset_detail) + ')';
        }
        setText('st-reset-reason', resetText);

        /* Memory (internal DRAM) */
        var freeKb = (data.free_heap / 1024).toFixed(0);
        var minKb = (data.min_free_heap / 1024).toFixed(0);
        var largestKb = (data.largest_free_block / 1024).toFixed(0);
        var totalKb = (data.total_heap / 1024).toFixed(0);

        var pct = data.total_heap > 0 ? ((data.total_heap - data.free_heap) / data.total_heap * 100) : 0;

        setText('st-free-heap', freeKb + ' KB (' + pct.toFixed(1) + '% used)');
        setText('st-min-free-heap', minKb + ' KB');
        setText('st-largest-free-block', largestKb + ' KB');
        setText('st-total-heap', totalKb + ' KB');

        var bar = document.getElementById('st-heap-bar');
        if (bar) bar.style.width = Math.min(100, Math.max(5, pct)).toFixed(0) + '%';
        var pctEl = document.getElementById('st-heap-pct');
        if (pctEl) pctEl.textContent = 'Usage: ' + pct.toFixed(1) + '%';

        /* PSRAM (shown only when the board reports SPIRAM) */
        var psramTotal = data.psram_total || 0;
        var psramFreeRow = document.getElementById('st-psram-free-row');
        var psramTotalRow = document.getElementById('st-psram-total-row');
        if (psramTotal > 0) {
            if (psramFreeRow) psramFreeRow.style.display = '';
            if (psramTotalRow) psramTotalRow.style.display = '';
            var psramFree = data.psram_free || 0;
            var psramFreeKb = (psramFree / 1024).toFixed(0);
            var psramTotalKb = (psramTotal / 1024).toFixed(0);
            var psramPct = ((psramTotal - psramFree) / psramTotal * 100);
            setText('st-psram-free', psramFreeKb + ' KB (' + psramPct.toFixed(1) + '% used)');
            setText('st-psram-total', psramTotalKb + ' KB');
        } else {
            if (psramFreeRow) psramFreeRow.style.display = 'none';
            if (psramTotalRow) psramTotalRow.style.display = 'none';
        }

        /* Uptime */
        var secs = Math.floor(data.uptime_ms / 1000);
        var mins = Math.floor(secs / 60);
        var hrs = Math.floor(mins / 60);
        mins = mins % 60;
        var uptimeStr;
        if (hrs > 24) {
            var days = Math.floor(hrs / 24);
            hrs = hrs % 24;
            uptimeStr = days + 'd ' + hrs + 'h ' + mins + 'm';
        } else if (hrs > 0) {
            uptimeStr = hrs + 'h ' + mins + 'm';
        } else {
            uptimeStr = mins + 'm ' + (secs % 60) + 's';
        }
        setText('st-uptime', uptimeStr);

        /* STA */
        setText('st-sta-status', data.sta_connected ? 'Connected' : 'Disconnected');
        setText('st-sta-ssid', data.sta_connected ? data.sta_ssid : '--');
        setText('st-sta-ip', data.sta_connected ? data.sta_ip : '--');
        setText('st-sta-rssi', data.sta_connected && data.sta_rssi !== undefined ? data.sta_rssi + ' dBm' : '--');
        setText('st-sta-channel', data.sta_connected && data.sta_channel !== undefined ? '' + data.sta_channel : '--');
        setText('st-sta-auth', data.sta_connected && data.sta_auth ? data.sta_auth : '--');
        setText('st-mac-sta', data.mac_sta || '--');

        /* AP */
        setText('st-ap-ssid', data.ap_enabled ? data.ap_ssid : '--');
        setText('st-ap-ip', data.ap_enabled ? data.ap_ip : '--');
        setText('st-ap-clients', data.ap_enabled ? '' + data.ap_clients : '--');
        setText('st-ap-client-rssi', data.ap_enabled && data.ap_client_weakest_rssi !== undefined
            ? data.ap_client_weakest_rssi + ' dBm'
            : '--');
        setText('st-mac-ap', data.mac_ap || '--');

        /* Services */
        setText('st-wifi-mode', data.wifi_mode || '--');
        setText('st-dns-status', data.dns_server ? 'Running' : 'Stopped');
        setText('st-http-static-errors', '' + (data.http_static_send_failures || 0));
        setText('st-http-json-errors', '' + (data.http_json_send_failures || 0));
    });
}

function clearSkeleton(el) {
    if (!el) return;
    var s = el.querySelector('.skeleton');
    if (s) {
        el.innerHTML = '';
    }
}

function setText(id, text) {
    var el = (typeof pumpEl === 'function' ? pumpEl(id) : null) || document.getElementById(id);
    if (!el) return;
    clearSkeleton(el);
    var val = (text == null) ? '' : String(text);
    if (el.textContent !== val) {
        el.textContent = val;
    }
}

function setHtml(id, html) {
    var el = (typeof pumpEl === 'function' ? pumpEl(id) : null) || document.getElementById(id);
    if (!el) return;
    clearSkeleton(el);
    var val = (html == null) ? '' : String(html);
    if (el.innerHTML !== val) {
        el.innerHTML = val;
    }
}

function refreshStatus(force) {
    if (document.hidden && !force) return;
    if (statusSummaryRequestInFlight) return;
    statusSummaryRequestInFlight = true;
    apiGet('/api/status', function (err, data) {
        statusSummaryRequestInFlight = false;
        if (err || !data || !data.ok) {
            if (err && err.message === 'HTTP 401') { navigateTo('/login'); }
            var dot = document.getElementById('sidebar-status-dot');
            var txt = document.getElementById('sidebar-status-text');
            if (dot && txt) {
                dot.className = 'status-dot off';
                txt.textContent = 'Offline';
            }
            clearStatusSkeletonsToOffline();
            return;
        }

        /* Sidebar status dot */
        var dot = document.getElementById('sidebar-status-dot');
        var txt = document.getElementById('sidebar-status-text');
        if (dot && txt) {
            if (data.sta_connected) {
                dot.className = 'status-dot';
                txt.textContent = 'Online';
            } else if (data.ap_enabled) {
                dot.className = 'status-dot';
                txt.textContent = 'AP Mode';
            } else {
                dot.className = 'status-dot off';
                txt.textContent = 'Offline';
            }
        }
    });
}

/* ======== Wi-Fi Page ======== */

var selectedSsid = null;
var wifiConnectPollTimer = null;
var wifiJustConnected = false;
var wifiConnectedSsid = null;
var wifiLastPassword = '';

function initWifi() {
    if (window.location.pathname !== '/wifi') return;
    updateApPill();
    updateConnectionStatus();
    loadWifiProfiles();
}

function updateConnectionStatus() {
    apiGet('/api/status', function (err, data) {
        if (!err && data && data.ok && data.sta_connected) {
            window.appConnState = {
                connected: true,
                ssid: data.sta_ssid,
                ip: data.sta_ip
            };
        } else {
            window.appConnState = { connected: false };
        }
        // Force re-render if profiles are already loaded
        if (window.savedWifiProfiles) {
            renderSavedProfiles();
        }
    });
}

function loadWifiProfiles(retryCount) {
    if (retryCount === undefined) retryCount = 0;
    var el = document.getElementById('profile-list');
    if (!el) return;

    if (retryCount === 0) {
        el.innerHTML = '<div class="profile-empty">กำลังโหลด...</div>';
    }

    apiGet('/api/wifi/profiles', function (err, data) {
        if (err || !data || !data.ok) {
            if (retryCount < 1) {
                setTimeout(function () {
                    loadWifiProfiles(retryCount + 1);
                }, 1500);
            } else {
                el.innerHTML = '<button type="button" class="profile-empty clickable" onclick="loadWifiProfiles()"><div class="profile-empty-error-icon"><svg viewBox="0 0 24 24" width="24" height="24" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon"><circle cx="12" cy="12" r="10"></circle><line x1="12" y1="8" x2="12" y2="12"></line><line x1="12" y1="16" x2="12.01" y2="16"></line></svg></div><div class="profile-empty-error-text">ไม่สามารถดึงข้อมูลได้</div><div class="text-xs profile-empty-retry"><svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="vertical-align:middle;margin-right:4px;"><polyline points="23 4 23 10 17 10"></polyline><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"></path></svg>คลิกเพื่อลองใหม่</div></button>';
            }
            return;
        }
        var profiles = data.profiles || [];
        window.savedWifiProfiles = profiles;
        renderSavedProfiles();
    });
}

function renderSavedProfiles() {
    var el = document.getElementById('profile-list');
    if (!el) return;

    var profiles = window.savedWifiProfiles || [];
    /* Sort by connected first */
    profiles.sort(function (a, b) {
        if (a.connected && !b.connected) return -1;
        if (!a.connected && b.connected) return 1;
        return 0;
    });
    if (profiles.length === 0) {
        el.innerHTML = '<div class="profile-empty">ยังไม่มีเครือข่ายที่บันทึกไว้<br><span class="text-xs profile-empty-hint">เชื่อมต่อ Wi-Fi เพื่อบันทึก credential</span></div>';
        return;
    }

    var html = '<div class="profile-list">';
    for (var i = 0; i < profiles.length; i++) {
        var p = profiles[i];
        var isConn = p.connected === true;

        // Cross-reference with scan results
        var scanData = null;
        if (window.lastScanResults) {
            for (var k = 0; k < window.lastScanResults.length; k++) {
                if (window.lastScanResults[k].ssid === p.ssid) {
                    scanData = window.lastScanResults[k];
                    break;
                }
            }
        }

        var iconClass = isConn ? ' connected-icon' : (scanData ? '' : ' inactive-icon');
        var iconStyle = scanData ? '' : 'opacity: 0.4;';

        html += '<div class="profile-item' + (isConn ? ' connected' : '') + '">';

        var sigBars = 4;
        if (scanData) {
            if (scanData.rssi >= -50) { sigBars = 4; }
            else if (scanData.rssi >= -70) { sigBars = 3; }
            else if (scanData.rssi >= -85) { sigBars = 2; }
            else { sigBars = 1; }
        } else if (!isConn) {
            sigBars = 0;
        }

        var sigSvg = '<svg viewBox="0 0 24 24" width="20" height="20" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon">';
        if (sigBars >= 1 || isConn) sigSvg += '<line x1="12" y1="20" x2="12.01" y2="20"></line>';
        if (sigBars >= 2 || isConn) sigSvg += '<path d="M8.53 16.11a6 6 0 0 1 6.95 0"></path>';
        if (sigBars >= 3 || isConn) sigSvg += '<path d="M5 12.55a11 11 0 0 1 14.08 0"></path>';
        if (sigBars >= 4 || isConn) sigSvg += '<path d="M1.42 9a16 16 0 0 1 21.16 0"></path>';
        if (sigBars === 0 && !isConn) {
            sigSvg = '<svg viewBox="0 0 24 24" width="20" height="20" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon"><path d="M5 12.55a11 11 0 0 1 14.08 0"></path><path d="M1.42 9a16 16 0 0 1 21.16 0"></path><path d="M8.53 16.11a6 6 0 0 1 6.95 0"></path><line x1="12" y1="20" x2="12.01" y2="20"></line></svg>';
        } else {
            sigSvg += '</svg>';
        }

        /* Left side */
        html += '<div class="profile-item-left">';
        html += '<div class="network-icon' + iconClass + '" style="' + iconStyle + '">';
        html += sigSvg;
        html += '</div>';
        html += '<div class="network-info">';
        html += '<div class="net-ssid-row">';
        html += '<span class="net-ssid">' + escHtml(p.ssid) + '</span>';
        if (isConn) {
            html += '<span class="badge-current"><svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="3" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="margin-right:4px;"><polyline points="20 6 9 17 4 12"></polyline></svg> ปัจจุบัน</span>';
        }
        html += '</div>';
        html += '<div class="net-detail">';

        if (scanData) {
            // Found in scan
            var authLabel = scanData.auth !== 'Open' ? '<svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="margin-right:4px;"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 10 0v4"></path></svg>' : '<svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="margin-right:4px;"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 9.9-1"></path></svg>';
            html += authLabel + ' ' + escHtml(scanData.auth);
        } else if (isConn) {
            html += '<svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="margin-right:4px;"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 10 0v4"></path></svg> บันทึกแล้ว';
        } else if (typeof window.lastScanResults === 'undefined') {
            html += '<svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="margin-right:4px;"><circle cx="12" cy="12" r="10"></circle><line x1="12" y1="8" x2="12" y2="12"></line><line x1="12" y1="16" x2="12.01" y2="16"></line></svg> รอตรวจสอบระยะสัญญาณ';
        } else {
            // Scanned but not found
            html += '<svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="margin-right:4px;"><circle cx="12" cy="12" r="10"></circle><line x1="4.93" y1="4.93" x2="19.07" y2="19.07"></line></svg> อยู่นอกระยะสัญญาณ';
        }

        if (isConn && window.appConnState && window.appConnState.ip) {
            html += '<span class="detail-divider" style="margin:0 6px; opacity:0.5;">&bull;</span>';
            html += 'IP: <span class="mono">' + escHtml(window.appConnState.ip) + '</span>';
        }
        html += '</div>';
        html += '</div>';
        html += '</div>';

        /* Right side */
        html += '<div class="profile-actions">';

        html += '<label class="toggle-row-inline">';
        html += '<span class="toggle-label-text">เชื่อมต่ออัตโนมัติ</span>';
        html += '<div class="toggle-switch"><input type="checkbox" onchange="toggleAutoProfile(\'' + escJs(p.ssid) + '\', this.checked, ' + (p.auto ? 'true' : 'false') + ')" ' + (p.auto ? 'checked' : '') + '><span class="toggle-slider"></span></div>';
        html += '</label>';

        html += '<div class="action-divider"></div>';

        html += '<button class="btn-icon-text btn-forget" onclick="forgetProfile(\'' + escJs(p.ssid) + '\')">';
        html += '<svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path><line x1="10" y1="11" x2="10" y2="17"></line><line x1="14" y1="11" x2="14" y2="17"></line></svg>';
        html += '<span class="hidden-mobile">ลืม</span>';
        html += '</button>';

        if (isConn) {
            html += '<div class="action-divider"></div>';
            html += '<button class="btn-icon-text" onclick="doDisconnect()">';
            html += '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="svg-icon"><line x1="1" y1="1" x2="23" y2="23"></line><path d="M16.72 11.06A10.94 10.94 0 0 1 19 12.55"></path><path d="M5 12.55a10.94 10.94 0 0 1 5.17-2.39"></path><path d="M10.71 5.05A16 16 0 0 1 22.58 9"></path><path d="M1.42 9a15.91 15.91 0 0 1 4.7-2.88"></path><path d="M8.53 16.11a6 6 0 0 1 6.95 0"></path><line x1="12" y1="20" x2="12.01" y2="20"></line></svg>';
            html += '<span class="hidden-mobile">ตัดการเชื่อมต่อ</span>';
            html += '</button>';
        }

        html += '</div></div>';
    }
    html += '</div>';
    el.innerHTML = html;
}

function forgetProfile(ssid) {
    showConfirmModal('ลืมเครือข่าย', '\u0e25\u0e37\u0e21 "' + escHtml(ssid) + '" \u0e43\u0e0a\u0e48\u0e2b\u0e23\u0e37\u0e2d\u0e44\u0e21\u0e48?', function () {
        apiPost('/api/wifi/profiles/forget', { ssid: ssid }, function (err, data) {
            if (err || !data || !data.ok) {
                showToast('\u0e25\u0e37\u0e21\u0e44\u0e21\u0e48\u0e2a\u0e33\u0e40\u0e23\u0e47\u0e08', 'error');
                return;
            }
            showToast('\u0e25\u0e37\u0e21 "' + ssid + '" \u0e40\u0e23\u0e35\u0e22\u0e1a\u0e23\u0e49\u0e2d\u0e22', 'success');
            loadWifiProfiles();
        });
    }, true);
}

var toggleAutoTimer = null;
function toggleAutoProfile(ssid, isChecked, initialState) {
    if (toggleAutoTimer) {
        clearTimeout(toggleAutoTimer);
    }
    toggleAutoTimer = setTimeout(function () {
        if (isChecked === initialState) {
            return;
        }
        if (isChecked) {
            apiPost('/api/wifi/profiles/setauto', { ssid: ssid }, function (err, data) {
                if (err || !data || !data.ok) {
                    showToast('\u0e15\u0e31\u0e49\u0e07 Auto \u0e44\u0e21\u0e48\u0e2a\u0e33\u0e40\u0e23\u0e47\u0e08', 'error');
                    loadWifiProfiles(); // revert toggle
                    return;
                }
                showToast('\u0e15\u0e31\u0e49\u0e07 Auto-connect \u0e40\u0e23\u0e35\u0e22\u0e1a\u0e23\u0e49\u0e2d\u0e22', 'success');
                loadWifiProfiles();
            });
        } else {
            apiPost('/api/wifi/profiles/setauto', { index: -1 }, function (err, data) {
                if (err || !data || !data.ok) {
                    showToast('\u0e22\u0e01\u0e40\u0e25\u0e34\u0e01 Auto \u0e44\u0e21\u0e48\u0e2a\u0e33\u0e40\u0e23\u0e47\u0e08', 'error');
                    loadWifiProfiles(); // revert toggle
                    return;
                }
                showToast('\u0e22\u0e01\u0e40\u0e25\u0e34\u0e01 Auto-connect \u0e40\u0e23\u0e35\u0e22\u0e1a\u0e23\u0e49\u0e2d\u0e22', 'success');
                loadWifiProfiles();
            });
        }
    }, 1000);
}

function saveWifiProfile(ssid, password) {
    apiPost('/api/wifi/profiles/save', { ssid: ssid, password: password || '' }, function (err, data) {
        if (!err && data && data.ok) {
            loadWifiProfiles();
        }
    });
}

function doDisconnect() {
    showConfirmModal('ตัดการเชื่อมต่อ', 'คุณต้องการตัดการเชื่อมต่อ Wi-Fi ใช่หรือไม่?', function () {
        var btn = document.getElementById('disconnect-btn');
        if (btn) { setLoading(btn, true); }

        apiPost('/api/wifi/disconnect', {}, function (err, data) {
            if ((data && data.ok) || (err && !data)) {
                showToast('ตัดการเชื่อมต่อเรียบร้อย', 'success');
                clearSelection();
                setTimeout(function () {
                    if (btn) {
                        setLoading(btn, false);
                    }
                    updateConnectionStatus();
                    loadWifiProfiles();
                }, 1200);
            } else {
                if (btn) {
                    setLoading(btn, false);
                }
                showToast('ตัดการเชื่อมต่อไม่สำเร็จ', 'error');
            }
        });
    }, true);
}

function updateApPill() {
    apiGet('/api/status', function (err, data) {
        if (!err && data && data.ok) {
            var ipEl = document.getElementById('ap-pill-ip');
            var pill = document.getElementById('ap-pill');
            if (ipEl && data.ap_ip) ipEl.textContent = data.ap_ip;
            if (pill && !data.ap_enabled) pill.style.display = 'none';
        }
    });
}

function doScan() {
    var btn = document.getElementById('btn-scan');
    var list = document.getElementById('network-list');

    if (btn) { btn.disabled = true; btn.innerHTML = '<span class=\"spinner\"></span> \u0e01\u0e33\u0e25\u0e31\u0e07\u0e2a\u0e41\u0e01\u0e19...'; }
    apiGet('/api/wifi/scan', function (err, data) {
        if (btn) {
            setLoading(btn, false);
            btn.innerHTML = '<svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="2.5" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="margin-right: 4px; vertical-align: middle;"><path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l.73-.73"></path></svg> สแกนใหม่';
        }

        if (err || !data || !data.ok) {
            if (list) list.innerHTML = '<div class="network-empty error">สแกนไม่สำเร็จ</div>';
            return;
        }

        var nets = data.networks || [];
        if (nets.length === 0) {
            if (list) list.innerHTML = '<div class="network-empty">ไม่พบเครือข่าย</div>';
            window.lastScanResults = [];
            if (window.savedWifiProfiles) renderSavedProfiles();
            return;
        }

        /* Sort by connected first, then RSSI (strongest first) */
        nets.sort(function (a, b) {
            if (a.connected && !b.connected) return -1;
            if (!a.connected && b.connected) return 1;
            return b.rssi - a.rssi;
        });

        window.lastScanResults = nets;
        if (window.savedWifiProfiles) renderSavedProfiles();

        for (var j = 0; j < nets.length; j++) {
            if (nets[j].connected && selectedSsid === nets[j].ssid) {
                clearSelection();
                break;
            }
        }

        var html = '';
        var visibleCount = 0;
        for (var i = 0; i < nets.length; i++) {
            var isConnected = nets[i].connected === true;
            if (isConnected) continue; /* Hide the connected network from the discovered list */

            visibleCount++;
            var sigBars = 1;
            if (nets[i].rssi >= -50) { sigBars = 4; }
            else if (nets[i].rssi >= -70) { sigBars = 3; }
            else if (nets[i].rssi >= -85) { sigBars = 2; }

            var sigSvg = '<svg viewBox="0 0 24 24" width="20" height="20" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon">';
            sigSvg += '<line x1="12" y1="20" x2="12.01" y2="20"></line>';
            if (sigBars >= 2) sigSvg += '<path d="M8.53 16.11a6 6 0 0 1 6.95 0"></path>';
            if (sigBars >= 3) sigSvg += '<path d="M5 12.55a11 11 0 0 1 14.08 0"></path>';
            if (sigBars >= 4) sigSvg += '<path d="M1.42 9a16 16 0 0 1 21.16 0"></path>';
            sigSvg += '</svg>';

            html += '<button type=\"button\" class=\"network-item' + (selectedSsid === nets[i].ssid ? ' selected' : '') + '\" ' +
                'data-ssid=\"' + escHtml(nets[i].ssid) + '\" ' +
                'onclick=\"selectNetwork(\'' + escHtml(escJs(nets[i].ssid)) + '\')\" style="cursor:pointer;">' +
                '<div class=\"network-item-left\">' +
                '<div class=\"network-icon\">' +
                sigSvg +
                '</div>' +
                '<div class=\"network-info\">' +
                '<div class=\"net-ssid\">' + escHtml(nets[i].ssid) + '</div>' +
                '<div class=\"net-detail\">' +
                (nets[i].auth !== 'Open' ? '<svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="margin-right:4px;"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 10 0v4"></path></svg> ' : '<svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" class="svg-icon" style="margin-right:4px;"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 9.9-1"></path></svg> ') + escHtml(nets[i].auth) +
                '</div>' +
                '</div>' +
                '</div>' +
                '<div class=\"network-item-right\">' +
                '</div>' +
                '</button>';
        }

        if (visibleCount === 0) {
            html = '<div class="network-empty">\u0e44\u0e21\u0e48\u0e1e\u0e1a\u0e40\u0e04\u0e23\u0e37\u0e2d\u0e02\u0e48\u0e32\u0e22\u0e40\u0e1e\u0e34\u0e48\u0e21\u0e40\u0e15\u0e34\u0e21</div>'; /* ไม่พบเครือข่ายเพิ่มเติม */
        }

        if (list) list.innerHTML = html;
    });
}

function toggleStaticIp() {
    var checked = document.getElementById('static-ip-toggle').checked;
    var fields = document.getElementById('static-ip-fields');
    var ipInput = document.getElementById('static-ip');
    var gwInput = document.getElementById('static-gateway');
    var nmInput = document.getElementById('static-netmask');
    if (fields) {
        if (checked) {
            fields.classList.remove('hidden');
        } else {
            fields.classList.add('hidden');
        }
    }
    if (ipInput) { ipInput.disabled = !checked; ipInput.classList.remove('invalid'); if (!checked) ipInput.value = ''; }
    if (gwInput) { gwInput.disabled = !checked; gwInput.classList.remove('invalid'); if (!checked) gwInput.value = ''; }
    if (nmInput) { nmInput.disabled = !checked; nmInput.classList.remove('invalid'); if (!checked) nmInput.value = '255.255.255.0'; }
}

function toggleWifiPasswordVisibility() {
    var pwInput = document.getElementById('wifi-password');
    var showToggle = document.getElementById('show-wifi-password');
    if (pwInput && showToggle) {
        pwInput.type = showToggle.checked ? 'text' : 'password';
    }
}

function fadeSwap(toHide, toShow) {
    if (!toHide || !toShow) return;
    var reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    if (reduceMotion) {
        toHide.classList.add('hidden');
        toHide.style.opacity = '0';
        toShow.classList.remove('hidden');
        toShow.style.opacity = '1';
        return;
    }
    toHide.style.opacity = '0';
    setTimeout(function () {
        toHide.classList.add('hidden');
        toShow.style.opacity = '0';
        toShow.classList.remove('hidden');
        toShow.offsetHeight; // force reflow
        toShow.style.opacity = '1';
    }, 200);

}

function showSavedPasswordInput() {
    var pwGroup = document.getElementById('wifi-password-group');
    var savedMsg = document.getElementById('wifi-saved-msg');
    if (pwGroup) pwGroup.classList.remove('hidden');
    if (savedMsg) savedMsg.classList.add('hidden');
}

function selectNetwork(ssid) {
    var items = document.querySelectorAll('.network-item');
    for (var i = 0; i < items.length; i++) {
        if (items[i].getAttribute('data-ssid') === ssid && items[i].classList.contains('connected')) {
            return;
        }
    }

    selectedSsid = ssid;

    /* Update SSID display inside the modal */
    var display = document.getElementById('modal-wifi-ssid');
    if (display) display.textContent = ssid;

    /* Reset and enable input fields */
    var pwInput = document.getElementById('wifi-password');
    var showPwToggle = document.getElementById('show-wifi-password');
    var ipToggle = document.getElementById('static-ip-toggle');
    var connectBtn = document.getElementById('connect-btn');
    var cancelBtn = document.getElementById('cancel-btn');
    var statusEl = document.getElementById('connect-status');

    if (pwInput) pwInput.classList.remove('invalid');
    var ipInput = document.getElementById('static-ip');
    var gwInput = document.getElementById('static-gateway');
    var nmInput = document.getElementById('static-netmask');
    if (ipInput) ipInput.classList.remove('invalid');
    if (gwInput) gwInput.classList.remove('invalid');
    if (nmInput) nmInput.classList.remove('invalid');

    var isSaved = false;
    if (window.savedWifiProfiles) {
        for (var k = 0; k < window.savedWifiProfiles.length; k++) {
            if (window.savedWifiProfiles[k].ssid === ssid) {
                isSaved = true;
                break;
            }
        }
    }

    var isNetworkOpen = false;
    if (window.lastScanResults) {
        for (var i = 0; i < window.lastScanResults.length; i++) {
            if (window.lastScanResults[i].ssid === ssid) {
                if (window.lastScanResults[i].auth === 'Open') {
                    isNetworkOpen = true;
                }
                break;
            }
        }
    }

    if (pwInput) {
        pwInput.value = '';
        pwInput.type = 'password';
        pwInput.disabled = false;

        var pwGroup = document.getElementById('wifi-password-group');
        var savedMsg = document.getElementById('wifi-saved-msg');

        if (isNetworkOpen) {
            pwInput.placeholder = 'เครือข่ายเปิด (ไม่มีรหัสผ่าน)';
            pwInput.disabled = true;
            if (pwGroup) pwGroup.classList.add('hidden');
            if (savedMsg) savedMsg.classList.add('hidden');
        } else if (isSaved) {
            pwInput.placeholder = '\u0e23\u0e2b\u0e31\u0e2a\u0e1c\u0e48\u0e32\u0e19\u0e16\u0e39\u0e01\u0e1a\u0e31\u0e19\u0e17\u0e36\u0e01\u0e44\u0e27\u0e49\u0e41\u0e25\u0e49\u0e27 (\u0e40\u0e27\u0e49\u0e19\u0e27\u0e48\u0e32\u0e07\u0e44\u0e14\u0e49)';
            if (pwGroup) pwGroup.classList.add('hidden');
            if (savedMsg) savedMsg.classList.remove('hidden');
        } else {
            pwInput.placeholder = '\u0e23\u0e2b\u0e31\u0e2a\u0e1c\u0e48\u0e32\u0e19';
            if (pwGroup) pwGroup.classList.remove('hidden');
            if (savedMsg) savedMsg.classList.add('hidden');
        }
    }
    if (showPwToggle) { showPwToggle.checked = false; showPwToggle.disabled = isNetworkOpen; }
    if (ipToggle) { ipToggle.checked = false; ipToggle.disabled = false; }
    toggleStaticIp(); // clear static fields and hide them
    if (statusEl) statusEl.textContent = '';
    if (connectBtn) connectBtn.disabled = false;
    if (cancelBtn) cancelBtn.disabled = false;

    /* Show the Wi-Fi connect modal */
    var wifiModal = document.getElementById('wifi-connect-modal');
    if (wifiModal) {
        wifiModal.showModal();
    }

    /* Update step to 2 */
    updateStepper(2);

    /* Highlight in list */
    items = document.querySelectorAll('.network-item');
    for (var i = 0; i < items.length; i++) {
        if (items[i].getAttribute('data-ssid') === ssid) {
            items[i].classList.add('selected');
        } else {
            items[i].classList.remove('selected');
        }
    }
}

function clearSelection() {
    selectedSsid = null;

    /* Hide the Wi-Fi connect modal */
    var wifiModal = document.getElementById('wifi-connect-modal');
    if (wifiModal) {
        wifiModal.close();
    }

    /* Reset inputs */
    var pwInput = document.getElementById('wifi-password');
    var showPwToggle = document.getElementById('show-wifi-password');
    var ipToggle = document.getElementById('static-ip-toggle');
    var connectBtn = document.getElementById('connect-btn');
    var cancelBtn = document.getElementById('cancel-btn');
    var statusEl = document.getElementById('connect-status');

    if (pwInput) { pwInput.value = ''; pwInput.type = 'password'; pwInput.disabled = true; }
    if (showPwToggle) { showPwToggle.checked = false; showPwToggle.disabled = true; }
    if (ipToggle) { ipToggle.checked = false; ipToggle.disabled = true; }
    toggleStaticIp();
    if (statusEl) statusEl.textContent = '';
    if (connectBtn) connectBtn.disabled = true;
    if (cancelBtn) cancelBtn.disabled = true;

    /* Only reset stepper if we did NOT just successfully connect */
    if (!wifiJustConnected) {
        updateStepper(1);
    }

    /* Clear highlights */
    var items = document.querySelectorAll('.network-item');
    for (var i = 0; i < items.length; i++) {
        items[i].classList.remove('selected');
    }
}

/* Update network list in-place to mark the newly connected SSID */
function markNetworkConnected(ssid) {
    var items = document.querySelectorAll('.network-item');
    for (var i = 0; i < items.length; i++) {
        var itemSsid = items[i].getAttribute('data-ssid');
        if (itemSsid === ssid) {
            /* Switch to connected state */
            items[i].classList.add('connected');
            items[i].classList.remove('selected');
            items[i].disabled = true;
            items[i].removeAttribute('onclick');
            /* Replace the เลือก button with ✓ เชื่อมต่ออยู่ label */
            var right = items[i].querySelector('.network-item-right');
            if (right) {
                var sigHtml = right.querySelector('.net-signal') ? right.querySelector('.net-signal').outerHTML : '';
                right.innerHTML = sigHtml + '<span class="network-connected-label">&#10003; \u0e40\u0e0a\u0e37\u0e48\u0e2d\u0e21\u0e15\u0e48\u0e2d\u0e2d\u0e22\u0e39\u0e48</span>';
            }
            /* Green icon */
            var icon = items[i].querySelector('.network-icon');
            if (icon) icon.style.color = 'var(--secondary)';
        } else {
            /* Remove connected class from any previously-connected item */
            items[i].classList.remove('connected');
        }
    }
}



function setWifiModalInputsDisabled(disabled) {
    var modalInputs = document.querySelectorAll('#wifi-connect-modal input, #wifi-connect-modal select, #wifi-connect-modal button');
    for (var i = 0; i < modalInputs.length; i++) {
        modalInputs[i].disabled = disabled;
    }
}

function doConnect() {
    if (!selectedSsid) return;

    var pwInput = document.getElementById('wifi-password');
    var password = pwInput ? pwInput.value : '';

    var connectBtn = document.getElementById('connect-btn');
    var cancelBtn = document.getElementById('cancel-btn');
    var statusEl = document.getElementById('connect-status');

    if (pwInput) pwInput.classList.remove('invalid');
    var ipInput = document.getElementById('static-ip');
    var gwInput = document.getElementById('static-gateway');
    var nmInput = document.getElementById('static-netmask');
    if (ipInput) ipInput.classList.remove('invalid');
    if (gwInput) gwInput.classList.remove('invalid');
    if (nmInput) nmInput.classList.remove('invalid');

    /* 1. Password length validation for secured unsaved networks */
    var isSaved = false;
    if (window.savedWifiProfiles) {
        for (var k = 0; k < window.savedWifiProfiles.length; k++) {
            if (window.savedWifiProfiles[k].ssid === selectedSsid) {
                isSaved = true;
                break;
            }
        }
    }

    var isNetworkOpen = false;
    if (window.lastScanResults) {
        for (var i = 0; i < window.lastScanResults.length; i++) {
            if (window.lastScanResults[i].ssid === selectedSsid) {
                if (window.lastScanResults[i].auth === 'Open') {
                    isNetworkOpen = true;
                }
                break;
            }
        }
    }

    if (!isNetworkOpen && !isSaved) {
        if (!password || password.length < 8) {
            if (pwInput) pwInput.classList.add('invalid');
            if (statusEl) {
                statusEl.textContent = 'รหัสผ่านสำหรับเครือข่ายนี้ต้องมีความยาวอย่างน้อย 8 ตัวอักษร';
                statusEl.style.color = 'var(--error)';
            }
            return;
        }
    }

    var body = { ssid: selectedSsid, password: password };
    wifiLastPassword = password;  /* capture for saveWifiProfile on success */

    /* 2. IP format validation */
    if (document.getElementById('static-ip-toggle').checked) {
        var ipVal = ipInput ? ipInput.value.trim() : '';
        var gwVal = gwInput ? gwInput.value.trim() : '';
        var nmVal = nmInput ? nmInput.value.trim() : '';

        if (!isValidIp(ipVal)) {
            if (ipInput) ipInput.classList.add('invalid');
            if (statusEl) {
                statusEl.textContent = 'กรุณากรอก IP Address ให้ถูกต้อง (เช่น 192.168.1.100)';
                statusEl.style.color = 'var(--error)';
            }
            return;
        }
        if (!isValidIp(gwVal)) {
            if (gwInput) gwInput.classList.add('invalid');
            if (statusEl) {
                statusEl.textContent = 'กรุณากรอก Gateway ให้ถูกต้อง (เช่น 192.168.1.1)';
                statusEl.style.color = 'var(--error)';
            }
            return;
        }
        if (!isValidIp(nmVal)) {
            if (nmInput) nmInput.classList.add('invalid');
            if (statusEl) {
                statusEl.textContent = 'กรุณากรอก Subnet Netmask ให้ถูกต้อง (เช่น 255.255.255.0)';
                statusEl.style.color = 'var(--error)';
            }
            return;
        }

        body.ip = ipVal;
        body.gateway = gwVal;
        body.netmask = nmVal;
    }

    if (connectBtn) setLoading(connectBtn, true);
    if (cancelBtn) cancelBtn.disabled = true;
    setWifiModalInputsDisabled(true);
    if (statusEl) { statusEl.textContent = ''; statusEl.style.color = 'var(--on-surface-variant)'; }

    updateStepper(3);

    apiPost('/api/wifi/connect', body, function (err, data) {
        if (err) {
            if (err.message && err.message.indexOf('HTTP') === 0) {
                if (connectBtn) setLoading(connectBtn, false);
                if (cancelBtn) cancelBtn.disabled = false;
                setWifiModalInputsDisabled(false);
                toggleStaticIp();
                if (statusEl) { statusEl.textContent = '\u0e40\u0e01\u0e34\u0e14\u0e02\u0e49\u0e2d\u0e1c\u0e34\u0e14\u0e1e\u0e25\u0e32\u0e14: ' + err.message; statusEl.style.color = 'var(--error)'; }
                updateStepper(2);
            } else {
                /* Network error means the ESP32 likely dropped the Wi-Fi to switch networks! */
                if (statusEl) { statusEl.textContent = '\u0e01\u0e33\u0e25\u0e31\u0e07\u0e23\u0e2d\u0e1c\u0e25\u0e01\u0e32\u0e23\u0e40\u0e0a\u0e37\u0e48\u0e2d\u0e21\u0e15\u0e48\u0e2d...'; statusEl.style.color = 'var(--on-surface-variant)'; }
                var banner = document.getElementById('reconnect-banner');
                if (banner) banner.classList.remove('hidden');
                if (wifiConnectPollTimer) clearTimeout(wifiConnectPollTimer);
                wifiConnectPollTimer = setTimeout(function () { pollWifiConnection(1); }, 2000);
            }
            return;
        }

        if (data && data.ok && data.connecting) {
            if (statusEl) { statusEl.textContent = '\u0e01\u0e33\u0e25\u0e31\u0e07\u0e23\u0e2d\u0e1c\u0e25\u0e01\u0e32\u0e23\u0e40\u0e0a\u0e37\u0e48\u0e2d\u0e21\u0e15\u0e48\u0e2d...'; statusEl.style.color = 'var(--on-surface-variant)'; }

            var banner = document.getElementById('reconnect-banner');
            if (banner) banner.classList.remove('hidden');

            if (wifiConnectPollTimer) clearTimeout(wifiConnectPollTimer);
            wifiConnectPollTimer = setTimeout(function () { pollWifiConnection(1); }, 1000);
        } else if (data && data.ok) {
            if (connectBtn) setLoading(connectBtn, false);
            if (cancelBtn) cancelBtn.disabled = false;
            setWifiModalInputsDisabled(false);
            toggleStaticIp();
            if (statusEl) { statusEl.textContent = '\u0e40\u0e0a\u0e37\u0e48\u0e2d\u0e21\u0e15\u0e48\u0e2d\u0e2a\u0e33\u0e40\u0e23\u0e47\u0e08! IP: ' + (data.ip || 'N/A'); statusEl.style.color = 'var(--secondary)'; }
            /* Mark step 3 as done */
            updateStepper(3, true);
            /* Update AP pill */
            updateApPill();
            /* Auto close modal after 2 seconds */
            setTimeout(function () {
                clearSelection();
            }, 2000);
        } else {
            if (connectBtn) setLoading(connectBtn, false);
            if (cancelBtn) cancelBtn.disabled = false;
            setWifiModalInputsDisabled(false);
            toggleStaticIp();
            if (statusEl) {
                statusEl.textContent = '\u0e40\u0e0a\u0e37\u0e48\u0e2d\u0e21\u0e15\u0e48\u0e2d\u0e44\u0e21\u0e48\u0e2a\u0e33\u0e40\u0e23\u0e47\u0e08: ' + (data && data.error ? data.error : '\u0e25\u0e2d\u0e07\u0e2d\u0e35\u0e01\u0e04\u0e23\u0e31\u0e49\u0e07');
                statusEl.style.color = 'var(--error)';
            }
            updateStepper(2);
        }
    });
}

function pollWifiConnection(attempt) {
    apiGet('/api/status', function (err, data) {
        var connectBtn = document.getElementById('connect-btn');
        var cancelBtn = document.getElementById('cancel-btn');
        var statusEl = document.getElementById('connect-status');

        if (!err && data && data.ok) {
            if (data.sta_connected) {
                wifiConnectPollTimer = null;
                if (connectBtn) setLoading(connectBtn, false);
                if (cancelBtn) cancelBtn.disabled = false;
                if (statusEl) { statusEl.textContent = 'เชื่อมต่อสำเร็จ! IP: ' + (data.sta_ip || 'N/A'); statusEl.style.color = 'var(--secondary)'; }

                var banner = document.getElementById('reconnect-banner');
                if (banner) banner.classList.add('hidden');

                updateStepper(3, true);
                updateConnectionStatus();
                updateApPill();

                /* Mark the connected network in the list */
                var connSsid = data.sta_ssid || selectedSsid;

                if (connSsid && connSsid !== selectedSsid) {
                    /* Fallback detected! We are back on the old Wi-Fi */
                    wifiConnectPollTimer = null;
                    if (connectBtn) setLoading(connectBtn, false);
                    if (cancelBtn) cancelBtn.disabled = false;
                    setWifiModalInputsDisabled(false);
                    toggleStaticIp();
                    if (statusEl) {
                        statusEl.textContent = 'เชื่อมต่อไม่สำเร็จ: ระบบได้ดึงคุณกลับมายัง Wi-Fi เดิม';
                        statusEl.style.color = 'var(--error)';
                    }
                    updateStepper(2);
                    var banner = document.getElementById('reconnect-banner');
                    if (banner) banner.classList.add('hidden');
                    return;
                }

                markNetworkConnected(connSsid);

                /* Persist the credential to NVS (only on confirmed connect success) */
                saveWifiProfile(connSsid, wifiLastPassword || '');

                /* Keep stepper green after modal closes */
                wifiJustConnected = true;
                wifiConnectedSsid = connSsid;

                /* Auto close modal after 2 seconds, keep stepper at step 3 done */
                setTimeout(function () {
                    clearSelection();
                }, 2000);
                return;
            }

            if (data.sta_retry_blocked) {
                wifiConnectPollTimer = null;
                if (connectBtn) setLoading(connectBtn, false);
                if (cancelBtn) cancelBtn.disabled = false;
                setWifiModalInputsDisabled(false);
                toggleStaticIp();
                if (statusEl) {
                    statusEl.innerHTML = '<svg class="label-icon"><use href="#icon-x-circle"></use></svg> การเชื่อมต่อล้มเหลว: กรุณาตรวจสอบรหัสผ่านหรือความแรงสัญญาณ';
                    statusEl.style.color = 'var(--error)';
                }
                var banner = document.getElementById('reconnect-banner');
                if (banner) banner.classList.add('hidden');
                updateStepper(2);
                return;
            }
        }

        if (attempt >= 15) {
            wifiConnectPollTimer = null;
            if (connectBtn) setLoading(connectBtn, false);
            if (cancelBtn) cancelBtn.disabled = false;
            setWifiModalInputsDisabled(false);
            toggleStaticIp();

            var banner = document.getElementById('reconnect-banner');
            if (banner) banner.classList.add('hidden');

            if (!err && data && data.ok && !data.sta_connected) {
                /* We are still connected to SoftAP, and connection to STA failed/timed out */
                if (statusEl) {
                    statusEl.innerHTML = '<svg class="label-icon"><use href="#icon-x-circle"></use></svg> เชื่อมต่อไม่สำเร็จ: หมดเวลาการเชื่อมต่อ';
                    statusEl.style.color = 'var(--error)';
                }
                updateStepper(2);
            } else {
                /* Connection to SoftAP lost, assume device moved to new network */
                if (statusEl) {
                    statusEl.innerHTML = '<svg class="label-icon"><use href="#icon-check-circle"></use></svg> อุปกรณ์ย้ายไปยังเครือข่ายใหม่แล้ว กรุณาเปลี่ยน Wi-Fi บนมือถือเพื่อใช้งานต่อ';
                    statusEl.style.color = 'var(--secondary)';
                }
                updateStepper(3, true);
                updateConnectionStatus();
            }
            return;
        }

        var countdownEl = document.getElementById('reconnect-countdown');
        if (countdownEl) countdownEl.textContent = (15 - attempt);

        wifiConnectPollTimer = setTimeout(function () { pollWifiConnection(attempt + 1); }, 200);
    }, 800);
}

function updateStepper(activeStep, done) {
    // Stepper UI removed in v1.2
}
/* ======== Utilities ======== */

function escHtml(str) {
    if (!str) return '';
    return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function escJs(str) {
    if (!str) return '';
    return str.replace(/\\/g, '\\\\').replace(/'/g, "\\'").replace(/"/g, '\\"');
}

function isValidIp(ip) {
    if (!ip) return false;
    var parts = ip.split('.');
    if (parts.length !== 4) return false;
    for (var i = 0; i < 4; i++) {
        var part = parts[i];
        if (!/^\d+$/.test(part)) return false;
        var num = parseInt(part, 10);
        if (num < 0 || num > 255) return false;
        if (part.length > 1 && part[0] === '0') return false;
    }
    return true;
}



/* ======== Change Password ======== */

function openPasswordModal() {
    var modal = document.getElementById('password-modal');
    if (modal) {
        document.getElementById('current-password').value = '';
        document.getElementById('new-username').value = '';
        document.getElementById('new-password').value = '';

        var currentInput = document.getElementById('current-password');
        var newInput = document.getElementById('new-password');
        if (currentInput) currentInput.type = 'password';
        if (newInput) newInput.type = 'password';

        var eyes = modal.querySelectorAll('.pwd-eye-btn use');
        for (var i = 0; i < eyes.length; i++) {
            eyes[i].setAttribute('href', '#icon-eye');
        }

        var errEl = document.getElementById('password-error');
        if (errEl) {
            errEl.classList.add('hidden');
            errEl.textContent = '';
        }
        modal.showModal();
    }
}

function togglePwdVisibility(inputId, btnEl) {
    var input = document.getElementById(inputId);
    if (!input || !btnEl) return;
    var iconUse = btnEl.querySelector('use');
    if (input.type === 'password') {
        input.type = 'text';
        if (iconUse) iconUse.setAttribute('href', '#icon-eye-off');
    } else {
        input.type = 'password';
        if (iconUse) iconUse.setAttribute('href', '#icon-eye');
    }
}

function closePasswordModal() {
    var modal = document.getElementById('password-modal');
    if (modal) {
        modal.close();
    }
}

function doChangePassword(e) {
    if (e) e.preventDefault();
    var currentPassword = document.getElementById('current-password').value;
    var newUsername = document.getElementById('new-username').value.trim();
    var newPassword = document.getElementById('new-password').value;
    var errEl = document.getElementById('password-error');
    var btn = document.getElementById('password-save-btn');

    var modalInputs = document.querySelectorAll('#password-modal input, #password-modal button');
    function setModalInputsDisabled(disabled) {
        for (var i = 0; i < modalInputs.length; i++) {
            modalInputs[i].disabled = disabled;
        }
    }

    if (newPassword === DEFAULT_PASSWORD) {
        if (errEl) {
            errEl.classList.remove('hidden');
            errEl.textContent = 'ไม่สามารถตั้งรหัสผ่านเริ่มต้นได้ หากต้องการรีเซ็ตให้ใช้ปุ่ม Boot กดค้างไว้ 5+ วินาที';
        }
        return;
    }

    if (errEl) errEl.classList.add('hidden');
    setLoading(btn, true);
    setModalInputsDisabled(true);

    apiGet('/api/auth/nonce', function (err, data) {
        if (err || !data || !data.ok || !data.nonce) {
            setLoading(btn, false);
            setModalInputsDisabled(false);
            if (errEl) {
                errEl.classList.remove('hidden');
                errEl.textContent = 'ไม่สามารถดึงข้อมูล nonce ได้ กรุณาลองใหม่';
            }
            return;
        }

        var payload = {
            nonce: data.nonce,
            current_password: currentPassword,
            new_password: newPassword
        };
        if (newUsername) {
            payload.new_username = newUsername;
        }

        apiPost('/api/auth/credentials', payload, function (err2, data2) {
            setLoading(btn, false);
            if (err2 || !data2 || !data2.ok) {
                setModalInputsDisabled(false);
                if (errEl) {
                    errEl.classList.remove('hidden');
                    if (data2 && data2.error === 'invalid_credentials') {
                        errEl.textContent = 'รหัสผ่านเดิมไม่ถูกต้อง';
                    } else if (data2 && data2.error === 'cannot_use_default') {
                        errEl.textContent = 'ไม่สามารถตั้งรหัสผ่านเริ่มต้นได้ หากต้องการรีเซ็ตให้ใช้ปุ่ม Boot กดค้างไว้ 5+ วินาที';
                    } else {
                        errEl.textContent = 'เปลี่ยนรหัสผ่านไม่สำเร็จ กรุณาลองใหม่';
                    }
                }
                return;
            }

            closePasswordModal();
            showToast('เปลี่ยนรหัสผ่านเรียบร้อย ระบบจะนำคุณออกเพื่อล็อกอินใหม่', 'success');
            setTimeout(function () {
                document.cookie = 'session=; Path=/; Max-Age=0';
                document.cookie = 'logged_in=; Path=/; Max-Age=0';
                navigateTo('/login');
            }, 2500);
        });
    });
}

/* ======== SPA Routing ======== */

function navigateTo(path) {
    if (window.location.pathname === path) return;
    history.pushState(null, '', path);
    handleRoute();
}

function handleRoute() {
    var path = window.location.pathname;

    if (path === '/' || path === '') {
        path = isAuthenticated() ? '/dashboard' : '/login';
        history.replaceState(null, '', path);
    }

    if (path === '/login' && isAuthenticated()) {
        navigateTo('/dashboard');
        return;
    }

    var views = document.querySelectorAll('.view');
    for (var i = 0; i < views.length; i++) {
        views[i].classList.add('hidden');
        views[i].setAttribute('aria-hidden', 'true');
    }

    var viewId = 'view-login';
    if (path === '/dashboard') viewId = 'view-dashboard';
    else if (path === '/cooling') viewId = 'view-cooling';
    else if (path === '/status') viewId = 'view-status';
    else if (path === '/wifi') viewId = 'view-wifi';
    else if (path === '/hardware') viewId = 'view-hardware';
    else if (path === '/ota') viewId = 'view-ota';

    var activeView = document.getElementById(viewId);
    if (activeView) {
        activeView.classList.remove('hidden');
        activeView.setAttribute('aria-hidden', 'false');
    }

    if (viewId === 'view-login') {
        document.body.classList.add('login-active');
    } else {
        document.body.classList.remove('login-active');
    }

    var navItems = document.querySelectorAll('.sidebar-nav .nav-item');
    for (var j = 0; j < navItems.length; j++) {
        navItems[j].classList.remove('active');
        if (navItems[j].getAttribute('href') === path) {
            navItems[j].classList.add('active');
        }
    }

    var mNavItems = document.querySelectorAll('.mobile-bottom-nav .mobile-nav-item, .mobile-more-menu .mobile-menu-item');
    var isMoreMenuActive = false;
    for (var k = 0; k < mNavItems.length; k++) {
        mNavItems[k].classList.remove('active');
        if (mNavItems[k].getAttribute('href') === path) {
            mNavItems[k].classList.add('active');
            if (mNavItems[k].classList.contains('mobile-menu-item')) {
                isMoreMenuActive = true;
            }
        }
    }

    var mNavMoreBtn = document.getElementById('m-nav-more');
    if (mNavMoreBtn) {
        if (isMoreMenuActive) {
            mNavMoreBtn.classList.add('active');
        } else {
            mNavMoreBtn.classList.remove('active');
        }
    }

    checkAuth();

    cleanupCurrentView();

    if (path === '/login') {
        if (typeof initLogin === 'function') initLogin();
    } else if (path === '/dashboard') {
        if (typeof initDashboard === 'function') initDashboard();
    } else if (path === '/cooling') {
        if (typeof initCoolingDashboard === 'function') initCoolingDashboard();
    } else if (path === '/status') {
        if (typeof initStatus === 'function') initStatus();
    } else if (path === '/wifi') {
        if (typeof initWifi === 'function') initWifi();
    } else if (path === '/hardware') {
        if (typeof initHardwareInstall === 'function') initHardwareInstall();
    } else if (path === '/ota') {
        if (typeof initOta === 'function') initOta();
    }

    window.scrollTo(0, 0);
}

window.addEventListener('popstate', handleRoute);

/* ======== Init ======== */

(function () {

    // Mobile More Menu Toggle Logic
    document.addEventListener('click', function (e) {
        var moreBtn = document.getElementById('m-nav-more');
        var moreMenu = document.getElementById('mobile-more-menu');
        if (!moreBtn || !moreMenu) return;

        var isClickInsideBtn = moreBtn.contains(e.target);
        var isClickInsideMenu = moreMenu.contains(e.target);
        var isLink = e.target.closest('a');

        if (isClickInsideBtn) {
            moreMenu.classList.toggle('hidden');
        } else if (!isClickInsideMenu || (isClickInsideMenu && isLink)) {
            // Close if clicking outside, or if clicking a link inside the menu
            moreMenu.classList.add('hidden');
        }
    });

    document.addEventListener('click', function (e) {
        var link = e.target.closest('a');
        if (link) {
            var href = link.getAttribute('href');
            if (href && href.startsWith('/') && href !== '#') {
                e.preventDefault();
                navigateTo(href);
            }
        }
    });

    // Register cancel event listeners for native dialogs to trigger correct cleanups
    var confirmModal = document.getElementById('modal-container');
    if (confirmModal) {
        confirmModal.addEventListener('cancel', function (e) {
            e.preventDefault();
            var cancelBtn = document.getElementById('modal-cancel');
            if (cancelBtn) cancelBtn.click();
        });
    }
    var passwordModal = document.getElementById('password-modal');
    if (passwordModal) {
        passwordModal.addEventListener('cancel', function (e) {
            e.preventDefault();
            closePasswordModal();
        });
    }
    var wifiModal = document.getElementById('wifi-connect-modal');
    if (wifiModal) {
        wifiModal.addEventListener('cancel', function (e) {
            e.preventDefault();
            clearSelection();
        });
    }

    // Setup accessibility for collapsible settings details/summary
    var collapsibles = document.querySelectorAll('details.settings-collapsible');
    for (var i = 0; i < collapsibles.length; i++) {
        (function (details) {
            var summary = details.querySelector('summary');
            if (summary) {
                summary.setAttribute('aria-expanded', details.open ? 'true' : 'false');
                details.addEventListener('toggle', function () {
                    summary.setAttribute('aria-expanded', details.open ? 'true' : 'false');
                });
            }
        })(collapsibles[i]);
    }

    initSettingsTabs();

    updateThemeIcons(getEffectiveTheme());
    handleRoute();
})();

function confirmStaging() {
    apiPost('/api/confirm', {}, function (err, data) {
        if (err) {
            showToast('ยืนยันไม่สำเร็จ: ' + err.message, 'error');
        } else if (data && data.ok) {
            showToast('ยืนยันการตั้งค่าสำเร็จ', 'success');
            setTimeout(function () { window.location.reload(); }, 1500);
        } else {
            showToast('ยืนยันไม่สำเร็จ: ' + (data ? data.error : 'Unknown error'), 'error');
        }
    });
}

function cancelStaging() {
    apiDelete('/api/confirm', function (err, data) {
        if (err) {
            showToast('ยกเลิกไม่สำเร็จ: ' + err.message, 'error');
        } else if (data && data.ok) {
            showToast('ยกเลิกการตั้งค่าเรียบร้อยแล้ว อุปกรณ์กำลังรีบูตเพื่อย้อนกลับ...', 'success');
            setTimeout(function () { window.location.reload(); }, 1500);
        } else {
            showToast('ยกเลิกไม่สำเร็จ: ' + (data ? data.error : 'Unknown error'), 'error');
        }
    });
}

/* === Theme Management === */
function getEffectiveTheme() {
    var saved = localStorage.getItem('theme');
    if (saved === 'dark' || saved === 'light') return saved;
    return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
}
function cycleTheme() {
    var next = getEffectiveTheme() === 'dark' ? 'light' : 'dark';
    localStorage.setItem('theme', next);
    document.documentElement.setAttribute('data-theme', next);
    updateThemeIcons(next);
}
function updateThemeIcons(theme) {
    var btns = document.querySelectorAll('.theme-toggle use');
    var icon = theme === 'dark' ? '#icon-sun' : '#icon-moon';
    for (var i = 0; i < btns.length; i++) {
        btns[i].setAttribute('href', icon);
    }
}
window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function (e) {
    if (!localStorage.getItem('theme')) {
        var t = e.matches ? 'dark' : 'light';
        document.documentElement.setAttribute('data-theme', t);
        updateThemeIcons(t);
    }
});

function showForcePasswordModal() {
    var modal = document.getElementById('force-pwd-modal');
    if (modal && !modal.open) {
        var curr = document.getElementById('force-pwd-current');
        var newp = document.getElementById('force-pwd-new');
        var conf = document.getElementById('force-pwd-confirm');

        if (curr) { curr.value = ''; curr.type = 'password'; }
        if (newp) { newp.value = ''; newp.type = 'password'; }
        if (conf) { conf.value = ''; conf.type = 'password'; }

        var eyes = modal.querySelectorAll('.pwd-eye-btn use');
        for (var i = 0; i < eyes.length; i++) {
            eyes[i].setAttribute('href', '#icon-eye');
        }

        var errEl = document.getElementById('force-pwd-error');
        if (errEl) {
            errEl.classList.add('hidden');
            errEl.textContent = '';
        }

        modal.showModal();
        // Prevent canceling via Escape key
        modal.addEventListener('cancel', function (e) {
            e.preventDefault();
        });
    }
}

function submitForcePasswordChange() {
    var curr = document.getElementById('force-pwd-current').value;
    var newp = document.getElementById('force-pwd-new').value;
    var conf = document.getElementById('force-pwd-confirm').value;
    var errEl = document.getElementById('force-pwd-error');
    var btn = document.getElementById('force-pwd-submit');

    if (newp !== conf) {
        errEl.textContent = "รหัสผ่านใหม่ไม่ตรงกัน";
        errEl.classList.remove('hidden');
        return;
    }
    if (newp.length < 8) {
        errEl.textContent = "รหัสผ่านต้องยาวอย่างน้อย 8 ตัวอักษร";
        errEl.classList.remove('hidden');
        return;
    }
    if (newp === DEFAULT_PASSWORD) {
        errEl.textContent = "ไม่สามารถตั้งรหัสผ่านเริ่มต้นได้ หากต้องการรีเซ็ตให้ใช้ปุ่ม Boot กดค้างไว้ 5+ วินาที";
        errEl.classList.remove('hidden');
        return;
    }

    errEl.classList.add('hidden');
    var origText = btn.textContent;
    btn.disabled = true;
    btn.textContent = "กำลังบันทึก...";

    apiPost('/api/change-password', { current_password: curr, new_password: newp }, function (err, data) {
        btn.disabled = false;
        btn.textContent = origText;
        if (err || !data.ok) {
            var msg = 'เกิดข้อผิดพลาดในการเปลี่ยนรหัสผ่าน';
            if (data) {
                if (data.error === 'invalid_credentials') msg = 'รหัสผ่านปัจจุบันไม่ถูกต้อง';
                else if (data.error === 'cannot_use_default') msg = 'ไม่สามารถตั้งรหัสผ่านเริ่มต้นได้ หากต้องการรีเซ็ตให้ใช้ปุ่ม Boot กดค้างไว้ 5+ วินาที';
            }
            errEl.textContent = msg;
            errEl.classList.remove('hidden');
        } else {
            var modal = document.getElementById('force-pwd-modal');
            modal.close();
            // Refresh current view to load data that was blocked
            handleRoute();
        }
    });
}

/* ======== OTA Firmware Update ======== */
function initOta() {
    if (window.location.pathname !== '/ota') return;
    apiGet('/api/status', function (err, data) {
        var versionEl = document.getElementById('ota-current-version');
        if (versionEl) {
            if (err || !data || !data.ok) {
                versionEl.textContent = 'ไม่สามารถดึงข้อมูลได้';
            } else {
                versionEl.textContent = data.project_version || 'ไม่ทราบเวอร์ชัน';
            }
        }
    });

    var fileInput = document.getElementById('ota-file-input');
    var uploadBtn = document.getElementById('ota-upload-btn');
    var progressContainer = document.getElementById('ota-progress-container');
    var progressBar = document.getElementById('ota-progress-bar');
    var progressText = document.getElementById('ota-progress-text');
    var msgEl = document.getElementById('ota-message');
    var progressAria = document.getElementById('ota-progress-aria');

    if (fileInput && uploadBtn) {
        fileInput.onchange = function () {
            uploadBtn.disabled = !fileInput.files || fileInput.files.length === 0;
            msgEl.classList.add('hidden');
            if (fileInput.files && fileInput.files.length > 0) {
                var label = fileInput.parentElement.querySelector('span');
                if (label) label.textContent = fileInput.files[0].name;
            } else {
                var label = fileInput.parentElement.querySelector('span');
                if (label) label.textContent = 'คลิกเพื่อเลือกไฟล์ หรือลากไฟล์มาวางที่นี่';
            }
        };

        uploadBtn.onclick = function () {
            if (!fileInput.files || fileInput.files.length === 0) return;
            var file = fileInput.files[0];
            if (file.size > 0x1F0000) {
                msgEl.textContent = 'ไฟล์มีขนาดใหญ่เกินไป (สูงสุด ~2MB)';
                msgEl.className = 'field-error ota-message-base ota-message-error';
                msgEl.classList.remove('hidden');
                return;
            }

            uploadBtn.disabled = true;
            fileInput.disabled = true;
            progressContainer.classList.remove('hidden');
            progressBar.style.transform = 'scaleX(0)';
            progressText.textContent = 'กำลังอัปโหลด: 0%';
            msgEl.classList.add('hidden');

            var xhr = new XMLHttpRequest();
            xhr.open('POST', '/api/ota', true);

            xhr.upload.onprogress = function (e) {
                if (e.lengthComputable) {
                    var percent = Math.round((e.loaded / e.total) * 100);
                    progressBar.style.transform = 'scaleX(' + (percent / 100) + ')';
                    progressText.textContent = 'กำลังอัปโหลด: ' + percent + '%';
                    if (progressAria) progressAria.setAttribute('aria-valuenow', percent);
                }
            };

            xhr.onload = function () {
                if (xhr.status === 200) {
                    try {
                        var res = JSON.parse(xhr.responseText);
                        if (res.ok) {
                            progressText.textContent = 'อัปเดตสำเร็จ! ระบบกำลังรีสตาร์ท...';
                            progressBar.style.transform = 'scaleX(1)';
                            if (progressAria) progressAria.setAttribute('aria-valuenow', 100);
                            msgEl.textContent = 'อัปเดตสำเร็จ! ระบบจะรีบูตในไม่ช้า กรุณารอสักครู่แล้วเชื่อมต่อใหม่';
                            msgEl.className = 'field-error ota-message-base ota-message-success';
                            msgEl.classList.remove('hidden');
                            setTimeout(function () {
                                window.location.href = '/login';
                            }, 5000);
                            return;
                        }
                    } catch (e) { }
                }

                // Error case
                uploadBtn.disabled = false;
                fileInput.disabled = false;

                if (xhr.status === 401) {
                    msgEl.textContent = 'เซสชันหมดอายุ กรุณาเข้าสู่ระบบใหม่...';
                    setTimeout(function () {
                        window.location.href = '/login';
                    }, 2000);
                } else if (xhr.status === 413) {
                    msgEl.textContent = 'ไฟล์มีขนาดใหญ่เกินไป (HTTP 413)';
                } else {
                    msgEl.textContent = 'เกิดข้อผิดพลาดในการอัปเดต (HTTP ' + xhr.status + ')';
                }
                msgEl.className = 'field-error ota-message-base ota-message-error';
                msgEl.classList.remove('hidden');
            };

            xhr.onerror = function () {
                uploadBtn.disabled = false;
                fileInput.disabled = false;
                msgEl.textContent = 'เกิดข้อผิดพลาดในการเชื่อมต่อเครือข่าย';
                msgEl.className = 'field-error ota-message-base ota-message-error';
                msgEl.classList.remove('hidden');
            };

            xhr.send(file);
        };
    }
}

// A9: Wake screen — POST to pump status endpoint to trigger
//     tft_display_reset_idle_timer() on firmware side.
//     Does NOT reset via GET /api/status to avoid auto-polling keeping screen on forever.
function wakeScreen() {
    apiPost('/api/display/wake', {}, function (err, data) {
        var btn = document.getElementById('btn-wake-screen');
        if (btn) {
            btn.innerHTML = '<svg class="label-icon"><use href="#icon-check-circle"></use></svg> หน้าจอเปิดแล้ว';
            setTimeout(function () { btn.innerHTML = '<svg class="label-icon"><use href="#icon-sun"></use></svg> เปิดหน้าจอทันที'; }, 2000);
        }
    });
}

var currentSavedDim = null;
var dimPreviewTimeout = null;

// Load dim setting from API
function loadDimSetting() {
    apiGet('/api/display/config', function (err, data) {
        if (!err && data && data.ok) {
            var pct = data.dim_percent;
            currentSavedDim = (data.nvs_dim_percent !== undefined) ? data.nvs_dim_percent : pct;

            var slider = document.getElementById('input-dim-percent');
            if (slider) slider.value = pct;

            updateDimLabel(pct, true);
        }
    });
}

// Live label update while dragging
function updateDimLabel(val, skipPreview) {
    var label = document.getElementById('dim-percent-label');
    var valInt = parseInt(val, 10);

    if (label) {
        var displayStr = valInt == 0 ? '0% (ปิดหน้าจอ)' : (valInt + '%');
        if (currentSavedDim !== null && valInt !== currentSavedDim) {
            label.innerHTML = '<span class="text-muted">' + currentSavedDim + '% &rarr;</span> ' + displayStr;
        } else {
            label.textContent = displayStr;
        }
    }

    var btn = document.getElementById('btn-save-dim');
    if (btn && currentSavedDim !== null) {
        btn.disabled = (valInt === currentSavedDim);
    }

    if (!skipPreview) {
        if (dimPreviewTimeout) {
            clearTimeout(dimPreviewTimeout);
        }
        dimPreviewTimeout = setTimeout(function () {
            apiPost('/api/display/config', { dim_percent: valInt, save: false }, function () { });
        }, 100);
    }
}

// Save dim setting via API
function saveDimSetting() {
    var slider = document.getElementById('input-dim-percent');
    if (!slider) return;
    var pct = parseInt(slider.value, 10);
    var btn = document.getElementById('btn-save-dim');
    if (btn) btn.disabled = true;

    apiPost('/api/display/config', { dim_percent: pct, save: true }, function (err, data) {
        if (!err && data && data.ok) {
            currentSavedDim = pct;
            if (btn) btn.disabled = true;
            // Reset label to pure value format without arrow
            updateDimLabel(pct, true);
            showToast('บันทึกความสว่าง Idle: ' + pct + '%', 'success');
        } else {
            if (btn) btn.disabled = false;
            showToast('บันทึกไม่สำเร็จ', 'error');
        }
    });
}
