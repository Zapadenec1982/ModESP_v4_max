// Standalone saturation test — no ModESP dependencies
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "modesp/refrigerant/saturation.h"
#include <cmath>
#include <string>

using namespace modesp;

static constexpr float TOL = 1.0f;

TEST_CASE("R134a known points") {
    float t = saturation_temp(Refrigerant::R134a, 1.01325f);
    CHECK(t == doctest::Approx(-26.1f).epsilon(TOL));
    t = saturation_temp(Refrigerant::R134a, 2.94f);
    CHECK(t == doctest::Approx(0.0f).epsilon(TOL));
}

TEST_CASE("R404A boiling point") {
    float t = saturation_temp(Refrigerant::R404A, 1.01325f);
    CHECK(t == doctest::Approx(-46.5f).epsilon(TOL));
}

TEST_CASE("R290 propane boiling point") {
    float t = saturation_temp(Refrigerant::R290, 1.01325f);
    CHECK(t == doctest::Approx(-42.1f).epsilon(TOL));
}

TEST_CASE("R32 boiling point") {
    float t = saturation_temp(Refrigerant::R32, 1.01325f);
    CHECK(t == doctest::Approx(-51.7f).epsilon(TOL));
}

TEST_CASE("R448A glide = 5.4K") {
    const auto* c = get_refrigerant(Refrigerant::R448A);
    REQUIRE(c != nullptr);
    CHECK(c->glide == doctest::Approx(5.4f));
    float t_mid = saturation_temp(Refrigerant::R448A, 3.0f);
    float t_dew = dew_point_temp(Refrigerant::R448A, 3.0f);
    CHECK(t_dew == doctest::Approx(t_mid + 2.7f).epsilon(0.01f));
}

TEST_CASE("Round-trip all refrigerants at -20C") {
    for (uint8_t i = 0; i < REFRIGERANT_COUNT; ++i) {
        auto r = static_cast<Refrigerant>(i);
        const auto* c = get_refrigerant(r);
        REQUIRE(c != nullptr);
        float p = saturation_pressure(r, -20.0f);
        REQUIRE_FALSE(std::isnan(p));
        REQUIRE(p > 0.0f);
        float t = saturation_temp(r, p);
        REQUIRE_FALSE(std::isnan(t));
        CHECK_MESSAGE(t == doctest::Approx(-20.0f).epsilon(0.1f),
                      "Round-trip failed for ", c->name);
    }
}

TEST_CASE("Invalid inputs") {
    CHECK(std::isnan(saturation_temp(Refrigerant::R404A, 0.0f)));
    CHECK(std::isnan(saturation_temp(Refrigerant::R404A, -1.0f)));
    CHECK(std::isnan(saturation_temp(Refrigerant::UNKNOWN, 3.0f)));
}

TEST_CASE("Lookup by name") {
    CHECK(std::string(refrigerant_name(Refrigerant::R404A)) == "R404A");
    CHECK(std::string(refrigerant_name(Refrigerant::R744)) == "R744");
    CHECK(std::string(refrigerant_name(Refrigerant::UNKNOWN)) == "?");
}
