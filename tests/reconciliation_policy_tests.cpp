#include "metaplasia/host/reconciliation_policy.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    using namespace std::chrono_literals;
    const std::chrono::steady_clock::time_point observed{};

    Require(
        metaplasia::host::ShouldDeferStartMenuInitialization(
            true,
            true,
            true,
            observed,
            observed + 4999ms),
        "defer enabled Start customization during XAML startup");
    Require(
        !metaplasia::host::ShouldDeferStartMenuInitialization(
            true,
            true,
            true,
            observed,
            observed + 5s),
        "allow Start initialization after the bounded grace period");
    Require(
        !metaplasia::host::ShouldDeferStartMenuInitialization(
            true,
            false,
            true,
            observed,
            observed + 1s),
        "never defer the Explorer and Taskbar process path");
    Require(
        !metaplasia::host::ShouldDeferStartMenuInitialization(
            true,
            true,
            false,
            observed,
            observed + 1s),
        "do not defer Start deactivation");
    Require(
        !metaplasia::host::ShouldDeferStartMenuInitialization(
            false,
            true,
            true,
            observed,
            observed + 1s),
        "do not defer a stopped process");
}
