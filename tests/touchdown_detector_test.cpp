// Pure-logic test for sync/touchdown_detector.h - a normal landing, a
// bounced one, a touch-and-go, hops during the takeoff roll and a
// reposition not counting as landings.

#include "sync/touchdown_detector.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

using namespace flytogether;

namespace {

constexpr double kDt = 1.0 / 30.0;

// A simple aircraft sim: flies along a line northwards at `gs_kt`.
struct Sim {
    TouchdownDetector detector;
    TouchdownSample s;
    std::vector<LandingResult> results;

    Sim() {
        s.latitude = 50.0;
        s.longitude = 8.0;
        s.groundspeed_kt = 60.0;
        s.heading_true_deg = 0.0;
        s.track_true_deg = 0.0;
    }

    void Step(bool on_ground, double agl_m, double vs_fpm, double g = 1.0) {
        s.time_s += kDt;
        s.latitude += s.groundspeed_kt * 0.514444 * kDt / 111195.0;
        s.on_ground = on_ground;
        s.agl_m = agl_m;
        s.vs_fpm = vs_fpm;
        s.g_normal = g;
        if (const auto r = detector.Update(s)) {
            results.push_back(*r);
        }
    }
    void Hold(double seconds, bool on_ground, double agl_m, double vs_fpm = 0.0, double g = 1.0) {
        for (double t = 0; t < seconds; t += kDt) Step(on_ground, agl_m, vs_fpm, g);
    }
    // Climb out, then descend through 50 ft at -500 fpm and flare to `touch_fpm`.
    void FlyApproach(double touch_fpm) {
        Hold(5.0, true, 0.0);
        Hold(20.0, false, 100.0, 500.0);
        double agl = 30.0;
        while (agl > 1.0) {
            const double fpm = agl > 5.0 ? -500.0 : touch_fpm;
            agl += fpm / 196.85 * kDt;
            Step(false, std::max(agl, 0.5), fpm);
        }
    }
};

bool Near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

} // namespace

int main() {
    // Normal landing: one result, a few seconds after touchdown.
    {
        Sim sim;
        sim.s.track_true_deg = 3.0; // drifting right
        sim.FlyApproach(-150.0);
        sim.Step(true, 0.0, -40.0, 1.35);
        sim.Hold(0.3, true, 0.0, 0.0, 1.35); // a load held for a moment counts in full
        sim.Hold(0.2, true, 0.0);
        assert(sim.results.empty()); // not settled yet
        sim.Hold(3.0, true, 0.0);
        assert(sim.results.size() == 1);
        const LandingResult& r = sim.results[0];
        assert(Near(r.vs_fpm, -150.0, 0.1));
        assert(Near(r.peak_g, 1.35, 1e-9));
        assert(Near(r.drift_deg, 3.0, 1e-6));
        assert(r.bounces == 0 && !r.touch_and_go);
        assert(r.flare_distance_m > 50.0 && r.flare_distance_m < 400.0);
        sim.Hold(10.0, true, 0.0);
        assert(sim.results.size() == 1); // reported once
    }

    // Bounced landing: two bounces, peak G from the second contact.
    {
        Sim sim;
        sim.FlyApproach(-400.0);
        sim.Hold(0.3, true, 0.0, -100.0, 1.6);
        sim.Hold(0.8, false, 1.0, 100.0);
        sim.Hold(0.3, true, 0.0, -200.0, 1.9);
        sim.Hold(0.4, false, 0.5, 50.0);
        sim.Hold(0.1, false, 0.1, 0.0); // flicker below kBounceMinAirS doesn't count on its own
        sim.Hold(4.0, true, 0.0);
        assert(sim.results.size() == 1);
        assert(sim.results[0].bounces == 2);
        assert(Near(sim.results[0].peak_g, 1.9, 1e-9));
        assert(Near(sim.results[0].vs_fpm, -400.0, 0.1));
    }

    // X-Plane's one-frame G spike at gear contact is smoothed away.
    {
        Sim sim;
        sim.FlyApproach(-150.0);
        sim.Step(true, 0.0, -60.0, 2.66);
        sim.Hold(4.0, true, 0.0, 0.0, 1.05);
        assert(sim.results.size() == 1);
        assert(sim.results[0].peak_g < 1.35);
        assert(sim.results[0].peak_g > 1.0);
    }

    // Gear-contact flicker right at touchdown is not a bounce.
    {
        Sim sim;
        sim.FlyApproach(-120.0);
        sim.Step(true, 0.0, -60.0);
        sim.Step(false, 0.05, 0.0);
        sim.Step(true, 0.0, 0.0);
        sim.Hold(4.0, true, 0.0);
        assert(sim.results.size() == 1 && sim.results[0].bounces == 0);
    }

    // Touch-and-go: reported when airborne again, the next landing counts too.
    {
        Sim sim;
        sim.FlyApproach(-200.0);
        sim.Hold(1.5, true, 0.0);
        sim.Hold(6.0, false, 30.0, 600.0);
        assert(sim.results.size() == 1 && sim.results[0].touch_and_go);
        sim.Hold(30.0, false, 300.0);
        double agl = 30.0;
        while (agl > 1.0) {
            agl -= 300.0 / 196.85 * kDt;
            sim.Step(false, std::max(agl, 0.5), -300.0);
        }
        sim.Hold(4.0, true, 0.0);
        assert(sim.results.size() == 2 && !sim.results[1].touch_and_go);
        assert(Near(sim.results[1].vs_fpm, -300.0, 0.1));
    }

    // A short hop during the takeoff roll is no landing.
    {
        Sim sim;
        sim.Hold(3.0, true, 0.0);
        sim.Hold(1.0, false, 2.0, 200.0);
        sim.Hold(5.0, true, 0.0);
        assert(sim.results.empty());
    }

    // Placed on final by the map (a position jump): needs to fly the arming
    // time before a touchdown counts, and the jump itself is no landing.
    {
        Sim sim;
        sim.Hold(3.0, true, 0.0);
        sim.s.latitude += 0.5;
        sim.Hold(1.0, false, 50.0, -500.0);
        sim.Hold(4.0, true, 0.0);
        assert(sim.results.empty());
    }

    // Wrap-around drift: heading 359, track 2 = +3 right.
    {
        Sim sim;
        sim.s.heading_true_deg = 359.0;
        sim.s.track_true_deg = 2.0;
        sim.FlyApproach(-100.0);
        sim.Step(true, 0.0, -50.0);
        sim.Hold(4.0, true, 0.0);
        assert(sim.results.size() == 1 && Near(sim.results[0].drift_deg, 3.0, 1e-6));
    }

    assert(Near(DistanceMeters(50.0, 8.0, 51.0, 8.0), 111195.0, 10.0));

    std::printf("touchdown_detector_test: all checks passed\n");
    return 0;
}
