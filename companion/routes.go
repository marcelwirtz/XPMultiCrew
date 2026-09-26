package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// Route planning on the map page: export to X-Plane's .fms format (loadable
// in the G1000, GNS 430/530 and FMS) and sharing a route with the
// Multiplayer session (the plugin relays the encoded text as-is).

// RouteWaypoint is one point of a planned route.
type RouteWaypoint struct {
	Kind  string  `json:"kind"` // APT, VRP, VOR, NDB, USR
	Ident string  `json:"ident"`
	Name  string  `json:"name"`
	Lat   float64 `json:"lat"`
	Lon   float64 `json:"lon"`
}

// PlannedRoute is the map's route.
type PlannedRoute struct {
	Name      string          `json:"name"`
	CruiseFt  int             `json:"cruiseFt"`
	TasKt     int             `json:"tasKt"`
	Waypoints []RouteWaypoint `json:"waypoints"`
}

// SharedRoute is a route someone in the session shared (FromSenderID 0 =
// this side's own shared route).
type SharedRoute struct {
	FromSenderID uint32       `json:"fromSenderId"`
	Route        PlannedRoute `json:"route"`
}

const (
	maxRouteWaypoints   = 40
	maxRoutePayloadSize = 2000 // one relay datagram, see the plugin's ROUTE_SHARE
)

var routeKinds = map[string]bool{"APT": true, "VRP": true, "VOR": true, "NDB": true, "USR": true}

// cleanRouteText keeps route text safe for the line-based plugin protocol
// and the encoding below (no separators, no newlines).
func cleanRouteText(s string, max int) string {
	var b strings.Builder
	for _, r := range s {
		if r == ',' || r == ';' || r == '|' || r == '\n' || r == '\r' || r < 32 {
			continue
		}
		b.WriteRune(r)
	}
	out := strings.TrimSpace(b.String())
	if len(out) > max {
		out = out[:max]
	}
	return out
}

// encodeRoute: "v1|<name>|<cruiseFt>|<tasKt>|<kind>,<ident>,<name>,<lat>,<lon>;..."
func encodeRoute(r PlannedRoute) (string, error) {
	if len(r.Waypoints) < 2 {
		return "", errors.New("a route needs at least two waypoints")
	}
	if len(r.Waypoints) > maxRouteWaypoints {
		return "", fmt.Errorf("a shared route can have at most %d waypoints", maxRouteWaypoints)
	}
	parts := make([]string, 0, len(r.Waypoints))
	for _, w := range r.Waypoints {
		kind := w.Kind
		if !routeKinds[kind] {
			kind = "USR"
		}
		parts = append(parts, fmt.Sprintf("%s,%s,%s,%.5f,%.5f", kind, cleanRouteText(w.Ident, 12),
			cleanRouteText(w.Name, 24), w.Lat, w.Lon))
	}
	payload := fmt.Sprintf("v1|%s|%d|%d|%s", cleanRouteText(r.Name, 32), r.CruiseFt, r.TasKt, strings.Join(parts, ";"))
	payload = strings.ReplaceAll(payload, " ", "_") // the plugin command is whitespace-tokenised
	if len(payload) > maxRoutePayloadSize {
		return "", errors.New("route too long to share - remove a few waypoints")
	}
	return payload, nil
}

func decodeRoute(payload string) (PlannedRoute, bool) {
	payload = strings.ReplaceAll(payload, "_", " ")
	f := strings.SplitN(payload, "|", 5)
	if len(f) != 5 || f[0] != "v1" {
		return PlannedRoute{}, false
	}
	r := PlannedRoute{Name: f[1], Waypoints: []RouteWaypoint{}}
	r.CruiseFt, _ = strconv.Atoi(f[2])
	r.TasKt, _ = strconv.Atoi(f[3])
	for _, w := range strings.Split(f[4], ";") {
		p := strings.Split(w, ",")
		if len(p) != 5 || !routeKinds[p[0]] {
			continue
		}
		lat, err1 := strconv.ParseFloat(p[3], 64)
		lon, err2 := strconv.ParseFloat(p[4], 64)
		if err1 != nil || err2 != nil || lat < -90 || lat > 90 || lon < -180 || lon > 180 {
			continue
		}
		r.Waypoints = append(r.Waypoints, RouteWaypoint{Kind: p[0], Ident: p[1], Name: p[2], Lat: lat, Lon: lon})
	}
	if len(r.Waypoints) < 2 {
		return PlannedRoute{}, false
	}
	return r, true
}

