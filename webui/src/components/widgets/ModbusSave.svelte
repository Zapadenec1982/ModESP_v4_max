<script>
  import { apiGet, apiPost } from '../../lib/api.js';
  import { setStateKey, state } from '../../stores/state.js';
  import { modbusSlaveAddr, modbusBaudRate, modbusParity } from '../../stores/modbusForm.js';
  import { t } from '../../stores/i18n.js';
  import { toastSuccess, toastError } from '../../stores/toast.js';
  import { onMount } from 'svelte';

  export let config;

  let loading = false;
  let mounted = false;
  let prevEnabled = null;

  onMount(async () => {
    try {
      const d = await apiGet('/api/modbus');
      // Status keys для відображення
      setStateKey('modbus.status', d.status || 'stopped');
      setStateKey('modbus.enabled', d.enabled || false);
      setStateKey('modbus.req_count', d.req_count || 0);
      setStateKey('modbus.err_count', d.err_count || 0);
      // Form fields через stores
      modbusSlaveAddr.set(d.slave_addr || 1);
      modbusBaudRate.set(d.baud_rate || 19200);
      modbusParity.set(d.parity || 'even');
      // Number/Select widgets читають з $state
      setStateKey('modbus.slave_addr', d.slave_addr || 1);
      setStateKey('modbus.baud_rate', d.baud_rate || 19200);
      setStateKey('modbus.parity', d.parity || 'even');
      prevEnabled = !!d.enabled;
    } catch (e) {}
    mounted = true;
  });

  // Instant toggle: при зміні modbus.enabled одразу відправляємо на сервер
  $: if (mounted && prevEnabled !== null && !!$state['modbus.enabled'] !== prevEnabled) {
    prevEnabled = !!$state['modbus.enabled'];
    apiPost('/api/modbus', { enabled: prevEnabled })
      .then(r => { if (r.ok) toastSuccess($t['alert.saved_modbus']); })
      .catch(() => {});
  }

  async function save() {
    loading = true;
    const data = {
      slave_addr: parseInt($state['modbus.slave_addr']) || 1,
      baud_rate: parseInt($state['modbus.baud_rate']) || 19200,
      parity: $state['modbus.parity'] || 'even',
      enabled: !!$state['modbus.enabled'],
    };

    try {
      const r = await apiPost('/api/modbus', data);
      if (r.ok) toastSuccess($t['alert.saved_modbus']);
      else toastError($t['alert.error']);
    } catch (e) {
      toastError($t['alert.error'] + ': ' + e.message);
    } finally {
      loading = false;
    }
  }
</script>

<div class="save-widget">
  <button class="action-btn" disabled={loading} on:click={save}>
    {loading ? '...' : (config.label || $t['btn.save'])}
  </button>
</div>

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
</style>
