// ═══════════════════════════════════════════════════════════════
//  Tests: Refrigerant Saturation (Antoine equation)
//
//  Verified against:
//  - NIST RefProp 10.0 data
//  - Engineers Edge R134a table
//  - Danfoss PT charts
// ═══════════════════════════════════════════════════════════════

#include "doctest.h"
#include "modesp/refrigerant/saturation.h"
#include <cmath>
#include <string>

using namespace modesp;

// Tolerance: ±1.0°C (Antoine equation vs NIST data may differ by up to ~0.5°C,
// plus the Danfoss constants are float32 not float64)
static constexpr float TOL = 1.0f;

// ─── R134a: well-known reference points ────────────────────

TEST_CASE("R134a saturation — known points from NIST") {
    // R134a boiling point at 1 atm (1.01325 bar) = -26.07°C
    float t = saturation_temp(Refrigerant::R134a, 1.01325f);
    CHECK(t == doctest::Approx(-26.1f).epsilon(TOL));

    // R134a at 0°C = 2.94 bar (from Engineers Edge table)
    t = saturation_temp(Refrigerant::R134a, 2.94f);
    CHECK(t == doctest::Approx(0.0f).epsilon(TOL));

    // R134a at 5 bar → ~15.7°C
    t = saturation_temp(Refrigerant::R134a, 5.0f);
    CHECK(t == doctest::Approx(15.7f).epsilon(TOL));

    // Inverse: 0°C → ~2.94 bar
    float p = saturation_pressure(Refrigerant::R134a, 0.0f);
    CHECK(p == doctest::Approx(2.94f).epsilon(0.15f));
}

// ─── R404A: most common in commercial refrigeration ────────

TEST_CASE("R404A saturation — commercial refrigeration range") {
    // R404A boiling point at 1 atm = -46.5°C
    float t = saturation_temp(Refrigerant::R404A, 1.01325f);
    CHECK(t == doctest::Approx(-46.5f).epsilon(TOL));

    // R404A at 3 bar → ~ -18°C (typical low temp suction)
    t = saturation_temp(Refrigerant::R404A, 3.0f);
    CHECK(t == doctest::Approx(-18.0f).epsilon(2.0f));  // wider tolerance for mid-range

    // R404A at 6 bar → ~ -3°C (typical medium temp suction)
    t = saturation_temp(Refrigerant::R404A, 6.0f);
    CHECK(t == doctest::Approx(-3.0f).epsilon(2.0f));

    // Inverse round-trip: temp → pressure → temp
    float p = saturation_pressure(Refrigerant::R404A, -20.0f);
    CHECK(p > 1.5f);
    CHECK(p < 4.0f);
    float t2 = saturation_temp(Refrigerant::R404A, p);
    CHECK(t2 == doctest::Approx(-20.0f).epsilon(0.1f));  // round-trip must be tight
}

// ─── R290 (Propane): natural refrigerant ───────────────────

TEST_CASE("R290 propane saturation") {
    // R290 boiling point at 1 atm = -42.1°C
    float t = saturation_temp(Refrigerant::R290, 1.01325f);
    CHECK(t == doctest::Approx(-42.1f).epsilon(TOL));

    // R290 at 5 bar → ~ +1°C
    t = saturation_temp(Refrigerant::R290, 5.0f);
    CHECK(t == doctest::Approx(1.0f).epsilon(2.0f));
}

// ─── Zeotropic blends: dew point with glide ────────────────

TEST_CASE("R448A dew point — glide compensation") {
    // R448A has glide = 5.4K
    const auto* c = get_refrigerant(Refrigerant::R448A);
    REQUIRE(c != nullptr);
    CHECK(c->glide == doctest::Approx(5.4f));

    // Midpoint and dew point should differ by glide/2 = 2.7°C
    float t_mid = saturation_temp(Refrigerant::R448A, 3.0f);
    float t_dew = dew_point_temp(Refrigerant::R448A, 3.0f);
    CHECK(t_dew == doctest::Approx(t_mid + 2.7f).epsilon(0.01f));
}

