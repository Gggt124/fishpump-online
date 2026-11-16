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
    if (typeof dryRunStopPolling === 'function') dryRunStopPolling();
    if (typeof pumpPollTimer !== 'undefined' && pumpPollTimer) { clearInterval(pumpPollTimer); pumpPollTimer = null; }
    if (typeof pumpTickTimer !== 'undefined' && pumpTickTimer) { clearInterval(pumpTickTimer); pumpTickTimer = null; }
    if (typeof coolingPollTimer !== 'undefined' && coolingPollTimer) { clearInterval(coolingPollTimer); coolingPollTimer = null; }
    if (typeof sensorsPollTimer !== 'undefined' && sensorsPollTimer) { clearInterval(sensorsPollTimer); sensorsPollTimer = null; }
    if (typeof alarmValuesTimer !== 'undefined' && alarmValuesTimer) { clearInterval(alarmValuesTimer); alarmValuesTimer = null; }
    if (typeof closeAlarmDialog === 'function') closeAlarmDialog();
    if (typeof sseEventSource !== 'undefined' && sseEventSource) { try { sseEventSource.close(); } catch (e) {} sseEventSource = null; }
    if (typeof sseReconnectTimer !== 'undefined' && sseReconnectTimer) { clearTimeout(sseReconnectTimer); sseReconnectTimer = null; }
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
    stopAlarmSummaryPolling();
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
        } else if (window.location.pathname === '/sensors') {
            e.preventDefault();
            syncSensorsStatus(true);
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
var coolingModalPoints = [];
var coolingModalCurveType = 'linear';
var coolingModalWired = false;
var sseEventSource = null;
var sseReconnectTimer = null;
var sensorsPollTimer = null;
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
    initDryRunCard();
    syncPumpStatus(true);
    refreshStatus();
    dashboardInterval = setInterval(refreshStatus, 10000);
    startPumpLiveTimers();
    document.addEventListener('visibilitychange', handlePumpVisibilityChange);
}

function pumpEl(id) {
    return document.getElementById(id);
}

/* ======== Dry-run protection card ======== */

var dryRunWired = false;
var dryRunStatusTimer = null;
var dryRunTickTimer = null;
var dryRunMeasureTimer = null;
var dryRunMeasureBusy = false;
var dryRunLocal = null;
var dryRunUnavailable = false;
var dryRunSaving = false;
var dryRunMeasureRunning = false;
var dryRunSuggested = 0;
var DRY_RUN_FIELDS = {
    prime_sec: 'dry-run-prime',
    dry_trip_sec: 'dry-run-trip',
    retry_wait_min: 'dry-run-wait',
    max_retries: 'dry-run-retries'
};

