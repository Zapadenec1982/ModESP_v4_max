<script>
  import { state } from "../stores/state.js";
  import { t } from "../stores/i18n.js";
  import ZoneCard from "../components/ZoneCard.svelte";
  import MiniChart from "../components/MiniChart.svelte";

  $: activeZones = parseInt($state["equipment.active_zones"]) || 1;
  $: multiZone = activeZones >= 2;

  // Firmware always uses zone namespaces (thermo_z1, defrost_z1) even in single-zone mode.
  // In single-zone: hide zone label. In multi-zone: show "Zone 1" / "Zone 2".
  $: zones = multiZone
    ? [
        { label: "Zone 1", thermoPrefix: "thermo_z1", defrostPrefix: "defrost_z1",
          evapTempKey: "equipment.evap_temp_z1", evapFanKey: "equipment.evap_fan_z1" },
        { label: "Zone 2", thermoPrefix: "thermo_z2", defrostPrefix: "defrost_z2",
          evapTempKey: "equipment.evap_temp_z2", evapFanKey: "equipment.evap_fan_z2" },
      ]
    : [
        { label: $t['dash.chamber'], thermoPrefix: "thermo_z1", defrostPrefix: "defrost_z1",
          evapTempKey: "equipment.evap_temp_z1", evapFanKey: "equipment.evap_fan_z1" },
      ];

  $: compressor = $state["equipment.compressor"];
  $: condFan = $state["equipment.cond_fan"];
  $: hasCondFan = $state["equipment.has_cond_fan"];
  $: hasCondTemp = $state["equipment.has_cond_temp"];
  $: hasEvapTemp = $state["equipment.has_evap_temp"];
  $: alarmActive = $state["protection.alarm_active"];
  $: alarmCode = $state["protection.alarm_code"];

  // Single-zone: defrost/evapFan shown in eq grid. Multi-zone: shown inside ZoneCard.
  $: defrostActive = $state["defrost_z1.active"];
  $: evapFan = $state["equipment.evap_fan_z1"];

  function formatTime(secs) {
    if (!secs) return "00:00";
    const h = Math.floor(secs / 3600);
    const m = Math.floor((secs % 3600) / 60);
    return h > 0
      ? `${h.toString().padStart(2, "0")}:${m.toString().padStart(2, "0")}`
      : `${m.toString().padStart(2, "0")}:${(secs % 60).toString().padStart(2, "0")}`;
  }
</script>

