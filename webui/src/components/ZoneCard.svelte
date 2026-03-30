<script>
  import { state } from "../stores/state.js";
  import { stateMeta } from "../stores/ui.js";
  import { t } from "../stores/i18n.js";
  import SliderWidget from "./widgets/SliderWidget.svelte";

  /** @type {{ label: string, thermoPrefix: string, defrostPrefix: string, evapTempKey: string, evapFanKey: string }} */
  export let zone;
  export let multiZone = false;

  $: displayTemp = $state[`${zone.thermoPrefix}.display_temp`];
  $: temperature = $state[`${zone.thermoPrefix}.temperature`];
  $: setpoint = $state[`${zone.thermoPrefix}.setpoint`];
  $: defrostActive = $state[`${zone.defrostPrefix}.active`];
  $: defrostTimeToNext = $state[`${zone.defrostPrefix}.time_to_next`];
  $: nightActive = $state[`${zone.thermoPrefix}.night_active`];
  $: compressor = $state["equipment.compressor"];
  $: evapFan = $state[zone.evapFanKey];
  $: evapTemp = $state[zone.evapTempKey];
  $: alarmActive = $state["protection.alarm_active"];
  $: hasEvapTemp = $state["equipment.has_evap_temp"];
  $: showDefrostSymbol = typeof displayTemp === "number" && displayTemp <= -900;

  $: sp = typeof setpoint === "number" ? setpoint : -18;
  $: shownTemp = showDefrostSymbol
    ? null
    : typeof displayTemp === "number"
      ? displayTemp
      : temperature;
  $: tempColor = getTemperatureColor(shownTemp, sp);
  $: tempGlow = getTemperatureGlow(shownTemp, sp);
  $: spKey = `${zone.thermoPrefix}.setpoint`;
  $: spMeta = $stateMeta[spKey] || { min: -35, max: 0, step: 0.5 };

  function getTemperatureColor(temp, sp) {
    if (typeof temp !== "number") return "var(--fg-muted)";
    if (temp < sp - 2) return "#3b82f6";
    if (temp <= sp + 2) return "var(--status-ok)";
    if (temp <= sp + 5) return "#f59e0b";
    return "#ef4444";
  }

  function getTemperatureGlow(temp, sp) {
    if (typeof temp !== "number") return "rgba(100,100,100,0.06)";
    if (temp < sp - 2) return "rgba(59,130,246,0.18)";
    if (temp <= sp + 2) return "rgba(34,197,94,0.15)";
    if (temp <= sp + 5) return "rgba(245,158,11,0.20)";
    return "rgba(239,68,68,0.25)";
  }

  function getTempClass(temp, sp) {
    if (typeof temp !== "number") return "ok";
    return temp > sp + 5 ? "alarm" : "ok";
  }

  function formatTime(secs) {
    if (!secs) return "00:00";
    const h = Math.floor(secs / 3600);
    const m = Math.floor((secs % 3600) / 60);
    return h > 0
      ? `${h.toString().padStart(2, "0")}:${m.toString().padStart(2, "0")}`
      : `${m.toString().padStart(2, "0")}:${(secs % 60).toString().padStart(2, "0")}`;
  }
</script>

