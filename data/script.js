const $=s=>document.querySelector(s),$$=s=>document.querySelectorAll(s);

// Poll status every 2 seconds (faster for NFC detection)
setInterval(fetchStatus, 2000);
fetchStatus();

async function fetchStatus() {
    try {
        const r = await fetch('/api/status');
        const d = await r.json();
        update(d);
    } catch(e) {
        console.error(e);
    }
}

function update(d) {
    // WiFi status
    if (d.wifi) {
        $('#wifi-dot').classList.toggle('on', d.wifi.connected || d.wifi.ap_mode);
        if (d.wifi.ssid) $('#cur-ssid').textContent = d.wifi.ssid;
        if (d.wifi.ip) $('#cur-ip').textContent = d.wifi.ip;
        if (d.wifi.rssi) $('#rssi').textContent = d.wifi.rssi;
    }

    // MQTT status
    if (d.mqtt) {
        $('#mqtt-dot').classList.toggle('on', d.mqtt.connected);
        if (d.mqtt.host) {
            $('#m-host').value = d.mqtt.host;
            $('#m-port').value = d.mqtt.port;
        }
    }

    // NFC status
    if (d.nfc) {
        // Reader connected status
        const nfcDot = $('#nfc-dot');
        if (d.nfc.connected) {
            nfcDot.classList.add('on');
            nfcDot.classList.remove('scanning');
        } else {
            nfcDot.classList.remove('on');
        }

        // Tag present indicator
        const tagDot = $('#tag-present-dot');
        tagDot.classList.toggle('on', d.nfc.tag_present);

        // Last UID
        const lastUid = d.nfc.last_uid || '--';
        $('#last-uid').textContent = lastUid;

        // Time since scan
        if (d.nfc.time_since_scan > 0) {
            const secs = d.nfc.time_since_scan;
            if (secs < 60) {
                $('#scan-time').textContent = `Last scan: ${secs} seconds ago`;
            } else if (secs < 3600) {
                $('#scan-time').textContent = `Last scan: ${Math.floor(secs/60)} minutes ago`;
            } else {
                $('#scan-time').textContent = `Last scan: ${Math.floor(secs/3600)} hours ago`;
            }
        } else {
            $('#scan-time').textContent = '';
        }
    }

    // Device info
    if (d.device) {
        if (d.device.mac) $('#mac').textContent = d.device.mac;
        if (d.device.name) $('#device-name').placeholder = d.device.name;
        if (d.device.version) {
            $('#footer-version').textContent = 'v' + d.device.version;
        }
        // Night mode status
        const nightToggle = $('#night-mode-toggle');
        const nightStatus = $('#night-mode-status');
        if (nightToggle && !nightToggle._userInteracting) {
            nightToggle.checked = d.device.night_mode === true;
            nightStatus.textContent = d.device.night_mode ? 'On' : 'Off';
            nightStatus.classList.toggle('active', d.device.night_mode);
        }
    }
}

// Copy UID button (with HTTP fallback since clipboard API requires HTTPS)
$('#copy-uid').onclick = async () => {
    const uid = $('#last-uid').textContent;
    if (uid && uid !== '--') {
        let success = false;

        // Try modern clipboard API first (works on HTTPS/localhost)
        if (navigator.clipboard && navigator.clipboard.writeText) {
            try {
                await navigator.clipboard.writeText(uid);
                success = true;
            } catch(e) {
                console.log('Clipboard API failed, trying fallback');
            }
        }

        // Fallback for HTTP: use textarea + execCommand
        if (!success) {
            const textarea = document.createElement('textarea');
            textarea.value = uid;
            textarea.style.position = 'fixed';
            textarea.style.opacity = '0';
            document.body.appendChild(textarea);
            textarea.select();
            try {
                success = document.execCommand('copy');
            } catch(e) {
                console.error('execCommand failed:', e);
            }
            document.body.removeChild(textarea);
        }

        const btn = $('#copy-uid');
        if (success) {
            btn.textContent = 'Copied!';
            btn.classList.add('copied');
            setTimeout(() => {
                btn.textContent = 'Copy';
                btn.classList.remove('copied');
            }, 2000);
        } else {
            btn.textContent = 'Failed';
            setTimeout(() => { btn.textContent = 'Copy'; }, 2000);
        }
    }
};

// Scan history
async function fetchScanHistory() {
    try {
        const r = await fetch('/api/scans');
        const d = await r.json();
        renderScanHistory(d.scans || []);
    } catch(e) {
        console.error(e);
        $('#scan-history').innerHTML = '<div class="no-scans">Error loading history</div>';
    }
}