function dryRunFmtMmss(sec) {
    sec = Math.max(0, Math.floor(Number(sec) || 0));
    var m = Math.floor(sec / 60);
    var s = sec % 60;
    return (m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s;
}

function dryRunStatusText(ps) {
    if (!ps) return '';
    if (ps.state === 'disabled') return 'ยังไม่ได้เปิดการป้องกัน';
    if (ps.state === 'monitor') return 'เฝ้าดูอยู่';
    if (ps.state === 'wait') {
        return 'หยุดปั๊ม ลองใหม่ใน ' + dryRunFmtMmss(ps.wait_remaining_sec) +
            ' (ครั้งที่ ' + ps.trips + '/' + ps.max_retries + ')';
    }
    if (ps.state === 'locked') return 'ล็อกปั๊ม รอรับทราบที่หน้าแจ้งเตือน';
    return '';
}

function dryRunShowStatus() {
    var el = pumpEl('dry-run-status');
    if (!el) return;
    var ps = dryRunLocal;
    if (dryRunUnavailable) {
        el.textContent = 'ระบบป้องกันไม่พร้อมใช้งาน';
        el.classList.add('is-warn');
        return;
    }
    if (ps && ps.state === 'wait') {
        var elapsed = Math.floor((Date.now() - ps.at) / 1000);
        ps = { state: 'wait', trips: ps.trips, max_retries: ps.max_retries,
               wait_remaining_sec: Math.max(0, ps.wait_remaining_sec - elapsed) };
    }
    el.textContent = dryRunStatusText(ps);
    el.classList.toggle('is-warn', !!ps && ps.state === 'disabled');
}

function dryRunApplyStatus(ps) {
    if (ps) {
        dryRunLocal = {
            state: ps.state,
            trips: ps.trips,
            max_retries: ps.max_retries,
            wait_remaining_sec: ps.wait_remaining_sec || 0,
            at: Date.now()
        };
    }
    dryRunShowStatus();
}

function dryRunFillForm(cfg) {
    var en = pumpEl('dry-run-enabled');
    if (en) en.checked = !!cfg.enabled;
    var bo = pumpEl('dry-run-backoff');
    if (bo) bo.checked = !!cfg.retry_backoff;
    var k;
    for (k in DRY_RUN_FIELDS) {
        if (DRY_RUN_FIELDS.hasOwnProperty(k)) {
            var el = pumpEl(DRY_RUN_FIELDS[k]);
            if (el && cfg[k] != null) el.value = cfg[k];
        }
    }
    dryRunRenderWarning();
}

function dryRunUpdateButtons() {
    var save = pumpEl('dry-run-save');
    if (save) save.disabled = dryRunUnavailable || dryRunSaving;
    var mb = pumpEl('dry-run-measure-btn');
    if (mb) mb.disabled = dryRunUnavailable || dryRunMeasureRunning;
}

function dryRunClearErrors() {
    var k;
    for (k in DRY_RUN_FIELDS) {
        if (DRY_RUN_FIELDS.hasOwnProperty(k)) {
            var el = pumpEl(DRY_RUN_FIELDS[k]);
            if (el) el.classList.remove('invalid');
            setText(DRY_RUN_FIELDS[k] + '-error', '');
        }
    }
    setPumpAlert('dry-run-error', '');
}

function dryRunMarkField(apiName, msg) {
    var id = DRY_RUN_FIELDS[apiName];
    if (!id) return false;
    var el = pumpEl(id);
    if (el) el.classList.add('invalid');
    setText(id + '-error', msg);
    return true;
}

function dryRunLoad(fillForm) {
    apiGet('/api/protection', function (err, data) {
        if (isUnauthorizedError(err)) { handleUnauthorized(); return; }
        if (err || !data || !data.ok) {
            if (err && err.message === 'HTTP 503') {
                dryRunUnavailable = true;
            } else if (fillForm) {
                setPumpAlert('dry-run-error', 'โหลดการตั้งค่าการป้องกันไม่สำเร็จ');
            }
            dryRunUpdateButtons();
            dryRunShowStatus();
            return;
        }
        dryRunUnavailable = false;
        if (fillForm && data.pump) dryRunFillForm(data.pump);
        dryRunApplyStatus(data.pump_status);
        dryRunUpdateButtons();
        if (fillForm) dryRunResumeMeasure();
    });
}

function dryRunStopStatusTimers() {
    if (dryRunStatusTimer) { clearInterval(dryRunStatusTimer); dryRunStatusTimer = null; }
    if (dryRunTickTimer) { clearInterval(dryRunTickTimer); dryRunTickTimer = null; }
}

function dryRunStartPolling() {
    dryRunStopStatusTimers();
    dryRunStatusTimer = setInterval(function () { dryRunLoad(false); }, 5000);
    dryRunTickTimer = setInterval(dryRunShowStatus, 1000);
}

function dryRunStopPolling() {
    dryRunStopStatusTimers();
    if (dryRunMeasureTimer) { clearInterval(dryRunMeasureTimer); dryRunMeasureTimer = null; }
    dryRunMeasureBusy = false;
}

function dryRunRenderWarning() {
    var el = pumpEl('dry-run-warning');
    if (!el) return;
    var en = pumpEl('dry-run-enabled');
    var primeEl = pumpEl('dry-run-prime');
    var sets = [];
    if (en && en.checked && primeEl && primeEl.value !== '' && pumpConfig) {
        var prime = parseInt(primeEl.value, 10);
        var on1 = pumpConfig.water_set1_on != null ? pumpConfig.water_set1_on : pumpConfig.timer1_on_sec;
        var on2 = pumpConfig.water_set2_on != null ? pumpConfig.water_set2_on : pumpConfig.timer2_on_sec;
        if (!isNaN(prime)) {
            if (on1 != null && Number(on1) <= prime) sets.push(1);
            if (on2 != null && Number(on2) <= prime) sets.push(2);
        }
    }
    var parts = [];
    var i;
    for (i = 0; i < sets.length; i++) {
        parts.push('เวลาเปิดปั๊ม (ชุด ' + sets[i] + ') สั้นกว่าหรือเท่ากับช่วงผ่อนผัน การป้องกันจะไม่ทำงาน');
    }
    setPumpAlert('dry-run-warning', parts.join(' · '));
}

function dryRunReadForm() {
    var out = { enabled: !!pumpEl('dry-run-enabled').checked, retry_backoff: !!pumpEl('dry-run-backoff').checked };
    var ok = true;
    var k;
    for (k in DRY_RUN_FIELDS) {
        if (DRY_RUN_FIELDS.hasOwnProperty(k)) {
            var el = pumpEl(DRY_RUN_FIELDS[k]);
            var raw = el ? String(el.value).trim() : '';
            var max = (k === 'max_retries') ? 255 : 65535;
            var n = Number(raw);
            if (raw === '' || !isFinite(n) || Math.floor(n) !== n || n < 0 || n > max) {
                dryRunMarkField(k, 'กรอกจำนวนเต็ม 0 ถึง ' + max);
                ok = false;
            } else {
                out[k] = n;
            }
        }
    }
    return ok ? out : null;
}

function dryRunSave() {
    if (dryRunSaving || dryRunUnavailable) return;
    dryRunClearErrors();
    var cfg = dryRunReadForm();
    if (!cfg) return;
    dryRunSaving = true;
    dryRunUpdateButtons();
    apiPost('/api/protection', { pump: cfg }, function (err, data) {
        dryRunSaving = false;
        if (err || !data || !data.ok) {
            var code = data && data.error;
            if (code === 'unavailable') {
                dryRunUnavailable = true;
                dryRunShowStatus();
            } else if (code === 'missing_field' || code === 'invalid_type' || code === 'value_out_of_range') {
                if (!dryRunMarkField(data.message, 'ค่าไม่ถูกต้อง')) setPumpAlert('dry-run-error', 'ค่าไม่ถูกต้อง');
            } else if (code === 'storage_failed') {
                setPumpAlert('dry-run-error', 'บันทึกไม่สำเร็จ ลองใหม่อีกครั้ง');
            } else {
                setPumpAlert('dry-run-error', 'บันทึกไม่สำเร็จ');
            }
            dryRunUpdateButtons();
            showToast('บันทึกไม่สำเร็จ', 'error');
            return;
        }
        if (data.pump) dryRunFillForm(data.pump);
        dryRunApplyStatus(data.pump_status);
        dryRunUpdateButtons();
        showToast('บันทึกการป้องกันแล้ว', 'success');
    });
}

function dryRunSetMeasureText(text, showStop, showUse) {
    var box = pumpEl('dry-run-measure');
    if (box) box.classList.toggle('hidden', !text);
    setText('dry-run-measure-text', text);
    var stop = pumpEl('dry-run-measure-stop');
    if (stop) stop.classList.toggle('hidden', !showStop);
    var use = pumpEl('dry-run-measure-use');
    if (use) use.classList.toggle('hidden', !showUse);
}

function dryRunRenderMeasure(m) {
    var secs = Math.floor((Number(m.elapsed_ms) || 0) / 1000);
    if (m.state === 'running') {
        dryRunMeasureRunning = true;
        dryRunSetMeasureText('กำลังวัด... ' + secs + ' วินาที', true, false);
    } else {
        dryRunMeasureRunning = false;
        if (dryRunMeasureTimer) { clearInterval(dryRunMeasureTimer); dryRunMeasureTimer = null; }
        if (m.state === 'done') {
            dryRunSuggested = Number(m.suggested_prime_sec) || 0;
            dryRunSetMeasureText('น้ำมาถึงใน ' + secs + ' วินาที แนะนำช่วงผ่อนผัน ' + dryRunSuggested + ' วินาที', false, true);
        } else if (m.state === 'no_flow') {
            dryRunSetMeasureText('ไม่พบน้ำไหลใน 120 วินาที ตรวจ flow switch / การล่อน้ำ', false, false);
        } else if (m.state === 'stopped') {
            dryRunSetMeasureText('หยุดวัดแล้ว', false, false);
        } else {
            dryRunSetMeasureText('', false, false);
        }
    }
    dryRunUpdateButtons();
}

function dryRunMeasurePoll() {
    if (dryRunMeasureBusy) return;
    dryRunMeasureBusy = true;
    apiGet('/api/protection/measure', function (err, data) {
        dryRunMeasureBusy = false;
        if (err || !data || !data.ok) return;
        dryRunRenderMeasure(data);
    }, 3000);
}

function dryRunBeginMeasurePolling() {
    if (dryRunMeasureTimer) clearInterval(dryRunMeasureTimer);
    dryRunMeasureTimer = setInterval(dryRunMeasurePoll, 1000);
}

function dryRunResumeMeasure() {
    apiGet('/api/protection/measure', function (err, data) {
        if (err || !data || !data.ok) {
            dryRunMeasureRunning = false;
            dryRunSetMeasureText('', false, false);
            dryRunUpdateButtons();
            return;
        }
        dryRunRenderMeasure(data);
        if (data.state === 'running') dryRunBeginMeasurePolling();
    });
}

function dryRunMeasureStart() {
    if (dryRunMeasureRunning || dryRunUnavailable) return;
    if (!window.confirm('ปั๊มอาจเดินแห้งได้นานสุด 120 วินาทีระหว่างวัด ดำเนินการต่อ?')) return;
    setPumpAlert('dry-run-error', '');
    apiPost('/api/protection/measure', { action: 'start' }, function (err, data) {
        if (err || !data || !data.ok) {
            var code = data && data.error;
            setPumpAlert('dry-run-error', code === 'measure_unavailable' ?
                'เริ่มวัดไม่ได้ในขณะนี้ ตรวจสอบสถานะปั๊มและการป้องกัน' : 'เริ่มวัดไม่สำเร็จ');
            return;
        }
        dryRunRenderMeasure(data);
        if (data.state === 'running') dryRunBeginMeasurePolling();
    });
}

function dryRunMeasureStop() {
    apiPost('/api/protection/measure', { action: 'stop' }, function (err, data) {
        if (err || !data || !data.ok) {
            setPumpAlert('dry-run-error', 'หยุดวัดไม่สำเร็จ');
            return;
        }
        dryRunRenderMeasure(data);
    });
}

function dryRunStartError(data) {
    if (!data) return '';
    if (data.error === 'dry_run_locked') return 'ปั๊มถูกล็อกเพราะเดินแห้ง กดรับทราบที่หน้าแจ้งเตือนก่อน';
    if (data.error === 'dry_run_waiting') return 'ปั๊มหยุดพักเพราะเดินแห้ง จะลองใหม่ใน ' + dryRunFmtMmss(data.wait_remaining_sec);
    if (data.error === 'measuring') return 'กำลังวัดอยู่ รอให้เสร็จก่อน';
    return '';
}

function initDryRunCard() {
    if (!pumpEl('dry-run-card')) return;
    if (!dryRunWired) {
        dryRunWired = true;
        var form = pumpEl('dry-run-form');
        if (form) form.addEventListener('submit', function (e) { e.preventDefault(); dryRunSave(); });
        var ids = ['dry-run-enabled', 'dry-run-prime'];
        var i;
        for (i = 0; i < ids.length; i++) {
            var f = pumpEl(ids[i]);
            if (f) {
                f.addEventListener('input', dryRunRenderWarning);
                f.addEventListener('change', dryRunRenderWarning);
            }
        }
        var mb = pumpEl('dry-run-measure-btn');
        if (mb) mb.addEventListener('click', dryRunMeasureStart);
        var ms = pumpEl('dry-run-measure-stop');
        if (ms) ms.addEventListener('click', dryRunMeasureStop);
        var mu = pumpEl('dry-run-measure-use');
        if (mu) mu.addEventListener('click', function () {
            var p = pumpEl('dry-run-prime');
            if (p) { p.value = dryRunSuggested; dryRunRenderWarning(); }
        });
    }
    dryRunClearErrors();
    dryRunLoad(true);
    dryRunStartPolling();
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
    wireCoolingActions();
    wireCoolingModal();
    initSettingsTabs();
    updateCoolingButtons();
    loadCoolingConfig();
    syncCoolingStatus(true);
    startCoolingStatusTimer();
    initTelemetrySSE();
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
            var dryMsg = dryRunStartError(data);
            setPumpAlert(system + '-status-error', dryMsg || ((data && data.message) ? data.message : 'สั่งงานไม่สำเร็จ'));
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
        dryRunRenderWarning();
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
}

function wireCoolingActions() {
    var autoBtn = pumpEl('cooling-mode-auto');
    var forceBtn = pumpEl('cooling-mode-force-off');
    var testBtn = pumpEl('cooling-test-on');
    if (autoBtn) autoBtn.onclick = function () { setCoolingMode('auto'); };
    if (forceBtn) forceBtn.onclick = function () { setCoolingMode('force_off'); };
    if (testBtn) testBtn.onclick = function () { setCoolingMode('test_on'); };
}

function wireCoolingModal() {
    if (coolingModalWired) return;
    coolingModalWired = true;

    var openBtn1 = pumpEl('cooling-open-modal-btn');
    var openBtn2 = pumpEl('cooling-open-modal-btn-2');
    var closeBtn = pumpEl('cooling-modal-close-x');
    var cancelBtn = pumpEl('cooling-modal-cancel');
    var addPointBtn = pumpEl('cooling-add-point-btn');
    var modal = pumpEl('cooling-rule-modal');
    var form = pumpEl('cooling-modal-form');

    if (openBtn1) openBtn1.onclick = function () { openCoolingRuleModal(); };
    if (openBtn2) openBtn2.onclick = function () { openCoolingRuleModal(); };
    if (closeBtn) closeBtn.onclick = function () { closeCoolingRuleModal(); };
    if (cancelBtn) cancelBtn.onclick = function () { closeCoolingRuleModal(); };

    if (modal) {
        modal.addEventListener('cancel', function () {
            closeCoolingRuleModal();
        });
        modal.addEventListener('click', function (e) {
            if (e.target === modal) closeCoolingRuleModal();
        });
    }

    if (addPointBtn) {
        addPointBtn.onclick = function () { addCoolingRulePoint(); };
    }

    var linearRadio = pumpEl('cooling-curve-linear');
    var stepRadio = pumpEl('cooling-curve-step');
    if (linearRadio) {
        linearRadio.onchange = function () {
            coolingModalCurveType = 'linear';
            refreshCoolingModalPreview();
        };
    }
    if (stepRadio) {
        stepRadio.onchange = function () {
            coolingModalCurveType = 'step';
            refreshCoolingModalPreview();
        };
    }

    var tbody = pumpEl('cooling-points-tbody');
    if (tbody) {
        tbody.addEventListener('input', function (e) {
            var target = e.target;
            var idx = parseInt(target.dataset.index, 10);
            if (isNaN(idx) || !coolingModalPoints[idx]) return;

            if (target.classList.contains('cooling-point-temp')) {
                var v = parseFloat(target.value);
                if (!isNaN(v)) coolingModalPoints[idx].temp_c = v;
            } else if (target.classList.contains('cooling-point-volt')) {
                var v = parseFloat(target.value);
                if (!isNaN(v)) coolingModalPoints[idx].output_volts = v;
            }
            refreshCoolingModalPreview();
        });

        tbody.addEventListener('click', function (e) {
            var btn = e.target.closest('.cooling-delete-point-btn');
            if (!btn) return;
            var idx = parseInt(btn.dataset.index, 10);
            if (isNaN(idx)) return;
            deleteCoolingRulePoint(idx);
        });
    }

    if (form) {
        form.onsubmit = function (e) {
            e.preventDefault();
            saveCoolingModalRules();
        };
    }
}

function openCoolingRuleModal() {
    setCoolingModalAlert('');
    var modal = pumpEl('cooling-rule-modal');
    if (!modal) return;

    apiGet('/api/cooling/config', function (err, data) {
        if (!err && data && data.ok) {
            coolingConfig = data;
            coolingConfigLoaded = true;
            populateCoolingModalFromConfig(data);
            updateCoolingSummaryPanel(data);
        } else {
            populateCoolingModalFromConfig(coolingConfig);
        }
    });

    populateCoolingModalFromConfig(coolingConfig);
    modal.showModal();
}

function closeCoolingRuleModal() {
    var modal = pumpEl('cooling-rule-modal');
    if (modal && modal.open) {
        modal.close();
    }
    setCoolingModalAlert('');
}

function setCoolingModalAlert(msg) {
    setPumpAlert('cooling-modal-error', msg);
}

function populateCoolingModalFromConfig(config) {
    coolingModalPoints = [];
    if (config && Array.isArray(config.points) && config.points.length >= 2) {
        for (var i = 0; i < config.points.length; i++) {
            var pt = config.points[i];
            coolingModalPoints.push({
                temp_c: typeof pt.temp_c === 'number' ? pt.temp_c : (pt.temp_c_x10 / 10.0),
                output_volts: typeof pt.output_volts === 'number' ? pt.output_volts : (pt.output_mv / 1000.0)
            });
        }
        coolingModalCurveType = config.curve_type || 'linear';
    } else {
        coolingModalPoints = [
            { temp_c: 24.0, output_volts: 0.0 },
            { temp_c: 28.0, output_volts: 5.0 },
            { temp_c: 32.0, output_volts: 10.0 }
        ];
        coolingModalCurveType = 'linear';
    }

    var linearRadio = pumpEl('cooling-curve-linear');
    var stepRadio = pumpEl('cooling-curve-step');
    if (coolingModalCurveType === 'step') {
        if (stepRadio) stepRadio.checked = true;
    } else {
        if (linearRadio) linearRadio.checked = true;
    }

    var minOff = pumpEl('cooling-modal-min-off');
    if (minOff) {
        minOff.value = (config && (config.min_off_sec || config.compressor_min_off_sec)) ? (config.min_off_sec || config.compressor_min_off_sec) : 180;
    }

    var testTimeout = pumpEl('cooling-modal-test-timeout');
    if (testTimeout) {
        testTimeout.value = (config && config.test_timeout_sec) ? config.test_timeout_sec : 30;
    }

    var autoEnable = pumpEl('cooling-modal-auto-enable');
    if (autoEnable) {
        autoEnable.checked = config ? !!config.auto_enable : true;
    }

    renderCoolingPointsTable();
    refreshCoolingModalPreview();
}

function renderCoolingPointsTable() {
    var tbody = pumpEl('cooling-points-tbody');
    if (!tbody) return;
    tbody.innerHTML = '';

    var canDelete = coolingModalPoints.length > 2;
    var canAdd = coolingModalPoints.length < 8;

    var addBtn = pumpEl('cooling-add-point-btn');
    if (addBtn) addBtn.disabled = !canAdd;

    for (var i = 0; i < coolingModalPoints.length; i++) {
        var pt = coolingModalPoints[i];
        var tr = document.createElement('tr');
        tr.dataset.index = i;

        var tdIndex = document.createElement('td');
        tdIndex.style.textAlign = 'center';
        tdIndex.innerHTML = '<span class="point-badge">#' + (i + 1) + '</span>';
        tr.appendChild(tdIndex);

        var tdTemp = document.createElement('td');
        tdTemp.innerHTML = '<div class="input-with-unit-sm">' +
            '<input type="number" class="cooling-point-temp" data-index="' + i + '" value="' + pt.temp_c.toFixed(1) + '" step="0.1" min="-50" max="150" required>' +
            '<span class="unit-sm">°C</span>' +
        '</div>';
        tr.appendChild(tdTemp);

        var tdVolt = document.createElement('td');
        tdVolt.innerHTML = '<div class="input-with-unit-sm">' +
            '<input type="number" class="cooling-point-volt" data-index="' + i + '" value="' + pt.output_volts.toFixed(1) + '" step="0.1" min="0" max="10" required>' +
            '<span class="unit-sm">V</span>' +
        '</div>';
        tr.appendChild(tdVolt);

        var tdAction = document.createElement('td');
        tdAction.style.textAlign = 'center';
        tdAction.innerHTML = '<button type="button" class="btn-icon-danger cooling-delete-point-btn" data-index="' + i + '" ' + (canDelete ? '' : 'disabled') + ' title="ลบจุดนี้">✕</button>';
        tr.appendChild(tdAction);

        tbody.appendChild(tr);
    }
}

function addCoolingRulePoint() {
    if (coolingModalPoints.length >= 8) return;
    var last = coolingModalPoints[coolingModalPoints.length - 1];
    var nextTemp = last ? Math.round((last.temp_c + 2.0) * 10) / 10 : 30.0;
    var nextVolt = last ? Math.min(10.0, Math.round((last.output_volts + 1.5) * 10) / 10) : 5.0;
    coolingModalPoints.push({
        temp_c: nextTemp,
        output_volts: nextVolt
    });
    renderCoolingPointsTable();
    refreshCoolingModalPreview();
}

function deleteCoolingRulePoint(idx) {
    if (coolingModalPoints.length <= 2) return;
    coolingModalPoints.splice(idx, 1);
    renderCoolingPointsTable();
    refreshCoolingModalPreview();
}

function refreshCoolingModalPreview() {
    var curTemp = (coolingLastStatus && coolingLastStatus.temperature_valid) ? coolingLastStatus.temperature_c : null;
    var curVolts = coolingLastStatus ? (typeof coolingLastStatus.output_volts === 'number' ? coolingLastStatus.output_volts : coolingLastStatus.output_mv / 1000.0) : null;
    drawCoolingCurveSvg(coolingModalPoints, coolingModalCurveType, curTemp, curVolts);
}

function drawCoolingCurveSvg(points, curveType, currentTemp, currentVolts) {
    var svg = pumpEl('cooling-curve-svg');
    if (!svg) return;

    var width = 520;
    var height = 220;
    var padLeft = 45;
    var padRight = 30;
    var padTop = 20;
    var padBottom = 35;
    var graphW = width - padLeft - padRight;
    var graphH = height - padTop - padBottom;

    if (!points || points.length < 2) {
        svg.innerHTML = '<text x="' + (width / 2) + '" y="' + (height / 2) + '" text-anchor="middle" fill="#888">ต้องมีอย่างน้อย 2 จุดควบคุม</text>';
        return;
    }

    var minT = points[0].temp_c;
    var maxT = points[points.length - 1].temp_c;
    for (var i = 0; i < points.length; i++) {
        if (points[i].temp_c < minT) minT = points[i].temp_c;
        if (points[i].temp_c > maxT) maxT = points[i].temp_c;
    }
    if (typeof currentTemp === 'number' && isFinite(currentTemp)) {
        if (currentTemp < minT) minT = currentTemp;
        if (currentTemp > maxT) maxT = currentTemp;
    }
    var tSpan = maxT - minT;
    if (tSpan <= 0) tSpan = 10;
    var xMin = Math.floor(minT - Math.max(2, tSpan * 0.1));
    var xMax = Math.ceil(maxT + Math.max(2, tSpan * 0.1));
    var xRange = xMax - xMin;
    if (xRange <= 0) xRange = 10;

    function toX(t) {
        return padLeft + ((t - xMin) / xRange) * graphW;
    }
    function toY(v) {
        var clampedV = Math.max(0, Math.min(10, v));
        return padTop + (1.0 - (clampedV / 10.0)) * graphH;
    }

    var elements = [];

    elements.push('<defs>' +
        '<linearGradient id="cooling-curve-grad" x1="0%" y1="0%" x2="0%" y2="100%">' +
            '<stop offset="0%" stop-color="#00b4d8" stop-opacity="0.35" />' +
            '<stop offset="100%" stop-color="#00b4d8" stop-opacity="0.02" />' +
        '</linearGradient>' +
    '</defs>');

    // Y-axis grid & labels
    var vSteps = [0, 2.5, 5, 7.5, 10];
    for (var j = 0; j < vSteps.length; j++) {
        var vy = toY(vSteps[j]);
        elements.push('<line x1="' + padLeft + '" y1="' + vy + '" x2="' + (padLeft + graphW) + '" y2="' + vy + '" stroke="rgba(255,255,255,0.1)" stroke-dasharray="3,3" />');
        elements.push('<text x="' + (padLeft - 8) + '" y="' + (vy + 4) + '" text-anchor="end" font-size="11" fill="#888">' + vSteps[j].toFixed(1) + 'V</text>');
    }

    // X-axis ticks & labels
    var numTicks = 5;
    for (var k = 0; k <= numTicks; k++) {
        var tickT = xMin + (k / numTicks) * xRange;
        var tx = toX(tickT);
        elements.push('<line x1="' + tx + '" y1="' + (padTop + graphH) + '" x2="' + tx + '" y2="' + (padTop + graphH + 4) + '" stroke="#666" />');
        elements.push('<text x="' + tx + '" y="' + (padTop + graphH + 18) + '" text-anchor="middle" font-size="11" fill="#888">' + tickT.toFixed(0) + '°C</text>');
    }

    // Axes
    elements.push('<line x1="' + padLeft + '" y1="' + padTop + '" x2="' + padLeft + '" y2="' + (padTop + graphH) + '" stroke="#666" stroke-width="1.5" />');
    elements.push('<line x1="' + padLeft + '" y1="' + (padTop + graphH) + '" x2="' + (padLeft + graphW) + '" y2="' + (padTop + graphH) + '" stroke="#666" stroke-width="1.5" />');

    // Curve Path: below T0, inverter output is 0V
    var pathD = '';
    pathD = 'M ' + padLeft + ' ' + toY(0.0);
    pathD += ' L ' + toX(points[0].temp_c) + ' ' + toY(0.0);
    pathD += ' L ' + toX(points[0].temp_c) + ' ' + toY(points[0].output_volts);

    if (curveType === 'step') {
        for (var p = 0; p < points.length - 1; p++) {
            var currP = points[p];
            var nextP = points[p + 1];
            pathD += ' L ' + toX(nextP.temp_c) + ' ' + toY(currP.output_volts);
            pathD += ' L ' + toX(nextP.temp_c) + ' ' + toY(nextP.output_volts);
        }
        pathD += ' L ' + (padLeft + graphW) + ' ' + toY(points[points.length - 1].output_volts);
    } else {
        for (var p = 1; p < points.length; p++) {
            pathD += ' L ' + toX(points[p].temp_c) + ' ' + toY(points[p].output_volts);
        }
        pathD += ' L ' + (padLeft + graphW) + ' ' + toY(points[points.length - 1].output_volts);
    }

    var areaD = pathD + ' L ' + (padLeft + graphW) + ' ' + (padTop + graphH) + ' L ' + padLeft + ' ' + (padTop + graphH) + ' Z';

    elements.push('<path d="' + areaD + '" fill="url(#cooling-curve-grad)" />');
    elements.push('<path d="' + pathD + '" fill="none" stroke="#00b4d8" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" />');

    // Points markers
    for (var m = 0; m < points.length; m++) {
        var px = toX(points[m].temp_c);
        var py = toY(points[m].output_volts);
        elements.push('<circle cx="' + px + '" cy="' + py + '" r="5" fill="#fff" stroke="#00b4d8" stroke-width="2.5" />');
        var textY = (py < padTop + 20) ? py + 16 : py - 10;
        elements.push('<text x="' + px + '" y="' + textY + '" text-anchor="middle" font-size="10" font-weight="600" fill="#00b4d8">' +
            points[m].temp_c.toFixed(1) + '°, ' + points[m].output_volts.toFixed(1) + 'V</text>');
    }

    // Live operating point marker
    if (typeof currentTemp === 'number' && isFinite(currentTemp) && currentTemp >= xMin && currentTemp <= xMax) {
        var cx = toX(currentTemp);
        var cy = (typeof currentVolts === 'number' && isFinite(currentVolts)) ? toY(currentVolts) : toY(0);
        elements.push('<line x1="' + cx + '" y1="' + padTop + '" x2="' + cx + '" y2="' + (padTop + graphH) + '" stroke="#ef4444" stroke-width="1.5" stroke-dasharray="4,3" />');
        elements.push('<circle cx="' + cx + '" cy="' + cy + '" r="6" fill="#ef4444" stroke="#fff" stroke-width="2" />');
        elements.push('<text x="' + cx + '" y="' + (padTop - 5) + '" text-anchor="middle" font-size="10" font-weight="bold" fill="#ef4444">Now ' + currentTemp.toFixed(1) + '°C</text>');
    }

    svg.innerHTML = elements.join('');
}

function saveCoolingModalRules() {
    if (coolingSavingConfig) return;
    setCoolingModalAlert('');

    // Read directly from DOM input elements to ensure sync even if inputs were cleared
    var tbody = pumpEl('cooling-points-tbody');
    var tempInputs = tbody ? tbody.querySelectorAll('.cooling-point-temp') : [];
    var voltInputs = tbody ? tbody.querySelectorAll('.cooling-point-volt') : [];

    if (tempInputs.length < 2 || tempInputs.length > 8) {
        setCoolingModalAlert('จำนวนจุดควบคุมต้องอยู่ระหว่าง 2 ถึง 8 จุด');
        return;
    }

    var parsedPoints = [];
    for (var i = 0; i < tempInputs.length; i++) {
        var tRaw = tempInputs[i].value.trim();
        var vRaw = voltInputs[i].value.trim();

        if (tRaw === '') {
            setCoolingModalAlert('กรุณากรอกอุณหภูมิในจุดที่ #' + (i + 1));
            tempInputs[i].focus();
            return;
        }
        if (vRaw === '') {
            setCoolingModalAlert('กรุณากรอกแรงดันในจุดที่ #' + (i + 1));
            voltInputs[i].focus();
            return;
        }

        var t = parseFloat(tRaw);
        var v = parseFloat(vRaw);

        if (isNaN(t) || t < -50 || t > 150) {
            setCoolingModalAlert('อุณหภูมิในจุดที่ #' + (i + 1) + ' ต้องอยู่ระหว่าง -50.0 ถึง 150.0 °C');
            tempInputs[i].focus();
            return;
        }
        if (isNaN(v) || v < 0 || v > 10.0) {
            setCoolingModalAlert('แรงดันในจุดที่ #' + (i + 1) + ' ต้องอยู่ระหว่าง 0.0 ถึง 10.0 V');
            voltInputs[i].focus();
            return;
        }

        if (i > 0 && t <= parsedPoints[i - 1].temp_c) {
            setCoolingModalAlert('อุณหภูมิต้องเรียงจากน้อยไปมาก จุด #' + (i + 1) + ' (' + t.toFixed(1) + '°C) ต้องมากกว่าจุด #' + i + ' (' + parsedPoints[i - 1].temp_c.toFixed(1) + '°C)');
            tempInputs[i].focus();
            return;
        }

        parsedPoints.push({
            temp_c: t,
            output_volts: v
        });
    }

    // Sync validated points to coolingModalPoints
    coolingModalPoints = parsedPoints;

    var minOffInput = pumpEl('cooling-modal-min-off');
    var minOffVal = minOffInput ? parseInt(minOffInput.value, 10) : 180;
    if (isNaN(minOffVal) || minOffVal < 120 || minOffVal > 86400) {
        setCoolingModalAlert('เวลา Lockout ต้องเป็นจำนวนเต็มระหว่าง 120 ถึง 86400 วินาที');
        return;
    }

    var testTimeoutInput = pumpEl('cooling-modal-test-timeout');
    var testTimeoutVal = testTimeoutInput ? parseInt(testTimeoutInput.value, 10) : 30;
    if (isNaN(testTimeoutVal) || testTimeoutVal < 1 || testTimeoutVal > 3600) {
        setCoolingModalAlert('เวลาทดสอบ Test ON ต้องอยู่ระหว่าง 1 ถึง 3600 วินาที');
        return;
    }

    var autoEnableInput = pumpEl('cooling-modal-auto-enable');
    var autoEnable = autoEnableInput ? !!autoEnableInput.checked : true;

    var payload = {
        curve_type: coolingModalCurveType,
        points: coolingModalPoints.map(function (pt) {
            return {
                temp_c: Math.round(pt.temp_c * 10) / 10,
                output_volts: Math.round(pt.output_volts * 100) / 100
            };
        }),
        min_off_sec: minOffVal,
        compressor_min_off_sec: minOffVal,
        test_timeout_sec: testTimeoutVal,
        auto_enable: autoEnable
    };

    var saveBtn = pumpEl('cooling-modal-save');
    setLoading(saveBtn, true);
    coolingSavingConfig = true;

    apiPost('/api/cooling/config', payload, function (err, data) {
        setLoading(saveBtn, false);
        coolingSavingConfig = false;
        if (err || !data || !data.ok) {
            var msg = (data && data.message) ? data.message : 'บันทึกการตั้งค่าไม่สำเร็จ';
            setCoolingModalAlert(msg);
            return;
        }

        coolingConfig = data.payload || data;
        coolingConfigLoaded = true;
        closeCoolingRuleModal();
        showToast('บันทึกกฎควบคุมความเย็นเรียบร้อย', 'success');
        updateCoolingSummaryPanel(coolingConfig);
        if (data.status) applyCoolingStatus(data.status, true);
        syncCoolingStatus(true);
    });
}

function updateCoolingSummaryPanel(config) {
    if (!config) return;
    var curveTypeStr = (config.curve_type === 'step') ? 'Discrete Step' : 'Linear Interpolation';
    var curveTagStr = (config.curve_type === 'step') ? 'Step' : 'Linear';
    setText('cooling-summary-curve-type', curveTypeStr);
    setText('cooling-summary-curve-tag', curveTagStr);

    var pts = config.points || [];
    setText('cooling-summary-point-count', pts.length + ' จุด');

    if (pts.length >= 2) {
        var minT = (typeof pts[0].temp_c === 'number') ? pts[0].temp_c : (pts[0].temp_c_x10 / 10.0);
        var maxT = (typeof pts[pts.length - 1].temp_c === 'number') ? pts[pts.length - 1].temp_c : (pts[pts.length - 1].temp_c_x10 / 10.0);
        setText('cooling-summary-temp-range', minT.toFixed(1) + '°C – ' + maxT.toFixed(1) + '°C');
    } else {
        setText('cooling-summary-temp-range', '--');
    }

    var minOff = config.min_off_sec || config.compressor_min_off_sec || 180;
    setText('cooling-summary-lockout', minOff + ' วินาที');

    var listEl = pumpEl('cooling-summary-points-list');
    if (listEl) {
        var chips = [];
        for (var i = 0; i < pts.length; i++) {
            var p = pts[i];
            var tc = (typeof p.temp_c === 'number') ? p.temp_c : (p.temp_c_x10 / 10.0);
            var ov = (typeof p.output_volts === 'number') ? p.output_volts : (p.output_mv / 1000.0);
            chips.push('<span class="cooling-rule-chip">#' + (i + 1) + ' ' + tc.toFixed(1) + '°C → ' + ov.toFixed(1) + 'V</span>');
        }
        listEl.innerHTML = chips.join('');
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
        updateCoolingSummaryPanel(data);
        if (data.status) applyCoolingStatus(data.status, true);
        setCoolingClean('โหลดค่า cooling แล้ว');
    });
}

