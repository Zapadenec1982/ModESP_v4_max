<script>
  import { apiPost } from '../../lib/api.js';
  import { t } from '../../stores/i18n.js';
  import { toastSuccess, toastError, toastWarn } from '../../stores/toast.js';
  import { wifiSsid, wifiPassword } from '../../stores/wifiForm.js';

  export let config;

  let loading = false;

  let showRestartDialog = false;

  async function save() {
    const ssid = ($wifiSsid || '').trim();
    const password = $wifiPassword || '';

    if (!ssid) { toastWarn($t['alert.ssid_empty']); return; }

    loading = true;
    try {
      const r = await apiPost('/api/wifi', { ssid, password });
      if (r.ok) {
        toastSuccess($t['alert.wifi_saved'] || 'WiFi saved');
        showRestartDialog = true;
      } else {
        toastError($t['alert.error']);
      }
    } catch (e) {
      toastError($t['alert.error'] + ': ' + e.message);
    } finally {
      loading = false;
    }
  }

  async function restart() {
    showRestartDialog = false;
    toastSuccess($t['alert.restarting'] || 'Restarting...');
    try {
      await apiPost('/api/restart');
    } catch (e) {
      // Expected — connection lost during restart
    }
  }

  function dismissDialog() {
    showRestartDialog = false;
  }
</script>

<div class="save-widget">
  <button class="action-btn" disabled={loading} on:click={save}>
    {loading ? '...' : (config.label || $t['btn.save'])}
  </button>
</div>

{#if showRestartDialog}
  <div class="dialog-overlay" on:click={dismissDialog}>
    <div class="dialog" on:click|stopPropagation>
      <div class="dialog-icon">📶</div>
      <div class="dialog-title">{$t['wifi.restart_title'] || 'WiFi збережено'}</div>
      <div class="dialog-text">{$t['wifi.restart_text'] || 'Перезавантажити пристрій для підключення до нової мережі?'}</div>
      <div class="dialog-buttons">
        <button class="btn-cancel" on:click={dismissDialog}>{$t['btn.cancel'] || 'Пізніше'}</button>
        <button class="btn-restart" on:click={restart}>{$t['btn.restart'] || 'Перезавантажити'}</button>
      </div>
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
  .btn-cancel {
    flex: 1; padding: 10px; border-radius: 10px; border: 1px solid var(--border, #e2e8f0);
    background: transparent; color: var(--text-secondary, #64748b); cursor: pointer; font-size: 14px;
  }
  .btn-restart {
    flex: 1; padding: 10px; border-radius: 10px; border: none;
    background: linear-gradient(135deg, #3b82f6, #0369a1); color: #fff;
    cursor: pointer; font-size: 14px; font-weight: 600;
  }
  .btn-cancel:hover { background: var(--hover-bg, #f1f5f9); }
  .btn-restart:hover { opacity: 0.9; }
</style>
