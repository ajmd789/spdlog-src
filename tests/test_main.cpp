#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

// Basic smoke test
TEST_CASE("testing the json library") {
    nlohmann::json j;
    j["test"] = "hello";
    CHECK(j["test"] == "hello");
}

TEST_CASE("testing spdlog") {
    spdlog::info("This is a test log message from doctest");
    CHECK(true);
}