function setCoolingConfigFields(data) {
    if (!data) return;
    updateCoolingSummaryPanel(data);
}

function updateCoolingSettingsPreview() {
    // backward-compatibility stub
}

function updateCoolingLockoutHint() {
    // backward-compatibility stub
}

function saveCoolingConfig() {
    openCoolingRuleModal();
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
    // Handled in modal
}

function initTelemetrySSE() {
    if (typeof EventSource === 'undefined') return;
    if (sseEventSource && sseEventSource.readyState !== EventSource.CLOSED) return;

    try {
        sseEventSource = new EventSource('/api/events');
        sseEventSource.addEventListener('cooling', function (e) {
            try {
                var data = JSON.parse(e.data);
                handleCoolingSseEvent(data);
            } catch (err) {
                console.error('SSE cooling parse error', err);
            }
        });
        sseEventSource.addEventListener('power', function (e) {
            try {
                handlePowerSseEvent(JSON.parse(e.data));
            } catch (err) {
                console.error('SSE power parse error', err);
            }
        });
        sseEventSource.addEventListener('water', function (e) {
            try {
                handleWaterSseEvent(JSON.parse(e.data));
            } catch (err) {
                console.error('SSE water parse error', err);
            }
        });
        sseEventSource.addEventListener('alarm', function (e) {
            try {
                handleAlarmSseEvent(JSON.parse(e.data));
            } catch (err) {
                console.error('SSE alarm parse error', err);
            }
        });
        sseEventSource.addEventListener('flow', function (e) {
            try {
                handleFlowSseEvent(JSON.parse(e.data));
            } catch (err) {
                console.error('SSE flow parse error', err);
            }
        });
        sseEventSource.onerror = function () {
            if (sseEventSource) {
                sseEventSource.close();
                sseEventSource = null;
            }
            if (!sseReconnectTimer) {
                sseReconnectTimer = setTimeout(function () {
                    sseReconnectTimer = null;
                    var p = window.location.pathname;
                    if (p === '/cooling' || p === '/sensors' || p === '/alarms') {
                        initTelemetrySSE();
                    }
                }, 5000);
            }
        };
    } catch (e) {
        console.error('SSE init error', e);
    }
}