var fmsNameUnsafe = regexp.MustCompile(`[^A-Za-z0-9_-]+`)

// navCycle reads the AIRAC cycle from earth_fix.dat's header ("... data
// cycle 2406 ..."), which the .fms file should declare.
func navCycle(xplaneRoot string) string {
	fix, _, _ := navFiles(xplaneRoot)
	f, err := os.Open(fix)
	if err != nil {
		return "2406"
	}
	defer f.Close()
	s := bufio.NewScanner(f)
	for i := 0; i < 3 && s.Scan(); i++ {
		if _, after, ok := strings.Cut(s.Text(), "data cycle "); ok {
			if fields := strings.Fields(after); len(fields) > 0 {
				return strings.TrimRight(fields[0], ",")
			}
		}
	}
	return "2406"
}

// fmsRoute renders X-Plane's v1100 .fms format. Type codes: 1 airport,
// 2 NDB, 3 VOR, 11 fix (VFR reporting points are fixes in X-Plane's
// database), 28 lat/lon.
func fmsRoute(r PlannedRoute, cycle string) (string, error) {
	if len(r.Waypoints) < 2 {
		return "", errors.New("a route needs at least two waypoints")
	}
	var b strings.Builder
	b.WriteString("I\n1100 Version\n")
	fmt.Fprintf(&b, "CYCLE %s\n", cycle)
	first, last := r.Waypoints[0], r.Waypoints[len(r.Waypoints)-1]
	if first.Kind == "APT" {
		fmt.Fprintf(&b, "ADEP %s\n", first.Ident)
	} else {
		fmt.Fprintf(&b, "DEP %s\n", fmsIdent(first))
	}
	if last.Kind == "APT" {
		fmt.Fprintf(&b, "ADES %s\n", last.Ident)
	} else {
		fmt.Fprintf(&b, "DES %s\n", fmsIdent(last))
	}
	fmt.Fprintf(&b, "NUMENR %d\n", len(r.Waypoints))
	for i, w := range r.Waypoints {
		code := map[string]int{"APT": 1, "NDB": 2, "VOR": 3, "VRP": 11}[w.Kind]
		if code == 0 {
			code = 28
		}
		via := "DRCT"
		alt := float64(r.CruiseFt)
		if code == 1 {
			alt = 0
			if i == 0 {
				via = "ADEP"
			} else if i == len(r.Waypoints)-1 {
				via = "ADES"
			}
		}
		fmt.Fprintf(&b, "%d %s %s %.6f %.6f %.6f\n", code, fmsIdent(w), via, alt, w.Lat, w.Lon)
	}
	return b.String(), nil
}

func fmsIdent(w RouteWaypoint) string {
	if w.Kind == "USR" || w.Ident == "" {
		// X-Plane's own convention for lat/lon waypoints.
		return fmt.Sprintf("%+07.3f_%+08.3f", w.Lat, w.Lon)
	}
	return strings.ReplaceAll(w.Ident, " ", "")
}

func exportFms(xplaneRoot string, r PlannedRoute) (string, error) {
	content, err := fmsRoute(r, navCycle(xplaneRoot))
	if err != nil {
		return "", err
	}
	name := r.Name
	if name == "" {
		name = fmsIdent(r.Waypoints[0]) + "-" + fmsIdent(r.Waypoints[len(r.Waypoints)-1])
	}
	name = strings.Trim(fmsNameUnsafe.ReplaceAllString(name, "_"), "_")
	if name == "" {
		name = "route"
	}
	dir := filepath.Join(xplaneRoot, "Output", "FMS plans")
	if err := os.MkdirAll(dir, 0755); err != nil {
		return "", err
	}
	file := name + ".fms"
	if err := os.WriteFile(filepath.Join(dir, file), []byte(content), 0644); err != nil {
		return "", err
	}
	return file, nil
}