<div class="page" id="pg-dash">
  <!-- 1. Zone Temperature Cards -->
  <div class="zone-grid" class:multi={multiZone}>
    {#each zones as zone (zone.thermoPrefix)}
      <ZoneCard {zone} {multiZone} />
    {/each}
  </div>

  <!-- 2. Metrics Row (shared sensors, single-zone only) -->
  {#if !multiZone}
    <div class="met-row">
      {#if hasEvapTemp}
        <div class="met">
          <div class="met-l">{$t['dash.evap_temp']}</div>
          <div class="met-v">
            {typeof $state["equipment.evap_temp"] === "number"
              ? $state["equipment.evap_temp"].toFixed(1)
              : "—"}<span class="met-u">°C</span>
          </div>
        </div>
      {/if}
      {#if hasCondTemp}
        <div class="met">
          <div class="met-l">{$t['dash.cond_short']}</div>
          <div class="met-v">
            {typeof $state["equipment.cond_temp"] === "number"
              ? $state["equipment.cond_temp"].toFixed(1)
              : "—"}<span class="met-u">°C</span>
          </div>
        </div>
      {/if}
    </div>
  {/if}

  <!-- 2b. Shared metrics for multi-zone (cond temp only) -->
  {#if multiZone && hasCondTemp}
    <div class="met-row">
      <div class="met">
        <div class="met-l">{$t['dash.cond_short']}</div>
        <div class="met-v">
          {typeof $state["equipment.cond_temp"] === "number"
            ? $state["equipment.cond_temp"].toFixed(1)
            : "—"}<span class="met-u">°C</span>
        </div>
      </div>
    </div>
  {/if}

  <!-- 3. Protection Summary -->
  <div class="prot-row" class:alarm={alarmActive}>
    <svg class="prot-icon" width="18" height="18" viewBox="0 0 24 24" fill="none"
      stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
      <path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/>
    </svg>
    <span class="prot-lbl">{$t['dash.protection']}</span>
    <div class="badge {alarmActive ? 'red' : 'green'} prot-badge">
      <span class="bdot"></span>
      {alarmActive ? $t['dash.prot_alarm'] : $t['dash.prot_ok']}
    </div>
    {#if alarmActive && alarmCode}
      <span class="prot-code">{alarmCode}</span>
    {/if}
  </div>

  <!-- 4. Equipment Grid -->
  <div class="eq-grid">
    <div class="eq-cell">
      <span class="eq-lbl">{$t['dash.compressor']}</span>
      <div class="eq-bot">
        <div class="badge {compressor ? 'blue' : 'neutral'}">
          <span class="bdot"></span>{compressor ? $t['eq.on'] : $t['eq.off']}
        </div>
        {#if compressor}
          <span class="eq-time">{formatTime($state["equipment.comp_run_time"])}</span>
        {/if}
      </div>
      <div class="eq-progress {compressor ? 'active blue' : ''}"></div>
    </div>
    {#if !multiZone}
      <div class="eq-cell">
        <span class="eq-lbl">{$t['dash.defrost_label']}</span>
        <div class="eq-bot">
          <div class="badge {defrostActive ? 'orange' : 'neutral'}">
            <span class="bdot"></span>{defrostActive ? $t['eq.on'] : $t['eq.idle']}
          </div>
          {#if !defrostActive && $state["defrost_z1.time_to_next"]}
            <span class="eq-time"
              >{$t['dash.in_time']} {formatTime($state["defrost_z1.time_to_next"] * 60)}</span>
          {/if}
        </div>
        <div class="eq-progress {defrostActive ? 'active orange' : ''}"></div>
      </div>
      <div class="eq-cell">
        <span class="eq-lbl">{$t['dash.evap_fan_label']}</span>
        <div class="eq-bot">
          <div class="badge {evapFan ? 'frost' : 'neutral'}">
            <span class="bdot"></span>{evapFan ? $t['eq.on'] : $t['eq.off']}
          </div>
        </div>
        <div class="eq-progress {evapFan ? 'active frost' : ''}"></div>
      </div>
    {/if}
    {#if hasCondFan}
      <div class="eq-cell">
        <span class="eq-lbl">{$t['dash.cond_fan_label']}</span>
        <div class="eq-bot">
          <div class="badge {condFan ? 'green' : 'neutral'}">
            <span class="bdot"></span>{condFan ? $t['eq.on'] : $t['eq.off']}
          </div>
        </div>
        <div class="eq-progress {condFan ? 'active green' : ''}"></div>
      </div>
    {/if}
  </div>

  <!-- 5. MiniChart -->
  <MiniChart />
</div>

<style>
  .page {
    display: flex;
    flex-direction: column;
    gap: 14px;
    width: 100%;
  }

  /* ===== ZONE GRID ===== */
  .zone-grid {
    display: grid;
    grid-template-columns: 1fr;
    gap: 14px;
  }
  .zone-grid.multi {
    grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
  }

  /* ===== BADGES (shared with ZoneCard via global) ===== */
  .badge {
    display: inline-flex;
    align-items: center;
    gap: 7px;
    padding: 6px 12px;
    border-radius: var(--radius-xs);
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 0.4px;
    white-space: nowrap;
  }
  .bdot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    flex-shrink: 0;
  }
  .badge.blue { background: var(--accent-dim); color: var(--accent); }
  .badge.blue .bdot { background: var(--accent); box-shadow: 0 0 6px var(--accent-glow); }
  .badge.green { background: var(--ok-dim); color: var(--ok); }
  .badge.green .bdot { background: var(--ok); box-shadow: 0 0 6px var(--ok-glow); }
  .badge.red { background: var(--danger-dim); color: var(--danger); }
  .badge.red .bdot { background: var(--danger); box-shadow: 0 0 4px var(--danger-glow); }
  .badge.orange { background: var(--warn-dim); color: var(--warn); }
  .badge.orange .bdot { background: var(--warn); box-shadow: 0 0 6px var(--warn-glow); }
  .badge.frost { background: var(--frost-dim); color: var(--frost); }
  .badge.frost .bdot { background: var(--frost); box-shadow: 0 0 6px var(--frost-glow); }
  .badge.neutral { background: var(--surface-2); color: var(--text-3); border: 1px solid var(--border); }
  .badge.neutral .bdot { background: var(--text-4); }

  /* ===== METRICS ROW ===== */
  .met-row {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
    gap: 14px;
  }
  .met {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 16px 14px;
    position: relative;
    overflow: hidden;
    transition: transform var(--transition-fast), border-color var(--transition-fast);
  }
  .met:hover { transform: translateY(-2px); border-color: var(--border-accent); }
  .met::before {
    content: "";
    position: absolute;
    left: 0; top: 0; bottom: 0;
    width: 3px;
    background: var(--border-accent);
  }
  .met:nth-child(1)::before { background: var(--frost); box-shadow: 0 0 8px var(--frost-glow); }
  .met:nth-child(2)::before { background: var(--warn); box-shadow: 0 0 8px var(--warn-glow); }
  .met::after {
    content: "";
    position: absolute;
    top: 0; left: 0; right: 0;
    height: 1px;
    background: linear-gradient(90deg, transparent, var(--border-accent), transparent);
  }
  .met-l {
    font-size: 9px;
    font-weight: 600;
    color: var(--text-3);
    text-transform: uppercase;
    letter-spacing: 1.2px;
    margin-bottom: 8px;
  }
  .met-v {
    font-size: 22px;
    font-weight: 700;
    letter-spacing: -1px;
    font-variant-numeric: tabular-nums;
    color: var(--text-1);
  }
  .met-u { font-size: 12px; font-weight: 300; color: var(--text-4); }

  /* ===== EQUIPMENT GRID ===== */
  .eq-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
  }
  .eq-cell {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 12px;
    position: relative;
    overflow: hidden;
    transition: transform var(--transition-fast), border-color var(--transition-fast);
  }
  .eq-cell:hover { transform: translateY(-2px); border-color: var(--border-accent); }
  .eq-lbl { font-size: 12px; color: var(--text-2); font-weight: 500; }
  .eq-bot { display: flex; align-items: center; justify-content: space-between; }
  .eq-time { font-size: 11px; color: var(--text-3); font-variant-numeric: tabular-nums; }

  .eq-progress {
    position: absolute;
    bottom: 0; left: 0; right: 0;
    height: 2px;
    background: var(--surface-3);
    overflow: hidden;
  }
  .eq-progress::after {
    content: "";
    position: absolute;
    top: 0; left: 0;
    width: 50%; height: 100%;
    background: var(--text-4);
    transition: background 0.3s;
    transform: translateX(-100%);
    will-change: transform;
  }
  .eq-progress.active::after { animation: eqScan 2s ease-in-out infinite; }
  .eq-progress.blue.active::after { background: var(--accent); box-shadow: 0 0 8px var(--accent-glow); }
  .eq-progress.orange.active::after { background: var(--warn); box-shadow: 0 0 8px var(--warn-glow); }
  .eq-progress.frost.active::after { background: var(--frost); box-shadow: 0 0 8px var(--frost-glow); }
  .eq-progress.green.active::after { background: var(--ok); box-shadow: 0 0 8px var(--ok-glow); }

  @keyframes eqScan {
    0% { transform: translateX(-100%); }
    100% { transform: translateX(200%); }
  }

  /* ===== PROTECTION SUMMARY ===== */
  .prot-row {
    display: flex;
    align-items: center;
    gap: 10px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 14px 16px;
    transition: border-color 0.3s;
  }
  .prot-row.alarm { border-color: var(--danger-border); background: var(--danger-dim); }
  .prot-icon { flex-shrink: 0; color: var(--ok); }
  .prot-row.alarm .prot-icon { color: var(--danger); }
  .prot-lbl { font-size: 13px; font-weight: 600; color: var(--text-1); flex: 1; }
  .prot-badge { flex-shrink: 0; }
  .prot-code {
    font-size: 12px;
    font-weight: 700;
    color: var(--danger);
    font-family: var(--font-mono);
    letter-spacing: 0.5px;
  }
</style>