function renderScanHistory(scans) {
    const container = $('#scan-history');
    if (!scans || scans.length === 0) {
        container.innerHTML = '<div class="no-scans">No scans yet</div>';
        return;
    }

    const html = scans.map(scan => {
        let timeStr;
        if (scan.ago < 60) {
            timeStr = `${scan.ago}s ago`;
        } else if (scan.ago < 3600) {
            timeStr = `${Math.floor(scan.ago/60)}m ago`;
        } else {
            timeStr = `${Math.floor(scan.ago/3600)}h ago`;
        }
        return `<div class="scan-item">
            <span class="uid">${scan.uid}</span>
            <span class="time">${timeStr}</span>
        </div>`;
    }).join('');

    container.innerHTML = html;
}

// Load history when section is opened
$('#history-section')?.addEventListener('toggle', function() {
    if (this.open) fetchScanHistory();
});

// Clear history
$('#clear-history').onclick = async () => {
    if (confirm('Clear scan history?')) {
        try {
            await fetch('/api/scans', { method: 'DELETE' });
            fetchScanHistory();
        } catch(e) {
            console.error(e);
        }
    }
};

// WiFi form
$('#wifi-form').onsubmit = async e => {
    e.preventDefault();
    try {
        const r = await fetch('/api/wifi', {
            method: 'POST',
            body: new URLSearchParams({
                ssid: $('#w-ssid').value,
                password: $('#w-pass').value
            })
        });
        const d = await r.json();
        alert(d.message || 'Saved');
    } catch(e) {
        alert('Error');
    }
};

// MQTT form
$('#mqtt-form').onsubmit = async e => {
    e.preventDefault();
    try {
        const r = await fetch('/api/mqtt', {
            method: 'POST',
            body: new URLSearchParams({
                host: $('#m-host').value,
                port: $('#m-port').value,
                user: $('#m-user').value,
                password: $('#m-pass').value
            })
        });
        const d = await r.json();
        alert(d.message || 'Saved');
    } catch(e) {
        alert('Error');
    }
};

// Device settings form
$('#device-form').onsubmit = async e => {
    e.preventDefault();
    const name = $('#device-name').value.trim();
    if (!name) {
        alert('Please enter a device name');
        return;
    }
    try {
        const r = await fetch('/api/device', {
            method: 'POST',
            body: new URLSearchParams({ name: name })
        });
        const d = await r.json();
        if (d.success) {
            alert('Device name saved. Restart device to update MQTT discovery.');
            $('#device-name').value = '';
            $('#device-name').placeholder = name;
        } else {
            alert(d.error || 'Error saving device name');
        }
    } catch(e) {
        alert('Error');
    }
};

// Factory reset
$('#reset').onclick = async () => {
    if (confirm('Reset all settings? This cannot be undone.')) {
        await fetch('/api/reset', { method: 'POST' });
        alert('Resetting...');
    }
};

// Password settings
async function fetchPasswords() {
    try {
        const r = await fetch('/api/passwords');
        const d = await r.json();
        $('#ota-status').textContent = d.ota_custom ? '(custom set)' : '(using default)';
        $('#ap-status').textContent = d.ap_custom ? '(custom set)' : '(using default)';
    } catch(e) {
        console.error(e);
    }
}
fetchPasswords();

$('#pass-form').onsubmit = async e => {
    e.preventDefault();
    const otaPass = $('#p-ota').value;
    const apPass = $('#p-ap').value;

    if (!otaPass && !apPass) {
        alert('Enter at least one password to change');
        return;
    }

    const params = {};
    if (otaPass) params.ota_password = otaPass;
    if (apPass) params.ap_password = apPass;

    try {
        const r = await fetch('/api/passwords', {
            method: 'POST',
            body: new URLSearchParams(params)
        });
        const d = await r.json();
        if (d.success) {
            alert(d.message);
            $('#p-ota').value = '';
            $('#p-ap').value = '';
            fetchPasswords();
        } else {
            alert(d.error || 'Error saving passwords');
        }
    } catch(e) {
        alert('Error');
    }
};

// System Logs
async function fetchLogs() {
    try {
        const r = await fetch('/api/logs');
        const logs = await r.json();
        renderLogs(logs);
    } catch(e) {
        console.error(e);
        $('#logs-container').innerHTML = '<div class="log-error">Error loading logs</div>';
    }
}

function renderLogs(logs) {
    const container = $('#logs-container');
    if (!logs || logs.length === 0) {
        container.innerHTML = '<div class="log-empty">No logs available</div>';
        return;
    }

    const html = logs.slice().reverse().map(log => {
        const time = formatLogTime(log);
        const levelClass = log.l.toLowerCase();
        return `<div class="log-entry log-${levelClass}">
            <span class="log-time">${time}</span>
            <span class="log-level">${log.l}</span>
            <span class="log-msg">${escapeHtml(log.m)}</span>
        </div>`;
    }).join('');

    container.innerHTML = html;
}

