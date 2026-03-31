<script>
  import { apiPost } from '../../lib/api.js';
  import { t } from '../../stores/i18n.js';
  import { toastSuccess, toastError, toastWarn } from '../../stores/toast.js';
  import { wifiSsid, wifiPassword } from '../../stores/wifiForm.js';
  import { state } from '../../stores/state.js';

  export let config;

  let loading = false;
  let connecting = false;
  let redirectUrl = '';
  let countdown = 30;
  let countdownInterval = null;

  // Реактивне спостереження за wifi.ip — коли STA підключиться, redirect
  $: if (connecting && $state['wifi.ip'] && $state['wifi.ip'] !== '192.168.4.1' && $state['wifi.ip'] !== '') {
    // STA підключився — redirect на нову адресу
    const newIp = $state['wifi.ip'];
    toastSuccess(`Connected! IP: ${newIp}`);
    clearInterval(countdownInterval);
    // Redirect через 2с щоб toast встиг показатись
    setTimeout(() => {
      window.location.href = redirectUrl || `http://${newIp}`;
    }, 2000);
  }

  async function save() {
    const ssid = ($wifiSsid || '').trim();
    const password = $wifiPassword || '';

    if (!ssid) { toastWarn($t['alert.ssid_empty']); return; }

    loading = true;
    try {
      const r = await apiPost('/api/wifi', { ssid, password });
      if (r.ok) {
        redirectUrl = r.redirect || '';
        toastSuccess($t['alert.wifi_saved'] || 'WiFi saved');
        startConnecting();
      } else {
        toastError($t['alert.error']);
      }
    } catch (e) {
      toastError($t['alert.error'] + ': ' + e.message);
    } finally {
      loading = false;
    }
  }

  function startConnecting() {
    connecting = true;
    countdown = 30;
    countdownInterval = setInterval(() => {
      countdown--;
      if (countdown <= 0) {
        clearInterval(countdownInterval);
        connecting = false;
        toastError($t['wifi.connect_timeout'] || 'Connection timeout. Try again or check credentials.');
      }
    }, 1000);
  }
</script>

<div class="save-widget">
  <button class="action-btn" disabled={loading || connecting} on:click={save}>
    {loading ? '...' : (config.label || $t['btn.save'])}
  </button>
</div>

{#if connecting}
  <div class="dialog-overlay">
    <div class="dialog">
      <div class="dialog-icon">📶</div>
      <div class="dialog-title">{$t['wifi.connecting_title'] || 'Підключення...'}</div>
      <div class="dialog-text">
        {$t['wifi.connecting_text'] || 'Підключаємось до нової мережі. AP залишається доступним.'}
      </div>
      <div class="progress-bar">
        <div class="progress-fill" style="width: {((30 - countdown) / 30) * 100}%"></div>
      </div>
      <div class="countdown">{countdown}s</div>
    </div>
  </div>
{/if}

<style>
  .save-widget { padding: 4px 0; margin-top: 6px; }
  .action-btn {
    background: linear-gradient(135deg, var(--accent), #0369a1);
    color: #fff; border: none;
    padding: 12px 24px; border-radius: 10px;
    cursor: pointer; font-size: 14px; font-weight: 600;
    width: 100%; transition: all 0.2s;
    box-shadow: 0 2px 8px rgba(59,130,246,0.3);
  }
  .action-btn:hover:not(:disabled) { transform: translateY(-1px); }
  .action-btn:disabled { opacity: 0.6; cursor: not-allowed; }

  .dialog-overlay {
    position: fixed; top: 0; left: 0; right: 0; bottom: 0;
    background: rgba(0,0,0,0.5); z-index: 1000;
    display: flex; align-items: center; justify-content: center;
  }
  .dialog {
    background: var(--card-bg, #fff); border-radius: 16px;
    padding: 28px; max-width: 360px; width: 90%;
    text-align: center; box-shadow: 0 8px 32px rgba(0,0,0,0.2);
  }
  .dialog-icon { font-size: 40px; margin-bottom: 12px; }
  .dialog-title { font-size: 18px; font-weight: 700; margin-bottom: 8px; color: var(--text, #1e293b); }
  .dialog-text { font-size: 14px; color: var(--text-secondary, #64748b); margin-bottom: 20px; line-height: 1.5; }
  .dialog-buttons { display: flex; gap: 10px; }
  .progress-bar {
    width: 100%; height: 6px; background: var(--border, #e2e8f0);
    border-radius: 3px; overflow: hidden; margin-top: 16px;
  }
  .progress-fill {
    height: 100%; background: linear-gradient(90deg, #3b82f6, #06b6d4);
    border-radius: 3px; transition: width 1s linear;
  }
  .countdown {
    margin-top: 8px; font-size: 13px; color: var(--text-secondary, #64748b);
  }
</style>
