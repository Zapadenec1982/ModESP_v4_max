#pragma once
// ═══════════════════════════════════════════════════════════════
//  Refrigerant Saturation — Antoine Equation (Danfoss method)
//
//  T_sat = A2 / (ln(P_bar * 100000) - A1) - A3 - 273.15  [°C]
//
//  Constants from Danfoss AM187286420404en-000702 (NIST RefProp 10.0).
//  Supports 23 refrigerants. Zero heap, constexpr tables in flash.
//
//  For zeotropic blends (glide > 0):
//    T_dew = T_mid + glide/2   (use for superheat calculation)
//    T_bubble = T_mid - glide/2
// ═══════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>

namespace modesp {

// ─── Refrigerant enum ──────────────────────────────────────
enum class Refrigerant : uint8_t {
    R134a    =  0,
    R404A    =  1,
    R507A    =  2,
    R290     =  3,   // Propane (natural, A3 flammable)
    R448A    =  4,   // R404A replacement (EU F-gas)
    R449A    =  5,   // R404A replacement (EU F-gas)
    R452A    =  6,   // R404A replacement
    R407A    =  7,
    R407C    =  8,   // R22 replacement
    R407F    =  9,   // R404A replacement
    R410A    = 10,   // Heat pumps, AC
    R32      = 11,   // Low GWP heat pumps
    R454B    = 12,   // Low GWP R410A replacement
    R454A    = 13,   // Low GWP
    R744     = 14,   // CO2 (transcritical)
    R717     = 15,   // Ammonia (industrial)
    R22      = 16,   // Legacy (phased out)
    R600a    = 17,   // Isobutane (domestic)
    R513A    = 18,   // R134a replacement
    R1234yf  = 19,   // Automotive, low GWP
    R1234ze  = 20,   // Chillers, low GWP
    R455A    = 21,   // R404A replacement, high glide
    R1270    = 22,   // Propylene (natural)

    COUNT    = 23,
    USER     = 254,  // User-defined A1/A2/A3/glide
    UNKNOWN  = 255,
};

// ─── Antoine constants ─────────────────────────────────────
struct AntoineConstants {
    float a1;           // ln(P) coefficient
    float a2;           // numerator
    float a3;           // offset (Kelvin)
    float glide;        // temperature glide for zeotropic blends (K)
    const char* name;   // human-readable name (e.g. "R404A")
};

// ─── Public API ────────────────────────────────────────────

/// Get Antoine constants for a refrigerant enum value.
/// Returns nullptr if refrigerant is UNKNOWN or out of range.
const AntoineConstants* get_refrigerant(Refrigerant r);

/// Get Antoine constants by index (0..COUNT-1).
/// Returns nullptr if index out of range.
const AntoineConstants* get_refrigerant_by_index(uint8_t index);

/// Get refrigerant name by enum. Returns "?" if unknown.
const char* refrigerant_name(Refrigerant r);

/// Total number of built-in refrigerants.
constexpr size_t REFRIGERANT_COUNT = static_cast<size_t>(Refrigerant::COUNT);

/// Calculate saturation temperature from pressure using Antoine equation.
/// @param r       Refrigerant enum
/// @param p_bar   Absolute pressure in bar (must be > 0)
/// @return        Saturation midpoint temperature in °C, or NAN on error
float saturation_temp(Refrigerant r, float p_bar);

/// Calculate saturation temperature with explicit Antoine constants.
/// Useful for USER-defined refrigerants.
/// @param c       Antoine constants
/// @param p_bar   Absolute pressure in bar (must be > 0)
/// @return        Saturation midpoint temperature in °C, or NAN on error
float saturation_temp(const AntoineConstants& c, float p_bar);

/// Calculate dew point temperature (for superheat calculation with blends).
/// T_dew = T_mid + glide/2
/// @param r       Refrigerant enum
/// @param p_bar   Absolute pressure in bar
/// @return        Dew point temperature in °C, or NAN on error
float dew_point_temp(Refrigerant r, float p_bar);

/// Calculate dew point temperature with explicit constants.
float dew_point_temp(const AntoineConstants& c, float p_bar);

/// Inverse: calculate saturation pressure from temperature.
/// P_bar = exp(A1 + A2 / (T_celsius + 273.15 + A3)) / 100000
/// @param r       Refrigerant enum
/// @param t_c     Temperature in °C
/// @return        Absolute pressure in bar, or NAN on error
float saturation_pressure(Refrigerant r, float t_c);

/// Inverse with explicit constants.
float saturation_pressure(const AntoineConstants& c, float t_c);

}  // namespace modesp