function formatLogTime(log) {
    if (log.e > 0) {
        const d = new Date(log.e * 1000);
        const now = new Date();
        const isToday = d.toDateString() === now.toDateString();
        const timeStr = d.toLocaleTimeString('nl-NL', {hour: '2-digit', minute: '2-digit', second: '2-digit'});

        if (isToday) {
            return timeStr;
        } else {
            const dateStr = d.toLocaleDateString('nl-NL', {day: '2-digit', month: '2-digit'});
            return `${dateStr} ${timeStr}`;
        }
    }
    const secs = Math.floor((log.u || 0) / 1000);
    const mins = Math.floor(secs / 60);
    if (mins > 0) return `+${mins}m${secs % 60}s`;
    return `+${secs}s`;
}

function escapeHtml(str) {
    return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

$('#refresh-logs').onclick = () => fetchLogs();

$('#clear-logs').onclick = async () => {
    if (confirm('Clear all logs?')) {
        try {
            await fetch('/api/logs', { method: 'DELETE' });
            fetchLogs();
        } catch(e) {
            console.error(e);
        }
    }
};

$('#logs-section')?.addEventListener('toggle', function() {
    if (this.open) fetchLogs();
});

// Tag Registry
async function fetchTags() {
    try {
        const r = await fetch('/api/tags');
        const d = await r.json();
        renderTags(d.tags || []);
        const countEl = $('#tag-count');
        if (countEl) {
            countEl.textContent = d.count > 0 ? `(${d.count}/${d.max})` : '';
        }
    } catch(e) {
        console.error(e);
        $('#registered-tags').innerHTML = '<div class="no-tags">Error loading tags</div>';
    }
}

function renderTags(tags) {
    const container = $('#registered-tags');
    if (!tags || tags.length === 0) {
        container.innerHTML = '<div class="no-tags">No tags registered</div>';
        return;
    }

    const html = tags.map(tag => `
        <div class="tag-item">
            <div class="tag-info">
                <span class="tag-name">${escapeHtml(tag.name)}</span>
                <span class="tag-uid">${escapeHtml(tag.uid)}</span>
            </div>
            <button class="btn-sm danger delete-tag" data-uid="${escapeHtml(tag.uid)}">Delete</button>
        </div>
    `).join('');

    container.innerHTML = html;

    // Add delete handlers
    container.querySelectorAll('.delete-tag').forEach(btn => {
        btn.onclick = async () => {
            const uid = btn.dataset.uid;
            if (confirm(`Delete tag "${uid}"?`)) {
                try {
                    const r = await fetch(`/api/tags?uid=${encodeURIComponent(uid)}`, { method: 'DELETE' });
                    const d = await r.json();
                    if (d.success) {
                        fetchTags();
                    } else {
                        alert(d.error || 'Error deleting tag');
                    }
                } catch(e) {
                    alert('Error deleting tag');
                }
            }
        };
    });
}

// Use last scanned UID button
$('#use-last-uid').onclick = () => {
    const lastUid = $('#last-uid').textContent;
    if (lastUid && lastUid !== '--') {
        $('#tag-uid').value = lastUid;
        $('#tag-name').focus();
    } else {
        alert('No tag scanned yet');
    }
};

// Tag registration form
$('#tag-form').onsubmit = async e => {
    e.preventDefault();
    const uid = $('#tag-uid').value.trim();
    const name = $('#tag-name').value.trim();

    if (!uid) {
        alert('Please scan a tag first and click "Use Last UID"');
        return;
    }
    if (!name) {
        alert('Please enter a name for the tag');
        return;
    }

    try {
        const r = await fetch('/api/tags', {
            method: 'POST',
            body: new URLSearchParams({ uid, name })
        });
        const d = await r.json();
        if (d.success) {
            $('#tag-uid').value = '';
            $('#tag-name').value = '';
            fetchTags();
            alert('Tag registered! A new trigger will appear in Home Assistant.');
        } else {
            alert(d.error || 'Error registering tag');
        }
    } catch(e) {
        alert('Error registering tag');
    }
};

// Load tags when section is opened
$('#tags-section')?.addEventListener('toggle', function() {
    if (this.open) fetchTags();
});

// Night mode toggle
const nightToggle = $('#night-mode-toggle');
if (nightToggle) {
    nightToggle.addEventListener('change', async function() {
        this._userInteracting = true;
        const enabled = this.checked;
        const status = $('#night-mode-status');

        try {
            const r = await fetch('/api/night_mode', {
                method: 'POST',
                body: new URLSearchParams({ enabled: enabled ? 'true' : 'false' })
            });
            const d = await r.json();
            if (d.success) {
                status.textContent = d.night_mode ? 'On' : 'Off';
                status.classList.toggle('active', d.night_mode);
            }
        } catch(e) {
            console.error('Night mode error:', e);
            // Revert toggle on error
            this.checked = !enabled;
        }

        setTimeout(() => { this._userInteracting = false; }, 1000);
    });
}