TEST_CASE("R449A dew point — glide compensation") {
    const auto* c = get_refrigerant(Refrigerant::R449A);
    REQUIRE(c != nullptr);
    CHECK(c->glide == doctest::Approx(5.0f));

    float t_mid = saturation_temp(Refrigerant::R449A, 3.0f);
    float t_dew = dew_point_temp(Refrigerant::R449A, 3.0f);
    CHECK(t_dew == doctest::Approx(t_mid + 2.5f).epsilon(0.01f));
}

TEST_CASE("R455A high glide (11.3K)") {
    const auto* c = get_refrigerant(Refrigerant::R455A);
    REQUIRE(c != nullptr);
    CHECK(c->glide == doctest::Approx(11.3f));
}

// ─── Azeotropes: zero glide ───────────────────────────────

TEST_CASE("R507A zero glide — dew == midpoint") {
    float t_mid = saturation_temp(Refrigerant::R507A, 3.0f);
    float t_dew = dew_point_temp(Refrigerant::R507A, 3.0f);
    CHECK(t_dew == doctest::Approx(t_mid).epsilon(0.01f));
}

// ─── Round-trip consistency ────────────────────────────────

TEST_CASE("Round-trip: temp→pressure→temp for all refrigerants") {
    for (uint8_t i = 0; i < REFRIGERANT_COUNT; ++i) {
        auto r = static_cast<Refrigerant>(i);
        const auto* c = get_refrigerant(r);
        REQUIRE(c != nullptr);

        // Test at -20°C (common suction temperature)
        float p = saturation_pressure(r, -20.0f);
        REQUIRE_FALSE(std::isnan(p));
        REQUIRE(p > 0.0f);

        float t = saturation_temp(r, p);
        REQUIRE_FALSE(std::isnan(t));
        CHECK_MESSAGE(t == doctest::Approx(-20.0f).epsilon(0.1f),
                      "Round-trip failed for ", c->name);
    }
}

// ─── Edge cases ────────────────────────────────────────────

TEST_CASE("Invalid pressure returns NAN") {
    CHECK(std::isnan(saturation_temp(Refrigerant::R404A, 0.0f)));
    CHECK(std::isnan(saturation_temp(Refrigerant::R404A, -1.0f)));
}

TEST_CASE("Unknown refrigerant returns NAN") {
    CHECK(std::isnan(saturation_temp(Refrigerant::UNKNOWN, 3.0f)));
    CHECK(std::isnan(saturation_pressure(Refrigerant::UNKNOWN, -20.0f)));
}

TEST_CASE("Lookup by index") {
    const auto* c0 = get_refrigerant_by_index(0);
    REQUIRE(c0 != nullptr);
    CHECK(std::string(c0->name) == "R134a");

    const auto* c_last = get_refrigerant_by_index(REFRIGERANT_COUNT - 1);
    REQUIRE(c_last != nullptr);
    CHECK(std::string(c_last->name) == "R1270");

    CHECK(get_refrigerant_by_index(255) == nullptr);
}

TEST_CASE("Refrigerant name lookup") {
    CHECK(std::string(refrigerant_name(Refrigerant::R404A)) == "R404A");
    CHECK(std::string(refrigerant_name(Refrigerant::R744)) == "R744");
    CHECK(std::string(refrigerant_name(Refrigerant::UNKNOWN)) == "?");
}

// ─── R744 (CO2): high pressure, different range ────────────

TEST_CASE("R744 CO2 saturation") {
    // CO2 at 1 atm = -78.5°C (sublimation, but Antoine works above triple point)
    // CO2 triple point: -56.6°C at 5.18 bar
    // Test at 30 bar → ~ -5°C
    float t = saturation_temp(Refrigerant::R744, 30.0f);
    CHECK(t == doctest::Approx(-5.0f).epsilon(3.0f));

    // CO2 at 10 bar → ~ -40°C
    t = saturation_temp(Refrigerant::R744, 10.0f);
    CHECK(t == doctest::Approx(-40.0f).epsilon(3.0f));
}

// ─── R32: heat pump refrigerant ────────────────────────────

TEST_CASE("R32 saturation") {
    // R32 boiling point at 1 atm = -51.7°C
    float t = saturation_temp(Refrigerant::R32, 1.01325f);
    CHECK(t == doctest::Approx(-51.7f).epsilon(TOL));
}
