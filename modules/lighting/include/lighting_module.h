/**
 * @file lighting_module.h
 * @brief Lighting control — OFF/ON/Auto (day-night ECO integration)
 *
 * Modes:
 *   0 = OFF — light always off
 *   1 = ON  — light always on
 *   2 = AUTO — light ON during day, OFF during night
 *              (reads thermostat.night_active from thermostat module)
 *
 * Equipment Layer integration:
 *   Lighting does NOT access HAL directly. It publishes
 *   lighting.req.light → Equipment reads and applies to relay.
 *   Light is independent of refrigeration arbitration.
 *
 * SharedState keys read:
 *   thermostat.night_active — bool, night mode flag from thermostat
 *
 * SharedState keys written:
 *   lighting.mode      — int (0=OFF, 1=ON, 2=AUTO), readwrite, persisted
 *   lighting.state     — bool (actual light state from equipment)
 *   lighting.req.light — bool (request to Equipment Manager)
 */

#pragma once

#include "modesp/base_module.h"

class LightingModule : public modesp::BaseModule {
public:
    LightingModule();

    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

private:
    int32_t mode_ = 0;           // 0=OFF, 1=ON, 2=AUTO
    bool light_request_ = false; // Current request to equipment
    bool last_actual_ = false;   // Cache to avoid version bumps
};
