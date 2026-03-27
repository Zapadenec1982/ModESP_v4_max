// ═══════════════════════════════════════════════════════════════
//  Refrigerant Saturation — Antoine Equation Implementation
//
//  Constants: Danfoss AM187286420404en-000702 (NIST RefProp 10.0)
//  Equation:  Te[K] = A2 / (ln(Pe[Pa]) - A1) - A3
//             Te[°C] = Te[K] - 273.15
//
//  Zero heap. All tables in flash (constexpr).
// ═══════════════════════════════════════════════════════════════

#include "modesp/refrigerant/saturation.h"
#include <cmath>

namespace modesp {

// ─── Antoine constants table (flash/DROM) ──────────────────
//
// Source: Danfoss fact sheet AM187286420404en-000702, May 2025
// Based on NIST RefProp 10.0
// Pressure unit: Pa (Pascals) with natural logarithm
//
static constexpr AntoineConstants REFRIGERANTS[] = {
    // idx  A1       A2        A3      Glide  Name
    {  9.936f, -2147.9f, 242.3f,  0.0f, "R134a"   },  //  0
    {  9.715f, -1946.4f, 245.8f,  0.5f, "R404A"   },  //  1
    {  9.770f, -1974.6f, 248.8f,  0.0f, "R507A"   },  //  2
    {  9.400f, -1990.7f, 253.9f,  0.0f, "R290"    },  //  3
    {  9.993f, -2021.4f, 242.2f,  5.4f, "R448A"   },  //  4
    {  9.975f, -2019.7f, 242.4f,  5.0f, "R449A"   },  //  5
    {  9.922f, -2023.5f, 247.1f,  3.7f, "R452A"   },  //  6
    { 10.060f, -2037.4f, 241.1f,  5.3f, "R407A"   },  //  7
    { 10.072f, -2059.7f, 241.1f,  5.9f, "R407C"   },  //  8
    { 10.091f, -2035.0f, 241.3f,  5.3f, "R407F"   },  //  9
    { 10.086f, -1990.0f, 248.7f,  0.1f, "R410A"   },  // 10
    { 10.271f, -2059.6f, 252.1f,  0.0f, "R32"     },  // 11
    { 10.043f, -1996.0f, 248.1f,  1.2f, "R454B"   },  // 12
    { 10.027f, -2058.0f, 247.3f,  5.4f, "R454A"   },  // 13
    { 10.661f, -1904.5f, 267.9f,  0.0f, "R744"    },  // 14  CO2
    { 10.760f, -2307.3f, 247.9f,  0.0f, "R717"    },  // 15  NH3
    {  9.748f, -2017.2f, 247.8f,  0.0f, "R22"     },  // 16
    {  9.322f, -2203.9f, 248.2f,  0.0f, "R600a"   },  // 17
    {  9.664f, -2055.8f, 242.3f,  0.0f, "R513A"   },  // 18
    {  9.644f, -2108.3f, 248.2f,  0.0f, "R1234yf" },  // 19
    {  9.775f, -2179.7f, 242.0f,  0.0f, "R1234ze" },  // 20
    { 10.068f, -2111.4f, 248.9f, 11.3f, "R455A"   },  // 21
    {  9.430f, -1942.1f, 253.5f,  0.0f, "R1270"   },  // 22
};

static_assert(sizeof(REFRIGERANTS) / sizeof(REFRIGERANTS[0])
              == static_cast<size_t>(Refrigerant::COUNT),
              "REFRIGERANTS[] size must match Refrigerant::COUNT");

// ─── Lookup ────────────────────────────────────────────────

const AntoineConstants* get_refrigerant(Refrigerant r) {
    auto idx = static_cast<uint8_t>(r);
    if (idx >= static_cast<uint8_t>(Refrigerant::COUNT)) return nullptr;
    return &REFRIGERANTS[idx];
}

const AntoineConstants* get_refrigerant_by_index(uint8_t index) {
    if (index >= static_cast<uint8_t>(Refrigerant::COUNT)) return nullptr;
    return &REFRIGERANTS[index];
}

const char* refrigerant_name(Refrigerant r) {
    const auto* c = get_refrigerant(r);
    return c ? c->name : "?";
}

// ─── Antoine equation: pressure → temperature ──────────────
//
//  Te[K] = A2 / (ln(Pe[Pa]) - A1) - A3
//  Te[°C] = Te[K] - 273.15
//
//  Pe[Pa] = P_bar * 100000

float saturation_temp(const AntoineConstants& c, float p_bar) {
    if (p_bar <= 0.0f) return NAN;

    float ln_p = logf(p_bar * 100000.0f);
    float denom = ln_p - c.a1;

    // Guard against division by zero (shouldn't happen in valid range)
    if (fabsf(denom) < 1e-6f) return NAN;

    float t_kelvin = c.a2 / denom - c.a3;
    return t_kelvin - 273.15f;
}

float saturation_temp(Refrigerant r, float p_bar) {
    const auto* c = get_refrigerant(r);
    if (!c) return NAN;
    return saturation_temp(*c, p_bar);
}

// ─── Dew point (for superheat with zeotropic blends) ───────
//
//  T_dew = T_mid + glide/2

float dew_point_temp(const AntoineConstants& c, float p_bar) {
    float t_mid = saturation_temp(c, p_bar);
    if (std::isnan(t_mid)) return NAN;
    return t_mid + c.glide * 0.5f;
}

float dew_point_temp(Refrigerant r, float p_bar) {
    const auto* c = get_refrigerant(r);
    if (!c) return NAN;
    return dew_point_temp(*c, p_bar);
}

// ─── Inverse: temperature → pressure ───────────────────────
//
//  ln(Pe[Pa]) = A1 + A2 / (T[°C] + 273.15 + A3)
//  Pe[Pa] = exp(...)
//  P_bar = Pe / 100000

float saturation_pressure(const AntoineConstants& c, float t_c) {
    float t_kelvin = t_c + 273.15f;
    float denom = t_kelvin + c.a3;

    if (fabsf(denom) < 1e-6f) return NAN;

    float ln_p = c.a1 + c.a2 / denom;
    float p_pa = expf(ln_p);

    return p_pa / 100000.0f;
}

float saturation_pressure(Refrigerant r, float t_c) {
    const auto* c = get_refrigerant(r);
    if (!c) return NAN;
    return saturation_pressure(*c, t_c);
}

}  // namespace modesp