/* ======== Sensors Page (PZEM power + SEN0708 pH) ======== */

var SENSORS_POLL_MS = 5000;
var sensorsWired = false;
var sensorsWater = { valid: false, fault: true, stable: false };

var CAL_STATE_TEXT = {
    idle: 'ยังไม่เริ่มคาลิเบรต',
    waiting_point2: 'บันทึกจุดที่ 1 แล้ว — ย้ายหัววัดไปน้ำยาจุดที่ 2',
    done: 'คาลิเบรตเสร็จ — ตรวจสอบในน้ำยา pH 6.86'
};
var CAL_ERROR_TEXT = {
    out_of_range: 'ค่าน้ำยาอยู่นอกช่วงของจุดนี้',
    not_stable: 'ค่ายังไม่นิ่ง',
    wrong_step: 'ต้องทำจุดที่ 1 ก่อน',
    sensor_fault: 'เซนเซอร์ไม่ตอบ',
    sensor_write: 'เซนเซอร์ไม่ยืนยันการเขียน',
    timeout: 'หมดเวลารอจุดที่ 2 (10 นาที)'
};

function fmtNum(v, digits) {
    return (typeof v === 'number' && isFinite(v)) ? v.toFixed(digits) : '--';
}

function setSensorNotice(id, notice) {
    var el = document.getElementById(id);
    if (!el) return;
    if (!notice || !notice.text) {
        el.classList.add('hidden');
        el.textContent = '';
        return;
    }
    el.textContent = notice.text;
    el.classList.remove('hidden');
    el.classList.toggle('notice-ok', !!notice.ok);
    el.classList.toggle('notice-error', !notice.ok);
}

function renderPowerChannel(ch, d) {
    var card = document.getElementById('power-card-' + ch);
    if (!card || !d) return;
    var fields = {
        current: fmtNum(d.current, 3),
        power: fmtNum(d.power, 1),
        voltage: fmtNum(d.voltage, 1),
        frequency: fmtNum(d.frequency, 1),
        pf: fmtNum(d.pf, 2),
        energy: fmtNum(typeof d.energy_wh === 'number' ? d.energy_wh / 1000 : null, 3)
    };
    for (var key in fields) {
        var el = card.querySelector('[data-field="' + key + '"]');
        if (el && el.textContent !== fields[key]) el.textContent = fields[key];
    }
    var values = card.querySelector('[data-role="values"]');
    if (values) values.classList.toggle('stale', !d.valid);
    var banner = card.querySelector('[data-role="fault"]');
    if (banner) banner.classList.toggle('hidden', !d.fault);
    var resetBtn = card.querySelector('[data-reset-energy]');
    if (resetBtn) resetBtn.disabled = !!d.fault;
}

function handlePowerSseEvent(data) {
    if (!data || (data.ch !== 0 && data.ch !== 1)) return;
    renderPowerChannel(data.ch, {
        valid: !!data.valid, fault: !!data.fault,
        voltage: data.v, current: data.a, power: data.w,
        energy_wh: data.wh, frequency: data.hz, pf: data.pf
    });
}

function updateCalButtons() {
    var ready = sensorsWater.valid && sensorsWater.stable && !sensorsWater.fault;
    ['cal-p1-btn', 'cal-p2-btn'].forEach(function (id) {
        var b = document.getElementById(id);
        if (b) b.disabled = !ready;
    });
    ['offset-ph-btn', 'offset-temp-btn', 'offset-reset-btn'].forEach(function (id) {
        var b = document.getElementById(id);
        if (b) b.disabled = !!sensorsWater.fault;
    });
}

function renderWater(d) {
    if (!d) return;
    sensorsWater = { valid: !!d.valid, fault: !!d.fault, stable: !!d.stable };
    setText('water-ph', fmtNum(d.ph, 2));
    setText('water-temp', fmtNum(d.temperature_c, 1));
    var values = document.getElementById('water-values');
    if (values) values.classList.toggle('stale', !d.valid);
    var fault = document.getElementById('water-fault');
    if (fault) fault.classList.toggle('hidden', !d.fault);
    var badge = document.getElementById('water-stable-badge');
    if (badge) {
        badge.textContent = d.fault ? 'ไม่มีสัญญาณ' : (d.stable ? 'ค่านิ่ง' : 'กำลังนิ่ง...');
        badge.classList.toggle('is-stable', !!d.stable && !d.fault);
    }
    updateCalButtons();
}

function handleWaterSseEvent(data) {
    if (!data) return;
    renderWater({ valid: data.valid, fault: data.fault, stable: data.stable,
                  ph: data.ph, temperature_c: data.temp_c });
}

function renderFlow(on) {
    var el = document.getElementById('flow-state');
    if (!el) return;
    el.textContent = on ? 'ไหล' : 'ไม่ไหล';
    el.classList.toggle('is-stable', !!on);
}

function handleAlarmSseEvent(data) {
    if (!data) return;
    syncAlarmSummary(true);
    if (window.location.pathname === '/alarms') syncAlarmStatus(true);
}

function handleFlowSseEvent(data) {
    if (!data) return;
    renderFlow(!!data.on);
}

function renderSensorsStatus(json) {
    if (!json || json.ok !== true) return;
    if (json.flow) renderFlow(!!json.flow.on);
    (json.power || []).forEach(function (p) { renderPowerChannel(p.channel, p); });
    setSensorNotice('power-notice', json.power_notice);

    var w = json.water || {};
    renderWater(w);
    var off = w.offsets || {};
    setText('offset-ph-current', off.known ? fmtNum(off.ph, 2) : '--');
    setText('offset-temp-current', off.known ? fmtNum(off.temp, 1) : '--');
    var cal = w.calibration || {};
    var text = 'สถานะ: ' + (CAL_STATE_TEXT[cal.state] || cal.state || '--');
    if (cal.error) text += ' — ' + (CAL_ERROR_TEXT[cal.error] || cal.error);
    setText('cal-status', text);
    setSensorNotice('water-notice', w.notice);
}

