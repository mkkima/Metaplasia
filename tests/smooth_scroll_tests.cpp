#include "metaplasia/agent/smooth_scroll.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void Require(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
bool Near(double left, double right) { return std::abs(left - right) < 0.000001; }
}
int main() {
    using namespace metaplasia::agent;
    SmoothScrollAnimation animation;
    Require(!animation.Active(0), "no animation or timer work while idle");
    Require(animation.Retarget(20, 10, 100), "start a wheel animation");
    Require(Near(animation.Position(100), 20), "no initial jump");
    double previous = 20;
    for (std::uint64_t time = 101; time <= 260; ++time) {
        const double current = animation.Position(time);
        Require(current >= previous && current <= 30, "monotonic easing without overshoot");
        previous = current;
    }
    Require(Near(animation.Position(260), 30) && !animation.Active(260), "exact final position and idle deadline");
    Require(animation.Retarget(30, 10, 300), "start next animation");
    const double at_retarget = animation.Position(340);
    Require(animation.Retarget(at_retarget, 10, 340), "accumulate rapid wheel input");
    Require(Near(animation.Position(340), at_retarget) && Near(animation.Target(), 50), "retarget continuously without losing wheel distance");
    const double at_reverse = animation.Position(360);
    Require(animation.Retarget(at_reverse, -5, 360), "reverse direction immediately");
    Require(Near(animation.Target(), at_reverse - 5), "reversal cancels old forward momentum");
    animation.Cancel();
    Require(!animation.Active(360), "cancel restores idle immediately");
    Require(!animation.Retarget(100, 20, 500), "bottom edge cannot overscroll");
    Require(!animation.Retarget(0, -20, 500), "top edge cannot overscroll");
    Require(!animation.Retarget(-1, 10, 500), "unsupported provider position rejected");
    Require(!animation.Retarget(10, std::numeric_limits<double>::quiet_NaN(), 500), "nonfinite motion rejected");
    auto delta = SmoothWheelPercent(-120, 3, 20, 600, 24);
    Require(delta && Near(*delta, 3), "system wheel line count maps to content range");
    Require(Near(*SmoothWheelPercent(120, 3, 20, 600, 24), -3), "wheel direction preserved");
    Require(!SmoothWheelPercent(30, 3, 20, 600, 24), "precision wheels and touchpads stay native");
    Require(!SmoothWheelPercent(-120, 0, 20, 600, 24), "disabled system scrolling stays native");
    Require(!SmoothWheelPercent(-120, 3, 100, 600, 24), "non-scrollable views stay native");
    Require(!SmoothWheelPercent(-120, 3, 20, 0, 24), "invalid viewport rejected");
    Require(Near(*SmoothWheelPercent(-120, UINT32_MAX, 20, 600, 24), 24), "page-scroll system preference supported");
    Require(Near(*SmoothWheelPercent(-120, 3, 20, 750, 30), *delta), "wheel distance is DPI independent");
    std::cout << "Explorer smooth-scroll policy tests passed\n";
}
