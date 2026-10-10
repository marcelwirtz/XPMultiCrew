#pragma once

// "host --airport EDDK" instead of typing coordinates: finds the airport in
// the user's own X-Plane (Global Scenery/Global Airports/Earth nav data/
// apt.dat) and lines the fake up on one of its runways. The X-Plane folder
// comes from --xplane, else from the companion's config.json (xplanePath).
//
// apt.dat only has the field elevation, not each runway end's - close
// enough: a Shared Cockpit co-pilot follows its own ground near the surface
// (plugin_main.cpp's SharedCockpitGroundOffsetM).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace airport_lookup {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusM = 6378137.0;

struct RunwayEnd {
    std::string name;
    double lat = 0.0, lon = 0.0;
    double displaced_m = 0.0;
};

struct Runway {
    RunwayEnd end[2];
};

struct Airport {
    std::string ident, name;
    double elevation_m = 0.0;
    std::vector<Runway> runways;
};

struct Placement {
    double lat = 0.0, lon = 0.0;
    double heading_deg = 0.0; // true
    double ground_m = 0.0;    // field elevation, meters MSL
    std::string description;
};

inline std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The companion's settings file (companion/config.go: os.UserConfigDir()
// + xpmulticrew-companion/config.json).
inline std::string CompanionConfigPath() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    return appdata ? std::string(appdata) + "\\xpmulticrew-companion\\config.json" : "";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Library/Application Support/xpmulticrew-companion/config.json" : "";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/xpmulticrew-companion/config.json";
    }
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.config/xpmulticrew-companion/config.json" : "";
#endif
}

// Just the one string field - not worth a JSON parser here.
inline std::string XPlaneRootFromCompanion() {
    std::ifstream in(CompanionConfigPath());
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const auto key = text.find("\"xplanePath\"");
    if (key == std::string::npos) return "";
    const auto open = text.find('"', text.find(':', key) + 1);
    if (open == std::string::npos) return "";
    std::string out;
    for (size_t i = open + 1; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) ++i; // JSON escapes (Windows backslashes)
        out += text[i];
    }
    return out;
}

inline std::vector<std::string> Fields(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> out;
    for (std::string f; in >> f;) out.push_back(f);
    return out;
}

// Streams the (large) apt.dat once and keeps only the wanted airport.
inline std::optional<Airport> FindAirport(const std::string& apt_dat, const std::string& ident, std::string& error) {
    std::ifstream in(apt_dat);
    if (!in) {
        error = "can't read " + apt_dat;
        return std::nullopt;
    }
    const std::string want = Upper(ident);
    std::optional<Airport> found;
    for (std::string line; std::getline(in, line);) {
        if (line.size() < 2) continue;
        const bool header = line.compare(0, 2, "1 ") == 0 || line.compare(0, 3, "16 ") == 0 ||
                            line.compare(0, 3, "17 ") == 0 || line.compare(0, 2, "99") == 0;
        if (header) {
            if (found) break; // past our airport
            const auto f = Fields(line);
            if (f.size() >= 5 && f[0] == "1" && Upper(f[4]) == want) {
                found = Airport{};
                found->ident = f[4];
                found->elevation_m = std::atof(f[1].c_str()) * 0.3048;
                for (size_t i = 5; i < f.size(); ++i) found->name += (i > 5 ? " " : "") + f[i];
            }
            continue;
        }
        if (!found || line.compare(0, 4, "100 ") != 0) continue;
        const auto f = Fields(line);
        if (f.size() < 21) continue;
        Runway r;
        for (int e = 0; e < 2; ++e) {
            const size_t b = 8 + 9 * e;
            r.end[e] = {f[b], std::atof(f[b + 1].c_str()), std::atof(f[b + 2].c_str()), std::atof(f[b + 3].c_str())};
        }
        found->runways.push_back(r);
    }
    if (!found) {
        error = "airport " + want + " not found in " + apt_dat;
    } else if (found->runways.empty()) {
        error = want + " has no land runway";
        found.reset();
    }
    return found;
}

inline double DistanceM(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kPi / 180, p2 = lat2 * kPi / 180;
    const double dp = p2 - p1, dl = (lon2 - lon1) * kPi / 180;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kEarthRadiusM * std::asin(std::sqrt(a));
}

inline double BearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kPi / 180, p2 = lat2 * kPi / 180, dl = (lon2 - lon1) * kPi / 180;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    return std::fmod(std::atan2(y, x) * 180 / kPi + 360.0, 360.0);
}

// "08" == "8", "08L" == "8L".
inline bool SameRunwayName(std::string a, std::string b) {
    const auto strip = [](std::string s) {
        s = Upper(s);
        while (s.size() > 1 && s[0] == '0') s.erase(0, 1);
        return s;
    };
    return strip(a) == strip(b);
}

// On `runway` (empty = the longest one), just past its threshold, facing
// down the runway.
inline std::optional<Placement> PlaceOnRunway(const Airport& apt, const std::string& runway, std::string& error) {
    const Runway* best = nullptr;
    int end = 0;
    double best_len = -1.0;
    for (const auto& r : apt.runways) {
        const double len = DistanceM(r.end[0].lat, r.end[0].lon, r.end[1].lat, r.end[1].lon);
        for (int e = 0; e < 2; ++e) {
            if (!runway.empty() ? SameRunwayName(r.end[e].name, runway) : (e == 0 && len > best_len)) {
                best = &r;
                end = e;
                best_len = len;
            }
        }
    }
    if (!best) {
        std::string names;
        for (const auto& r : apt.runways) names += " " + r.end[0].name + "/" + r.end[1].name;
        error = apt.ident + " has no runway " + runway + " (runways:" + names + ")";
        return std::nullopt;
    }
    const RunwayEnd& from = best->end[end];
    const RunwayEnd& to = best->end[1 - end];
    Placement p;
    p.heading_deg = BearingDeg(from.lat, from.lon, to.lat, to.lon);
    const double along = from.displaced_m + 30.0; // a little past the threshold
    const double h = p.heading_deg * kPi / 180;
    p.lat = from.lat + along * std::cos(h) / kEarthRadiusM * 180 / kPi;
    p.lon = from.lon + along * std::sin(h) / (kEarthRadiusM * std::cos(from.lat * kPi / 180)) * 180 / kPi;
    p.ground_m = apt.elevation_m;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s %s, runway %s (%.0f m), heading %.0f true, field elevation %.0f m",
                  apt.ident.c_str(), apt.name.c_str(), from.name.c_str(), best_len, p.heading_deg, p.ground_m);
    p.description = buf;
    return p;
}

inline std::optional<Placement> Resolve(std::string xplane_root, const std::string& ident, const std::string& runway,
                                        std::string& error) {
    if (xplane_root.empty()) xplane_root = XPlaneRootFromCompanion();
    if (xplane_root.empty()) {
        error = "X-Plane folder unknown - pass --xplane <folder> (or set it in the companion)";
        return std::nullopt;
    }
    const std::string apt_dat = xplane_root + "/Global Scenery/Global Airports/Earth nav data/apt.dat";
    const auto apt = FindAirport(apt_dat, ident, error);
    if (!apt) return std::nullopt;
    return PlaceOnRunway(*apt, runway, error);
}

} // namespace airport_lookup