var sensorsStatusRequestInFlight = false;
var sensorsStatusResyncPending = false;

function syncSensorsStatus(force) {
    if (window.location.pathname !== '/sensors') return;
    if (document.hidden && !force) return;
    /* One request at a time (the device has few sockets). A forced sync that
     * arrives mid-request runs once the current one finishes. */
    if (sensorsStatusRequestInFlight) {
        if (force) sensorsStatusResyncPending = true;
        return;
    }
    sensorsStatusRequestInFlight = true;
    apiGet('/api/sensors/status', function (err, data) {
        sensorsStatusRequestInFlight = false;
        if (sensorsStatusResyncPending) {
            sensorsStatusResyncPending = false;
            setTimeout(function () { syncSensorsStatus(true); }, 0);
        }
        var eyebrow = document.getElementById('sensors-sync-state');
        if (err) {
            if (eyebrow) { eyebrow.textContent = 'ดึงข้อมูลเซนเซอร์ไม่สำเร็จ: ' + err.message; eyebrow.className = 'runtime-eyebrow mb-md error'; }
            return;
        }
        renderSensorsStatus(data);
        if (eyebrow) { eyebrow.textContent = 'อัปเดตล่าสุด ' + new Date().toLocaleTimeString('th-TH'); eyebrow.className = 'runtime-eyebrow mb-md good'; }
    });
}

/* Commands run on the sensor task after the 202; re-read once it has had time. */
function postSensorCommand(url, body, btn) {
    setLoading(btn, true);
    apiPost(url, body, function (err, data) {
        setLoading(btn, false);
        updateCalButtons();
        if (err || !data || data.ok !== true) {
            var code = data && data.error;
            var msg = code === 'queue_full' ? 'เซนเซอร์ไม่ว่าง ลองใหม่อีกครั้ง'
                : code === 'value_out_of_range' ? 'ค่าที่กรอกอยู่นอกช่วงที่อนุญาต'
                : 'ส่งคำสั่งไม่สำเร็จ' + (err ? ': ' + err.message : '');
            showToast(msg, 'error');
            return;
        }
        showToast('ส่งคำสั่งแล้ว', 'success');
        setTimeout(function () { syncSensorsStatus(true); }, 800);
        setTimeout(function () { syncSensorsStatus(true); }, 2500);
    });
}

function readNumberInput(id) {
    var el = document.getElementById(id);
    var v = el ? parseFloat(el.value) : NaN;
    return isFinite(v) ? v : null;
}

function wireSensorsActions() {
    if (sensorsWired) return;
    sensorsWired = true;

    document.querySelectorAll('[data-reset-energy]').forEach(function (btn) {
        btn.addEventListener('click', function () {
            var ch = parseInt(btn.getAttribute('data-reset-energy'), 10);
            showConfirmModal('รีเซ็ตพลังงานสะสม', 'รีเซ็ต kWh ของปั๊ม ' + (ch + 1) + ' เป็น 0? ย้อนกลับไม่ได้', function () {
                postSensorCommand('/api/power/reset-energy', { channel: ch }, btn);
            }, true);
        });
    });

    [1, 2].forEach(function (point) {
        var btn = document.getElementById('cal-p' + point + '-btn');
        if (!btn) return;
        btn.addEventListener('click', function () {
            var v = readNumberInput('cal-p' + point + '-value');
            if (v === null) { showToast('กรอกค่า pH ของน้ำยาบัฟเฟอร์', 'error'); return; }
            showConfirmModal('คาลิเบรต pH', 'เขียนค่าคาลิเบรตจุดที่ ' + point + ' = ' + v.toFixed(2) + ' ลงเซนเซอร์?', function () {
                postSensorCommand('/api/water/calibrate', { point: point, buffer_ph: v }, btn);
            });
        });
    });

    var cancelBtn = document.getElementById('cal-cancel-btn');
    if (cancelBtn) cancelBtn.addEventListener('click', function () {
        postSensorCommand('/api/water/calibrate/cancel', {}, cancelBtn);
    });

    function wireOffset(btnId, inputId, key, digits, label) {
        var btn = document.getElementById(btnId);
        if (!btn) return;
        btn.addEventListener('click', function () {
            var v = readNumberInput(inputId);
            if (v === null) { showToast('กรอกค่า offset', 'error'); return; }
            var body = {}; body[key] = v;
            showConfirmModal('ตั้งค่า offset', 'เขียน ' + label + ' offset = ' + (v >= 0 ? '+' : '') + v.toFixed(digits) + ' ลงเซนเซอร์?', function () {
                postSensorCommand('/api/water/offset', body, btn);
            });
        });
    }
    wireOffset('offset-ph-btn', 'offset-ph-input', 'ph_offset', 2, 'pH');
    wireOffset('offset-temp-btn', 'offset-temp-input', 'temp_offset', 1, 'อุณหภูมิ');

    var resetBtn = document.getElementById('offset-reset-btn');
    if (resetBtn) resetBtn.addEventListener('click', function () {
        showConfirmModal('รีเซ็ต offset', 'ตั้ง pH offset และ offset อุณหภูมิ เป็น 0?', function () {
            postSensorCommand('/api/water/offset', { ph_offset: 0 }, resetBtn);
            setTimeout(function () {
                postSensorCommand('/api/water/offset', { temp_offset: 0 }, resetBtn);
            }, 300);
        });
    });
}

function initSensorsPage() {
    if (window.location.pathname !== '/sensors') return;
    wireSensorsActions();
    updateCalButtons();
    syncSensorsStatus(true);
    sensorsPollTimer = setInterval(function () { syncSensorsStatus(false); }, SENSORS_POLL_MS);
    initTelemetrySSE();
}

function renderFixedBusPins(list) {
    var box = document.getElementById('hw-fixed-bus-pins');
    if (!box || !list) return;
    box.textContent = '';
    list.forEach(function (b) {
        var row = document.createElement('div');
        row.className = 'fixed-bus-row';
        var name = document.createElement('span');
        name.textContent = b.label;
        var pins = document.createElement('strong');
        pins.className = 'mono';
        pins.textContent = 'UART' + b.uart + ' · TX GPIO ' + b.tx + ' · RX GPIO ' + b.rx + ' · ' + b.baud + ' bps';
        row.appendChild(name);
        row.appendChild(pins);
        box.appendChild(row);
    });
}

function renderFixedInputs(list) {
    var box = document.getElementById('hw-fixed-inputs');
    if (!box || !list) return;
    box.textContent = '';
    list.forEach(function (b) {
        var row = document.createElement('div');
        row.className = 'fixed-bus-row';
        var name = document.createElement('span');
        name.textContent = b.label;
        var pins = document.createElement('strong');
        pins.className = 'mono';
        pins.textContent = b.wiring;
        row.appendChild(name);
        row.appendChild(pins);
        box.appendChild(row);
    });
}