<div class="card">
  <div
    class="temp-card {alarmActive || getTempClass(shownTemp, sp) === 'alarm' ? 'alarm' : 'ok'}"
    class:compact={multiZone}
    style="--temp-glow: {tempGlow}"
  >
    <div class="temp-ey">{zone.label}</div>
    <div class="temp-hero">
      <div class="temp-dg">
        <span class="temp-n" style="color: {tempColor}">
          {#if showDefrostSymbol}
            -d-
          {:else if typeof shownTemp === "number"}
            {shownTemp.toFixed(1)}
          {:else}
            —
          {/if}
        </span>
        <span class="temp-deg">{showDefrostSymbol ? "" : "°C"}</span>
      </div>
      <div class="hero-badges">
        <div
          class="badge {defrostActive ? 'orange' : compressor ? 'cyan' : 'neutral'}"
        >
          <span class="bdot"></span>
          {defrostActive
            ? $t['dash.mode_defrost']
            : compressor
              ? $t['dash.mode_cooling']
              : $t['dash.mode_idle']}
        </div>
        {#if nightActive}
          <div class="badge night">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none"
              stroke="currentColor" stroke-width="2">
              <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>
            </svg>
            {$t['state.night']}
          </div>
        {/if}
      </div>
    </div>
    <div class="sp-area">
      <div class="sp-row">
        <span class="sp-lbl">{$t['dash.sp_label']}</span>
        <span class="sp-val">{sp.toFixed(1)}<small>°C</small></span>
      </div>
      <div class="slider-wrap">
        <SliderWidget
          config={{
            key: spKey,
            description: "",
            unit: "",
            min: spMeta.min,
            max: spMeta.max,
            step: spMeta.step,
            hideHeader: true,
            hideLabels: true,
          }}
          value={setpoint}
        />
      </div>
      <div class="rng-lbl">
        <span>{spMeta.min}</span><span>{spMeta.max}</span>
      </div>
    </div>

    <!-- Per-zone equipment indicators -->
    {#if multiZone}
      <div class="zone-eq">
        <div class="zone-eq-item">
          <span class="zone-eq-lbl">{$t['dash.defrost_label']}</span>
          <div class="badge sm {defrostActive ? 'orange' : 'neutral'}">
            <span class="bdot"></span>
            {defrostActive ? $t['eq.on'] : $t['eq.idle']}
          </div>
          {#if !defrostActive && defrostTimeToNext}
            <span class="zone-eq-time">{formatTime(defrostTimeToNext * 60)}</span>
          {/if}
        </div>
        <div class="zone-eq-item">
          <span class="zone-eq-lbl">{$t['dash.evap_fan_label']}</span>
          <div class="badge sm {evapFan ? 'frost' : 'neutral'}">
            <span class="bdot"></span>
            {evapFan ? $t['eq.on'] : $t['eq.off']}
          </div>
        </div>
        {#if hasEvapTemp && typeof evapTemp === "number"}
          <div class="zone-eq-item">
            <span class="zone-eq-lbl">{$t['dash.evap_temp']}</span>
            <span class="zone-eq-val">{evapTemp.toFixed(1)}°C</span>
          </div>
        {/if}
      </div>
    {/if}
  </div>
</div>

<style>
  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    overflow: hidden;
  }

  /* ===== TEMP CARD ===== */
  .temp-card {
    position: relative;
    padding: 32px 28px 24px;
    background: linear-gradient(180deg, var(--surface), var(--surface-2));
    overflow: hidden;
  }
  .temp-card::before {
    content: "";
    position: absolute;
    width: 200px;
    height: 200px;
    border-radius: 50%;
    background: var(--temp-glow, transparent);
    bottom: -40px;
    right: -40px;
    filter: blur(60px);
    pointer-events: none;
    animation: heroBreath 4s ease-in-out infinite;
  }
  @keyframes heroBreath {
    0%, 100% { transform: scale(1); opacity: 0.8; }
    50% { transform: scale(1.15); opacity: 0.5; }
  }

  .temp-card.compact {
    padding: 24px 20px 18px;
  }
  .temp-card.compact .temp-n {
    font-size: clamp(40px, 10vw, 56px);
  }
  .temp-card.compact .sp-val {
    font-size: 22px;
  }

  .temp-ey {
    font-size: 11px;
    font-weight: 600;
    color: var(--text-3);
    text-transform: uppercase;
    letter-spacing: 2px;
    margin-bottom: 12px;
    position: relative;
    z-index: 1;
  }
  .temp-hero {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    margin-bottom: 24px;
    position: relative;
    z-index: 1;
  }
  .temp-dg {
    display: flex;
    align-items: baseline;
    gap: 4px;
  }
  .temp-n {
    font-size: var(--text-hero-lg);
    font-weight: 300;
    line-height: 1;
    letter-spacing: -3px;
    font-variant-numeric: tabular-nums;
    transition: color 0.5s ease;
    text-shadow: 0 0 30px currentColor;
  }
  .temp-deg {
    font-size: clamp(18px, 4vw, 24px);
    font-weight: 300;
    color: var(--text-4);
    margin-left: 4px;
    align-self: flex-start;
    margin-top: 10px;
  }

  .sp-area {
    margin-top: 20px;
    padding-top: 20px;
    border-top: 1px solid var(--border);
  }
  .sp-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 14px;
  }
  .sp-lbl {
    font-size: 13px;
    color: var(--text-2);
    font-weight: 500;
  }
  .sp-val {
    font-size: 28px;
    font-weight: 600;
    color: var(--ok);
    letter-spacing: -1px;
    font-variant-numeric: tabular-nums;
    text-shadow: 0 0 24px var(--ok-glow);
  }
  .sp-val small {
    font-size: 14px;
    font-weight: 400;
    color: var(--text-3);
    text-shadow: none;
  }
  .rng-lbl {
    display: flex;
    justify-content: space-between;
    margin-top: 2px;
  }
  .rng-lbl span {
    font-size: 9px;
    color: var(--text-4);
    font-variant-numeric: tabular-nums;
  }
  .slider-wrap {
    width: 100%;
    margin-top: -12px;
    margin-bottom: -16px;
  }

  /* ===== HERO BADGES ===== */
  .hero-badges {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 6px;
  }
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
  .badge.sm { padding: 4px 8px; font-size: 10px; }
  .bdot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    flex-shrink: 0;
  }
  .badge.cyan { background: var(--info-dim); color: var(--info); }
  .badge.cyan .bdot { background: var(--info); box-shadow: 0 0 6px var(--info-glow); }
  .badge.orange { background: var(--warn-dim); color: var(--warn); }
  .badge.orange .bdot { background: var(--warn); box-shadow: 0 0 6px var(--warn-glow); }
  .badge.frost { background: var(--frost-dim); color: var(--frost); }
  .badge.frost .bdot { background: var(--frost); box-shadow: 0 0 6px var(--frost-glow); }
  .badge.neutral { background: var(--surface-2); color: var(--text-3); border: 1px solid var(--border); }
  .badge.neutral .bdot { background: var(--text-4); }
  .badge.night { background: var(--warn-dim); color: var(--warn); gap: 5px; }

  /* ===== ZONE EQUIPMENT ROW ===== */
  .zone-eq {
    display: flex;
    flex-wrap: wrap;
    gap: 12px;
    margin-top: 16px;
    padding-top: 14px;
    border-top: 1px solid var(--border);
    position: relative;
    z-index: 1;
  }
  .zone-eq-item {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .zone-eq-lbl {
    font-size: 10px;
    color: var(--text-3);
    font-weight: 500;
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }
  .zone-eq-time {
    font-size: 10px;
    color: var(--text-4);
    font-variant-numeric: tabular-nums;
  }
  .zone-eq-val {
    font-size: 13px;
    font-weight: 600;
    color: var(--text-1);
    font-variant-numeric: tabular-nums;
  }
</style>