function handleCoolingSseEvent(data) {
    if (!data) return;
    var mappedStatus = {
        temperature_c: data.temp_c,
        temperature_valid: !!data.valid,
        sensor_state: data.valid ? 'ok' : 'fault',
        fault: !data.valid,
        fault_code: data.valid ? 'none' : 'read_failed',
        output_volts: typeof data.output_volts === 'number' ? data.output_volts : (data.output_mv / 1000.0),
        output_mv: data.output_mv || Math.round(data.output_volts * 1000),
        duty_percent: typeof data.duty_percent === 'number' ? data.duty_percent : 0,
        lockout_remaining_sec: data.lockout_sec || 0,
        lockout_active: (data.lockout_sec > 0),
        mode: data.mode === 0 ? 'auto' : (data.mode === 2 ? 'test_on' : 'force_off'),
        config_valid: true
    };
    applyCoolingStatus(mappedStatus, true);
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

function computeCoolingStatusIndicator(status) {
    if (!status) return { text: 'STANDBY', cls: 'status-standby' };
    if (status.fault || status.sensor_state === 'fault' || status.temperature_valid === false) {
        return { text: 'FAULT', cls: 'status-fault' };
    }
    if (status.lockout_active || (status.lockout_remaining_sec > 0)) {
        return { text: 'LOCKOUT', cls: 'status-lockout' };
    }
    var volts = Number(status.output_volts) || 0;
    if (volts > 0.05 || (status.duty_percent && status.duty_percent > 0.5)) {
        return { text: 'ACTIVE', cls: 'status-active' };
    }
    return { text: 'STANDBY', cls: 'status-standby' };
}

function applyCoolingStatus(status, authoritative) {
    coolingLastStatus = status;
    if (authoritative) coolingLastSyncMs = Date.now();

    // 1. PT100 Temperature readout
    setHtml('cooling-temperature', renderCoolingTemperature(status));

    // 2. Inverter Voltage and Duty
    var volts = typeof status.output_volts === 'number' ? status.output_volts : (status.output_mv ? status.output_mv / 1000.0 : 0);
    var duty = typeof status.duty_percent === 'number' ? status.duty_percent : (volts / 10.0 * 100);
    setText('cooling-voltage-val', volts.toFixed(2) + ' V');
    setText('cooling-duty-val', duty.toFixed(1) + '%');

    // 3. 0-10V Output Level Progress Bar / Gauge
    var gaugePct = Math.max(0, Math.min(100, (volts / 10.0) * 100));
    var gaugeFill = pumpEl('cooling-gauge-fill');
    if (gaugeFill) gaugeFill.style.width = gaugePct.toFixed(1) + '%';
    setText('cooling-gauge-current-label', volts.toFixed(2) + ' V (' + duty.toFixed(1) + '%)');
    var gaugeBar = pumpEl('cooling-gauge-bar');
    if (gaugeBar) gaugeBar.setAttribute('aria-valuenow', volts.toFixed(2));

    // 4. Status Indicator (ACTIVE, STANDBY, LOCKOUT, FAULT)
    var ind = computeCoolingStatusIndicator(status);
    var statusPill = pumpEl('cooling-status-pill');
    if (statusPill) {
        statusPill.className = 'cooling-status-pill ' + ind.cls;
        statusPill.textContent = ind.text;
    }

    // 5. Badges
    var sensorOk = status.sensor_state === 'ok' && status.temperature_valid !== false;
    updateBadgeCardState('cooling-sensor-card', sensorOk);
    setHtml('cooling-sensor-state', sensorOk ? '<span class="status-success">ปกติ</span>' : '<span class="status-danger">FAULT</span>');

    var inverterActive = (volts > 0.05);
    updateBadgeCardState('cooling-inverter-card', inverterActive);
    setText('cooling-inverter-state', volts.toFixed(2) + ' V (' + duty.toFixed(0) + '%)');

    setHtml('cooling-fault-state', renderCoolingFault(status));

    var lockoutTestStr = '--';
    if (status.test_remaining_sec > 0) {
        lockoutTestStr = 'TEST ' + formatPumpCountdown(status.test_remaining_sec);
    } else if (status.lockout_active || (status.lockout_remaining_sec > 0)) {
        lockoutTestStr = 'LOCK ' + formatPumpCountdown(status.lockout_remaining_sec);
    }
    setText('cooling-lockout-test-state', lockoutTestStr);
    setText('cooling-lockout', status.lockout_active ? formatPumpCountdown(status.lockout_remaining_sec) : '--');
    setText('cooling-test-remaining', status.test_remaining_sec ? formatPumpCountdown(status.test_remaining_sec) : '--');

    setHtml('cooling-mode-state', renderCoolingMode(status.mode));

    // Sync state
    var sync = pumpEl('cooling-sync-state');
    if (sync) {
        sync.className = status.fault || status.sensor_state === 'fault'
            ? 'runtime-eyebrow error'
            : (status.lockout_active || status.mode === 'test_on' ? 'runtime-eyebrow warn' : 'runtime-eyebrow good');
        sync.innerHTML = status.fault || status.sensor_state === 'fault'
            ? renderSvgIcon('icon-x-circle') + ' Sensor Fault'
            : (status.lockout_active ? renderSvgIcon('icon-timer') + ' Compressor Lockout' : renderSvgIcon('icon-check-circle') + ' Synced');
    }

    setPumpAlert('cooling-status-error', status.fault ? 'พบ sensor fault ระบบบังคับปิดการทำงานแล้ว' : '');

    // If modal is open, refresh graph with live temperature marker
    var modal = pumpEl('cooling-rule-modal');
    if (modal && modal.open) {
        refreshCoolingModalPreview();
    }

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
        setPumpAlert('cooling-status-error', 'ไม่สามารถอ่านสถานะ cooling ได้ชั่วคราว');
        setHtml('cooling-temperature', '--');
        setText('cooling-voltage-val', '--');
        setText('cooling-duty-val', '--');
        setText('cooling-gauge-current-label', '--');
        var gaugeFill = pumpEl('cooling-gauge-fill');
        if (gaugeFill) gaugeFill.style.width = '0%';
        var gaugeBar = pumpEl('cooling-gauge-bar');
        if (gaugeBar) gaugeBar.setAttribute('aria-valuenow', '0');
        var pill = pumpEl('cooling-status-pill');
        if (pill) { pill.className = 'cooling-status-pill status-standby'; pill.textContent = 'STANDBY'; }

        updateBadgeCardState('cooling-relay-card', false);
        updateBadgeCardState('cooling-inverter-card', false);
        updateBadgeCardState('cooling-sensor-card', false);

        setHtml('cooling-mode-state', '--');
        setHtml('cooling-sensor-state', '--');
        setHtml('cooling-fault-state', '--');
        setText('cooling-inverter-state', '--');
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
    if (!status || status.temperature_valid === false) return '--';
    var value = Number(status.temperature_c);
    if (!isFinite(value)) return '--';
    return value.toFixed(2) + ' °C';
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
    if (value === 'ok') return '<span class="status-success">ปกติ</span>';
    if (value === 'fault') return '<span class="status-danger">FAULT</span>';
    return 'Unknown';
}

function renderCoolingFault(status) {
    if (!status) return '--';
    if (!status.fault) return '<span class="status-success">ปกติ</span>';
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
    renderFixedBusPins(data.fixed_bus_pins);
    renderFixedInputs(data.fixed_inputs);
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

var uplinkPreviewWired = false;
var uplinkPreviewInFlight = false;

function loadUplinkPreview() {
    if (uplinkPreviewInFlight) return;
    uplinkPreviewInFlight = true;
    var btn = document.getElementById('uplink-preview-refresh');
    if (btn) setLoading(btn, true);
    apiGet('/api/uplink/preview', function (err, data) {
        uplinkPreviewInFlight = false;
        if (btn) setLoading(btn, false);
        if (err || !data || data.ok !== true) {
            setText('uplink-preview-summary', 'โหลดตัวอย่างไม่สำเร็จ' + (err ? ': ' + err.message : ''));
            return;
        }
        var sink = data.sink === 'set' ? 'มีตัวส่งข้อมูล' : 'ยังไม่ได้ต่อตัวส่งข้อมูล (ยังไม่ส่งออก)';
        setText('uplink-preview-summary', sink + ' · รอส่ง ' + data.pending_events + ' เหตุการณ์ · สูญหาย ' + data.dropped + ' · ส่งทุก ' + data.interval_sec + ' วิ');
        var pre = document.getElementById('uplink-preview-json');
        if (pre) pre.textContent = JSON.stringify(data, null, 2);
    });
}

/* The preview loads only when the card is opened or refreshed (it builds ~20 KB of JSON on the board). */
function initUplinkPreview() {
    if (uplinkPreviewWired) return;
    var card = document.getElementById('uplink-preview-card');
    var btn = document.getElementById('uplink-preview-refresh');
    if (!card || !btn) return;
    uplinkPreviewWired = true;
    card.addEventListener('toggle', function () { if (card.open) loadUplinkPreview(); });
    btn.addEventListener('click', loadUplinkPreview);
}

function initStatus() {
    if (window.location.pathname !== '/status') return;
    initUplinkPreview();
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

/* ======== Alarms ======== */

var ALARM_SUMMARY_POLL_MS = 10000;
var ALARM_VALUES_POLL_MS = 2000;
var ALARM_SEEN_KEY = 'fishpump_alarm_seen';
var ALARM_HISTORY_LEN = 32;

var alarmSummaryTimer = null;
var alarmSummaryInFlight = false;
var alarmStatusInFlight = false;
var alarmStatusResync = false;
var alarmLastSummary = null;
var alarmStatus = null;
var alarmSignals = null;
var alarmSignalMap = {};
var alarmRules = [];
var alarmRulesMax = 24;
var alarmValuesTimer = null;
var alarmValuesInFlight = false;
var alarmEditing = null;
var alarmSaving = false;
var alarmWired = false;

var ALARM_GROUP_TEXT = {
    pump_power: 'ไฟฟ้าปั๊มน้ำ',
    chiller_power: 'ไฟฟ้า Chiller',
    water: 'คุณภาพน้ำ',
    cooling: 'ระบบทำความเย็น',
    pump: 'ปั๊ม / รีเลย์ / น้ำไหล',
    float: 'ลูกลอย',
    system: 'ระบบ'
};
var ALARM_COND_TEXT = {
    above: 'มากกว่า',
    below: 'น้อยกว่า',
    outside: 'นอกช่วง',
    inside: 'อยู่ในช่วง',
    is_true: 'เป็นจริง (เปิด)',
    is_false: 'เป็นเท็จ (ปิด)',
    stale: 'ไม่มีข้อมูลใหม่'
};
var ALARM_SEV_TEXT = { info: 'ข้อมูล', warning: 'เตือน', critical: 'วิกฤต' };
var ALARM_TYPE_TEXT = { raised: 'เกิด', cleared: 'หาย', acked: 'รับทราบ' };
var ALARM_FIELD_INPUT = {
    name: 'alarm-f-name', signal: 'alarm-f-signal', condition: 'alarm-f-condition',
    threshold: 'alarm-f-threshold', lo: 'alarm-f-lo', hi: 'alarm-f-hi',
    stale_sec: 'alarm-f-stale', hysteresis: 'alarm-f-hysteresis', delay_sec: 'alarm-f-delay',
    severity: 'alarm-f-severity', gate: 'alarm-f-gate-signal'
};

function alarmReadSeen() {
    try {
        var raw = window.localStorage.getItem(ALARM_SEEN_KEY);
        var v = raw ? JSON.parse(raw) : null;
        if (v && typeof v.boot_id === 'number' && typeof v.seq === 'number') return v;
    } catch (e) { }
    return null;
}

function alarmWriteSeen(bootId, seq) {
    try {
        window.localStorage.setItem(ALARM_SEEN_KEY, JSON.stringify({ boot_id: bootId, seq: seq }));
    } catch (e) { }
}

function alarmEl(id) { return document.getElementById(id); }

function alarmClear(el) {
    while (el && el.firstChild) el.removeChild(el.firstChild);
}

function alarmMake(tag, cls, text) {
    var el = document.createElement(tag);
    if (cls) el.className = cls;
    if (text != null) el.textContent = text;
    return el;
}

function alarmUnseenCount(sum) {
    var seen = alarmReadSeen();
    var n = (seen && seen.boot_id === sum.boot_id) ? sum.last_seq - seen.seq : sum.last_seq;
    if (n < 0) n = sum.last_seq;
    return Math.min(n, ALARM_HISTORY_LEN);
}

function renderAlarmSummary(sum) {
    var s = sum.summary || {};
    var active = s.active || 0;
    var onPage = window.location.pathname === '/alarms';
    if (onPage) alarmWriteSeen(sum.boot_id, sum.last_seq);

    var banner = alarmEl('alarm-banner');
    if (banner) {
        var show = active > 0 && !onPage;
        banner.classList.toggle('hidden', !show);
        banner.classList.toggle('is-critical', (s.critical || 0) > 0);
        if (show) {
            var text = 'มีการแจ้งเตือน ' + active + ' รายการ';
            if (s.critical > 0) text += ' (วิกฤต ' + s.critical + ')';
            setText('alarm-banner-text', text);
        }
    }

    var count = active + (onPage ? 0 : alarmUnseenCount(sum));
    ['alarm-badge', 'm-alarm-badge'].forEach(function (id) {
        var b = alarmEl(id);
        if (!b) return;
        b.textContent = count > 99 ? '99+' : String(count);
        b.classList.toggle('hidden', count === 0);
    });
}

function alarmStatusIsStale(sum) {
    if (!alarmStatus) return true;
    if (alarmStatus.boot_id !== sum.boot_id || alarmStatus.last_seq !== sum.last_seq) return true;
    return JSON.stringify(alarmStatus.summary) !== JSON.stringify(sum.summary);
}

function syncAlarmSummary(force) {
    if (!isAuthenticated()) return;
    if (document.hidden && !force) return;
    if (alarmSummaryInFlight) return;
    alarmSummaryInFlight = true;
    apiGet('/api/alarms/summary', function (err, data) {
        alarmSummaryInFlight = false;
        if (err || !data || data.ok !== true) return;
        alarmLastSummary = data;
        renderAlarmSummary(data);
        /* No SSE slot may be free: the poll alone keeps the Alarms page current. */
        if (window.location.pathname === '/alarms' && alarmStatusIsStale(data)) {
            syncAlarmStatus(true);
        }
    });
}

function startAlarmSummaryPolling() {
    if (alarmSummaryTimer) return;
    syncAlarmSummary(true);
    alarmSummaryTimer = setInterval(function () { syncAlarmSummary(false); }, ALARM_SUMMARY_POLL_MS);
}

function stopAlarmSummaryPolling() {
    if (alarmSummaryTimer) { clearInterval(alarmSummaryTimer); alarmSummaryTimer = null; }
}

function alarmSignal(key) { return alarmSignalMap[key] || null; }

function alarmFormatValue(key, v) {
    var sig = alarmSignal(key);
    if (v == null || typeof v === 'undefined') return '--';
    if (sig && sig.kind === 'bool') return v ? 'เปิด' : 'ปิด';
    if (typeof v !== 'number' || !isFinite(v)) return '--';
    var out = v.toFixed(sig ? sig.decimals : 2);
    if (sig && sig.unit) out += ' ' + sig.unit;
    return out;
}

function alarmAgo(ms) {
    var s = Math.max(0, Math.floor(ms / 1000));
    if (s < 60) return s + ' วินาทีที่แล้ว';
    var m = Math.floor(s / 60);
    if (m < 60) return m + ' นาทีที่แล้ว';
    var h = Math.floor(m / 60);
    if (h < 24) return h + ' ชั่วโมงที่แล้ว';
    return Math.floor(h / 24) + ' วันที่แล้ว';
}

function alarmClock(status, uptimeMs) {
    var d = new Date(Date.now() - (status.uptime_ms - uptimeMs));
    var pad = function (n) { return (n < 10 ? '0' : '') + n; };
    var t = pad(d.getHours()) + ':' + pad(d.getMinutes());
    if (d.toDateString() !== new Date().toDateString()) {
        t = pad(d.getDate()) + '/' + pad(d.getMonth() + 1) + ' ' + t;
    }
    return t;
}

function alarmSevChip(sev) {
    return alarmMake('span', 'alarm-sev sev-' + sev, ALARM_SEV_TEXT[sev] || sev);
}

/* One-line Thai summary of a rule, e.g. "pH นอกช่วง 6.5–8.5 นาน 60 วิ". */
function alarmDescribeRule(r) {
    var sig = alarmSignal(r.signal);
    var label = sig ? sig.label : r.signal;
    var unit = sig && sig.unit ? ' ' + sig.unit : '';
    var text;
    switch (r.condition) {
        case 'above': text = label + ' > ' + r.threshold + unit; break;
        case 'below': text = label + ' < ' + r.threshold + unit; break;
        case 'outside': text = label + ' นอกช่วง ' + r.lo + '–' + r.hi + unit; break;
        case 'inside': text = label + ' อยู่ในช่วง ' + r.lo + '–' + r.hi + unit; break;
        case 'is_true': text = label + ' = เปิด'; break;
        case 'is_false': text = label + ' = ปิด'; break;
        default: text = label + ' ไม่มีข้อมูลใหม่เกิน ' + r.stale_sec + ' วิ'; break;
    }
    if (r.delay_sec > 0) text += ' นาน ' + r.delay_sec + ' วิ';
    if (r.gate) {
        var g = alarmSignal(r.gate.signal);
        text += ' · เฉพาะตอน ' + (g ? g.label : r.gate.signal) + (r.gate.when ? ' เปิด' : ' ปิด');
    }
    if (r.latch) text += ' · ค้างจนรับทราบ';
    return text;
}

function renderAlarmStatus(st) {
    var stalled = alarmEl('alarm-engine-stalled');
    if (stalled) stalled.classList.toggle('hidden', !st.engine_stalled);

    var list = alarmEl('alarm-active-list');
    alarmClear(list);
    var anyUnacked = false;
    (st.active || []).forEach(function (a) {
        var row = alarmMake('div', 'alarm-row sev-' + a.severity);
        row.appendChild(alarmSevChip(a.severity));
        var main = alarmMake('div', 'alarm-main');
        main.appendChild(alarmMake('div', 'alarm-name', a.name));
        var sub = alarmFormatValue(a.signal, a.value) + ' · ' + alarmAgo(st.uptime_ms - a.since_ms);
        if (a.state === 'latched') sub += ' · ผ่านไปแล้ว รอรับทราบ';
        else if (a.acked) sub += ' · รับทราบแล้ว';
        if (a.dry_run && (a.dry_run.state === 'wait' || a.dry_run.state === 'locked')) {
            sub += ' · ' + dryRunStatusText(a.dry_run);
        }
        main.appendChild(alarmMake('div', 'alarm-sub', sub));
        row.appendChild(main);
        if (a.state === 'latched' || !a.acked) {
            anyUnacked = true;
            var btn = alarmMake('button', 'btn btn-outline btn-sm', 'รับทราบ');
            btn.type = 'button';
            btn.addEventListener('click', function () { alarmAck({ id: a.rule_id }, btn); });
            row.appendChild(btn);
        }
        list.appendChild(row);
    });
    if (!(st.active || []).length) {
        list.appendChild(alarmMake('p', 'text-sm text-muted', 'ไม่มีการแจ้งเตือน'));
    }
    var ackAll = alarmEl('alarm-ack-all');
    if (ackAll) ackAll.classList.toggle('hidden', !anyUnacked);

    var hist = alarmEl('alarm-history-list');
    alarmClear(hist);
    (st.history || []).forEach(function (h) {
        var row = alarmMake('div', 'alarm-row');
        row.appendChild(alarmMake('span', 'mono text-sm', alarmClock(st, h.uptime_ms)));
        row.appendChild(alarmMake('span', 'alarm-sev', ALARM_TYPE_TEXT[h.type] || h.type));
        var main = alarmMake('div', 'alarm-main');
        main.appendChild(alarmMake('div', 'alarm-name', h.name));
        main.appendChild(alarmMake('div', 'alarm-sub', alarmFormatValue(h.signal, h.value)));
        row.appendChild(main);
        hist.appendChild(row);
    });
    if (!(st.history || []).length) {
        hist.appendChild(alarmMake('p', 'text-sm text-muted', 'ยังไม่มีประวัติ'));
    }
}

function syncAlarmStatus(force) {
    if (window.location.pathname !== '/alarms') return;
    if (alarmStatusInFlight) {
        if (force) alarmStatusResync = true;
        return;
    }
    alarmStatusInFlight = true;
    apiGet('/api/alarms/status', function (err, data) {
        alarmStatusInFlight = false;
        if (alarmStatusResync) {
            alarmStatusResync = false;
            setTimeout(function () { syncAlarmStatus(true); }, 0);
        }
        if (window.location.pathname !== '/alarms') return;
        var eyebrow = alarmEl('alarms-sync-state');
        if (err || !data || data.ok !== true) {
            if (eyebrow) { eyebrow.textContent = 'ดึงข้อมูลไม่สำเร็จ: ' + (err ? err.message : 'ข้อมูลไม่ถูกต้อง'); eyebrow.className = 'runtime-eyebrow mb-md error'; }
            return;
        }
        alarmStatus = data;
        renderAlarmStatus(data);
        renderAlarmSummary(data);
        if (eyebrow) { eyebrow.textContent = 'อัปเดตล่าสุด ' + new Date().toLocaleTimeString('th-TH'); eyebrow.className = 'runtime-eyebrow mb-md good'; }
    });
}

function alarmAck(body, btn) {
    if (btn) setLoading(btn, true);
    apiPost('/api/alarms/ack', body, function (err, data) {
        if (btn) setLoading(btn, false);
        if (err || !data || data.ok !== true) {
            showToast('รับทราบไม่สำเร็จ' + (err ? ': ' + err.message : ''), 'error');
        }
        syncAlarmSummary(true);
        syncAlarmStatus(true);
    });
}

/* ---- Rules list ---- */

function alarmLoadRules() {
    apiGet('/api/alarms/rules', function (err, data) {
        if (err || !data || data.ok !== true) {
            alarmClear(alarmEl('alarm-rule-list'));
            alarmEl('alarm-rule-list').appendChild(alarmMake('p', 'text-sm text-muted', 'โหลดกฎไม่สำเร็จ'));
            return;
        }
        alarmRules = data.rules || [];
        alarmRulesMax = data.max || 24;
        renderAlarmRules(data.storage);
    });
}

function renderAlarmRules(storage) {
    setText('alarm-rule-count', alarmRules.length + '/' + alarmRulesMax);
    var add = alarmEl('alarm-add-rule');
    if (add) add.disabled = alarmRules.length >= alarmRulesMax;
    var note = alarmEl('alarm-storage-note');
    if (note) {
        var msg = storage === 'invalid' ? 'ข้อมูลกฎที่บันทึกไว้ใช้ไม่ได้ ระบบใช้กฎตั้งต้นแทน' : '';
        note.textContent = msg;
        note.classList.toggle('hidden', !msg);
    }
    var list = alarmEl('alarm-rule-list');
    alarmClear(list);
    alarmRules.forEach(function (r) {
        var row = alarmMake('div', 'alarm-row' + (r.enabled ? '' : ' is-off'));
        var toggle = document.createElement('input');
        toggle.type = 'checkbox';
        toggle.checked = !!r.enabled;
        toggle.setAttribute('aria-label', 'เปิดใช้งาน ' + r.name);
        toggle.addEventListener('change', function () {
            var copy = JSON.parse(JSON.stringify(r));
            copy.enabled = toggle.checked;
            toggle.disabled = true;
            alarmSaveRule(copy, function (ok) {
                toggle.disabled = false;
                if (!ok) toggle.checked = !!r.enabled;
            });
        });
        row.appendChild(toggle);
        row.appendChild(alarmSevChip(r.severity));
        var main = alarmMake('div', 'alarm-main');
        main.appendChild(alarmMake('div', 'alarm-name', r.name));
        main.appendChild(alarmMake('div', 'alarm-sub', alarmDescribeRule(r)));
        row.appendChild(main);
        var actions = alarmMake('div', 'alarm-actions');
        var edit = alarmMake('button', 'btn btn-outline btn-sm', 'แก้');
        edit.type = 'button';
        edit.addEventListener('click', function () { openAlarmDialog(r); });
        var del = alarmMake('button', 'btn btn-outline btn-sm', 'ลบ');
        del.type = 'button';
        del.addEventListener('click', function () {
            showConfirmModal('ลบกฎแจ้งเตือน', 'ลบกฎ "' + r.name + '" ใช่หรือไม่?', function () {
                apiPost('/api/alarms/rules/delete', { id: r.id }, function (err, data) {
                    if (err || !data || data.ok !== true) {
                        showToast('ลบไม่สำเร็จ' + (err ? ': ' + err.message : ''), 'error');
                    }
                    alarmLoadRules();
                    syncAlarmSummary(true);
                    syncAlarmStatus(true);
                });
            }, true);
        });
        actions.appendChild(edit);
        actions.appendChild(del);
        row.appendChild(actions);
        list.appendChild(row);
    });
    if (!alarmRules.length) {
        list.appendChild(alarmMake('p', 'text-sm text-muted', 'ยังไม่มีกฎ กด "เพิ่มกฎ" เพื่อเริ่ม'));
    }
}

/* POSTs one whole rule (create without id, replace with id). cb(ok, data). */
function alarmSaveRule(rule, cb) {
    apiPost('/api/alarms/rules', rule, function (err, data) {
        var ok = !err && data && data.ok === true;
        if (!ok && !alarmEl('alarm-rule-dialog').open) {
            showToast('บันทึกไม่สำเร็จ' + (err ? ': ' + err.message : (data && data.message ? ': ' + data.message : '')), 'error');
        }
        alarmLoadRules();
        syncAlarmSummary(true);
        syncAlarmStatus(true);
        if (cb) cb(ok, data, err);
    });
}

/* ---- Rule dialog ---- */

function alarmFillSelect(sel, items, selected) {
    alarmClear(sel);
    items.forEach(function (it) {
        var o = document.createElement('option');
        o.value = it.value;
        o.textContent = it.text;
        sel.appendChild(o);
    });
    if (selected != null) sel.value = selected;
}

function alarmFillSignalSelects() {
    var sig = alarmEl('alarm-f-signal');
    var gate = alarmEl('alarm-f-gate-signal');
    alarmClear(sig);
    alarmClear(gate);
    var none = document.createElement('option');
    none.value = '';
    none.textContent = '(ทุกเวลา)';
    gate.appendChild(none);
    var groups = {};
    var order = [];
    (alarmSignals || []).forEach(function (s) {
        if (!groups[s.group]) { groups[s.group] = []; order.push(s.group); }
        groups[s.group].push(s);
    });
    order.forEach(function (g) {
        var og = document.createElement('optgroup');
        og.label = ALARM_GROUP_TEXT[g] || g;
        var og2 = document.createElement('optgroup');
        og2.label = og.label;
        groups[g].forEach(function (s) {
            var o = document.createElement('option');
            o.value = s.key;
            o.textContent = s.label;
            og.appendChild(o);
            if (s.kind === 'bool') {
                var o2 = document.createElement('option');
                o2.value = s.key;
                o2.textContent = s.label;
                og2.appendChild(o2);
            }
        });
        sig.appendChild(og);
        if (og2.children.length) gate.appendChild(og2);
    });
}

/* Conditions valid for the selected signal (kind and period). */
function alarmUpdateConditions(selectedCond) {
    var s = alarmSignal(alarmEl('alarm-f-signal').value);
    var conds = [];
    if (s) {
        if (s.kind === 'bool') conds = ['is_true', 'is_false'];
        else conds = ['above', 'below', 'outside', 'inside'];
        if (s.period_sec > 0) conds.push('stale');
    }
    var sel = alarmEl('alarm-f-condition');
    var prev = selectedCond || sel.value;
    alarmFillSelect(sel, conds.map(function (c) { return { value: c, text: ALARM_COND_TEXT[c] }; }));
    if (conds.indexOf(prev) >= 0) sel.value = prev;
    alarmUpdateFields();
}

function alarmUpdateFields() {
    var s = alarmSignal(alarmEl('alarm-f-signal').value);
    var cond = alarmEl('alarm-f-condition').value;
    var dlg = alarmEl('alarm-rule-dialog');
    var unit = s && s.unit ? '(' + s.unit + ')' : '';
    var show = {
        threshold: cond === 'above' || cond === 'below',
        range: cond === 'outside' || cond === 'inside',
        stale: cond === 'stale',
        hysteresis: cond === 'above' || cond === 'below' || cond === 'outside' || cond === 'inside'
    };
    Array.prototype.forEach.call(dlg.querySelectorAll('[data-for]'), function (el) {
        el.classList.toggle('hidden', !show[el.getAttribute('data-for')]);
    });
    Array.prototype.forEach.call(dlg.querySelectorAll('[data-unit]'), function (el) { el.textContent = unit; });
    var stale = alarmEl('alarm-f-stale');
    if (s && cond === 'stale') stale.min = String(Math.max(5, 3 * s.period_sec));
    alarmUpdateGateWhen();
}

function alarmUpdateGateWhen() {
    alarmEl('alarm-f-gate-when').disabled = !alarmEl('alarm-f-gate-signal').value;
}

function alarmShowError(field, message) {
    var id = ALARM_FIELD_INPUT[field];
    var input = id ? alarmEl(id) : null;
    if (input) { input.classList.add('field-error'); try { input.focus(); } catch (e) { } }
    var box = alarmEl('alarm-f-error');
    var names = { name: 'ชื่อกฎ', signal: 'ค่าที่ตรวจ', condition: 'เงื่อนไข', threshold: 'ค่าที่กำหนด', lo: 'ค่าต่ำสุด', hi: 'ค่าสูงสุด',
        stale_sec: 'เวลาไม่มีข้อมูลใหม่', hysteresis: 'ช่วงกันสั่น', delay_sec: 'เวลาที่ต้องเกิน', severity: 'ระดับ', gate: 'เงื่อนไขเฉพาะตอน' };
    box.textContent = message || ('ค่าในช่อง "' + (names[field] || field) + '" ไม่ถูกต้อง');
    box.classList.remove('hidden');
}

function alarmClearErrors() {
    var dlg = alarmEl('alarm-rule-dialog');
    Array.prototype.forEach.call(dlg.querySelectorAll('.field-error'), function (el) { el.classList.remove('field-error'); });
    alarmEl('alarm-f-error').classList.add('hidden');
}

function alarmNumber(id, field) {
    var el = alarmEl(id);
    var v = el.value.trim();
    var n = v === '' ? NaN : Number(v);
    if (!isFinite(n)) { alarmShowError(field); return null; }
    return n;
}

/* Builds the rule JSON from the form; shows the field error and returns null on bad input. */
function alarmCollectRule() {
    alarmClearErrors();
    var name = alarmEl('alarm-f-name').value.trim();
    if (!name) { alarmShowError('name', 'กรุณาใส่ชื่อกฎ'); return null; }
    var rule = {
        name: name,
        enabled: alarmEditing ? !!alarmEditing.enabled : true,
        signal: alarmEl('alarm-f-signal').value,
        condition: alarmEl('alarm-f-condition').value,
        severity: alarmEl('alarm-f-severity').value,
        latch: alarmEl('alarm-f-latch').checked
    };
    if (alarmEditing) rule.id = alarmEditing.id;
    if (!rule.signal) { alarmShowError('signal'); return null; }
    if (!rule.condition) { alarmShowError('condition'); return null; }
    var n;
    if (rule.condition === 'above' || rule.condition === 'below') {
        if ((n = alarmNumber('alarm-f-threshold', 'threshold')) === null) return null;
        rule.threshold = n;
    } else if (rule.condition === 'outside' || rule.condition === 'inside') {
        if ((n = alarmNumber('alarm-f-lo', 'lo')) === null) return null;
        rule.lo = n;
        if ((n = alarmNumber('alarm-f-hi', 'hi')) === null) return null;
        rule.hi = n;
        if (rule.lo >= rule.hi) { alarmShowError('hi', 'ค่าสูงสุดต้องมากกว่าค่าต่ำสุด'); return null; }
    } else if (rule.condition === 'stale') {
        if ((n = alarmNumber('alarm-f-stale', 'stale_sec')) === null) return null;
        rule.stale_sec = n;
    }
    if (['above', 'below', 'outside', 'inside'].indexOf(rule.condition) >= 0) {
        if ((n = alarmNumber('alarm-f-hysteresis', 'hysteresis')) === null) return null;
        rule.hysteresis = n;
    }
    if ((n = alarmNumber('alarm-f-delay', 'delay_sec')) === null) return null;
    rule.delay_sec = n;
    var gateSig = alarmEl('alarm-f-gate-signal').value;
    if (gateSig) rule.gate = { signal: gateSig, when: alarmEl('alarm-f-gate-when').value === 'true' };
    return rule;
}

function alarmPollValues() {
    if (alarmValuesInFlight || !alarmEl('alarm-rule-dialog').open) return;
    alarmValuesInFlight = true;
    apiGet('/api/alarms/values', function (err, data) {
        alarmValuesInFlight = false;
        if (err || !data || data.ok !== true || !alarmEl('alarm-rule-dialog').open) return;
        var key = alarmEl('alarm-f-signal').value;
        var v = data.values ? data.values[key] : null;
        var text = v ? alarmFormatValue(key, v.v) : '--';
        if (v && v.v != null && !v.trusted) text += ' (ยังไม่น่าเชื่อถือ)';
        setText('alarm-f-live', text);
    });
}

function openAlarmDialog(rule) {
    if (!alarmSignals) {
        showToast('ยังโหลดรายการสัญญาณไม่เสร็จ ลองใหม่อีกครั้ง', 'error');
        return;
    }
    alarmEditing = rule || null;
    alarmClearErrors();
    setText('alarm-dialog-title', rule ? 'แก้ไขกฎแจ้งเตือน' : 'เพิ่มกฎแจ้งเตือน');
    alarmEl('alarm-f-name').value = rule ? rule.name : '';
    var firstSignal = alarmSignals.length ? alarmSignals[0].key : '';
    alarmEl('alarm-f-signal').value = rule ? rule.signal : firstSignal;
    alarmUpdateConditions(rule ? rule.condition : null);
    alarmEl('alarm-f-threshold').value = rule && rule.threshold != null ? rule.threshold : '';
    alarmEl('alarm-f-lo').value = rule && rule.lo != null ? rule.lo : '';
    alarmEl('alarm-f-hi').value = rule && rule.hi != null ? rule.hi : '';
    alarmEl('alarm-f-stale').value = rule && rule.stale_sec != null ? rule.stale_sec : '30';
    alarmEl('alarm-f-hysteresis').value = rule && rule.hysteresis != null ? rule.hysteresis : '0';
    alarmEl('alarm-f-delay').value = rule && rule.delay_sec != null ? rule.delay_sec : '0';
    alarmEl('alarm-f-severity').value = rule ? rule.severity : 'warning';
    alarmEl('alarm-f-latch').checked = !!(rule && rule.latch);
    alarmEl('alarm-f-gate-signal').value = rule && rule.gate ? rule.gate.signal : '';
    alarmEl('alarm-f-gate-when').value = rule && rule.gate && !rule.gate.when ? 'false' : 'true';
    alarmUpdateGateWhen();
    setText('alarm-f-live', '--');
    setLoading('alarm-f-save', false);
    alarmSaving = false;
    var dlg = alarmEl('alarm-rule-dialog');
    if (!dlg.open) dlg.showModal();
    alarmPollValues();
    if (alarmValuesTimer) clearInterval(alarmValuesTimer);
    alarmValuesTimer = setInterval(alarmPollValues, ALARM_VALUES_POLL_MS);
}

function closeAlarmDialog() {
    if (alarmValuesTimer) { clearInterval(alarmValuesTimer); alarmValuesTimer = null; }
    var dlg = alarmEl('alarm-rule-dialog');
    if (dlg && dlg.open) dlg.close();
}

function alarmSubmitDialog() {
    if (alarmSaving) return;
    var rule = alarmCollectRule();
    if (!rule) return;
    alarmSaving = true;
    setLoading('alarm-f-save', true);
    alarmSaveRule(rule, function (ok, data, err) {
        alarmSaving = false;
        setLoading('alarm-f-save', false);
        if (ok) {
            closeAlarmDialog();
            showToast('บันทึกกฎแล้ว', 'success');
            return;
        }
        if (err) { alarmShowError('', 'บันทึกไม่สำเร็จ: ' + err.message); return; }
        var code = data && data.error;
        var msg = data && data.message;
        if (code === 'rules_full') alarmShowError('', 'มีกฎครบ ' + alarmRulesMax + ' ข้อแล้ว ลบกฎเก่าก่อน');
        else if (code === 'out_of_memory') alarmShowError('', 'หน่วยความจำไม่พอ ลองใหม่อีกครั้ง');
        else if (code === 'not_found') alarmShowError('', 'ไม่พบกฎนี้ (อาจถูกลบไปแล้ว)');
        else if (code === 'storage_failed') alarmShowError('', 'บันทึกลงหน่วยความจำไม่สำเร็จ');
        else if (ALARM_FIELD_INPUT[msg]) alarmShowError(msg);
        else alarmShowError('', 'บันทึกไม่สำเร็จ' + (msg ? ': ' + msg : ''));
    });
}

function alarmWire() {
    if (alarmWired) return;
    alarmWired = true;
    alarmEl('alarm-add-rule').addEventListener('click', function () { openAlarmDialog(null); });
    alarmEl('alarm-ack-all').addEventListener('click', function () {
        showConfirmModal('รับทราบทั้งหมด', 'รับทราบการแจ้งเตือนทั้งหมดที่กำลังเกิดอยู่?', function () {
            alarmAck({ all: true }, null);
        }, false);
    });
    alarmEl('alarm-f-signal').addEventListener('change', function () { alarmUpdateConditions(null); setText('alarm-f-live', '--'); alarmPollValues(); });
    alarmEl('alarm-f-condition').addEventListener('change', alarmUpdateFields);
    alarmEl('alarm-f-gate-signal').addEventListener('change', alarmUpdateGateWhen);
    alarmEl('alarm-f-save').addEventListener('click', alarmSubmitDialog);
    alarmEl('alarm-f-cancel').addEventListener('click', closeAlarmDialog);
    alarmEl('alarm-rule-dialog').addEventListener('close', function () {
        if (alarmValuesTimer) { clearInterval(alarmValuesTimer); alarmValuesTimer = null; }
    });
}

function initAlarmsPage() {
    if (window.location.pathname !== '/alarms') return;
    alarmWire();
    alarmStatus = null;
    syncAlarmStatus(true);
    alarmLoadRules();
    if (!alarmSignals) {
        apiGet('/api/alarms/signals', function (err, data) {
            if (err || !data || data.ok !== true) return;
            alarmSignals = data.signals || [];
            alarmSignalMap = {};
            alarmSignals.forEach(function (s) { alarmSignalMap[s.key] = s; });
            alarmFillSignalSelects();
            alarmLoadRules();
            if (alarmStatus) renderAlarmStatus(alarmStatus);
        });
    }
    initTelemetrySSE();
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
    else if (path === '/sensors') viewId = 'view-sensors';
    else if (path === '/alarms') viewId = 'view-alarms';
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

    /* The alarm summary poll runs on every logged-in view and is not stopped by cleanupCurrentView(). */
    if (path === '/login' || !isAuthenticated()) {
        stopAlarmSummaryPolling();
    } else {
        startAlarmSummaryPolling();
        if (alarmLastSummary) renderAlarmSummary(alarmLastSummary);
    }

    if (path === '/login') {
        if (typeof initLogin === 'function') initLogin();
    } else if (path === '/dashboard') {
        if (typeof initDashboard === 'function') initDashboard();
    } else if (path === '/cooling') {
        if (typeof initCoolingDashboard === 'function') initCoolingDashboard();
    } else if (path === '/sensors') {
        if (typeof initSensorsPage === 'function') initSensorsPage();
    } else if (path === '/alarms') {
        if (typeof initAlarmsPage === 'function') initAlarmsPage();
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

