package main

import (
	"bufio"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
)

// VFR map data read at runtime from the user's own X-Plane installation
// (Resources/default data - Navigraph data X-Plane ships), never bundled:
//   - earth_fix.dat: VFR reporting points (ARINC 424 waypoint type 'V')
//   - earth_nav.dat: VORs and NDBs
//   - airspaces/airspace.txt: OpenAir-style airspaces (polygons only in
//     X-Plane's file; limits "<n> MSL" or "GND")
// Small enough to parse on first use and keep in memory.

// NavPoint is a VFR reporting point, VOR or NDB for the map/route planner.
type NavPoint struct {
	Kind    string  `json:"kind"` // "VRP", "VOR", "NDB"
	Ident   string  `json:"ident"`
	Name    string  `json:"name"`
	Airport string  `json:"airport,omitempty"` // VRPs: the airport they belong to
	Lat     float64 `json:"lat"`
	Lon     float64 `json:"lon"`
	Freq    string  `json:"freq,omitempty"`   // "114.20" / "401"
	MagVar  float64 `json:"magVar,omitempty"` // VORs: station declination, degrees east positive
}

// Airspace is one polygon from airspace.txt.
type Airspace struct {
	Name     string       `json:"name"`
	Class    string       `json:"class"` // A, B, C, D, CTR, P (prohibited), R (restricted), Q (danger)
	LowerFt  int          `json:"lowerFt"`
	LowerGnd bool         `json:"lowerGnd"`
	UpperFt  int          `json:"upperFt"`
	UpperGnd bool         `json:"-"`
	Poly     [][2]float64 `json:"poly"` // [lon, lat]
	minLon   float64
	minLat   float64
	maxLon   float64
	maxLat   float64
}

// NavData is what GetNavData sends to the map (airspaces go separately,
// per visible area - there are ~24k of them).
type NavData struct {
	Points []NavPoint `json:"points"`
}

type navCache struct {
	key       string
	points    []NavPoint
	airspaces []Airspace
}

var (
	navMu    sync.Mutex
	navState *navCache
)

func defaultDataDir(xplaneRoot string) string {
	return filepath.Join(xplaneRoot, "Resources", "default data")
}

func navFiles(xplaneRoot string) (fix, nav, air string) {
	dir := defaultDataDir(xplaneRoot)
	return filepath.Join(dir, "earth_fix.dat"), filepath.Join(dir, "earth_nav.dat"),
		filepath.Join(dir, "airspaces", "airspace.txt")
}

func fileKey(paths ...string) string {
	var b strings.Builder
	for _, p := range paths {
		if st, err := os.Stat(p); err == nil {
			fmt.Fprintf(&b, "%s|%d|%d;", p, st.Size(), st.ModTime().UnixNano())
		} else {
			fmt.Fprintf(&b, "%s|missing;", p)
		}
	}
	return b.String()
}

// loadNav returns the parsed data, re-parsing only when a file changed.
func loadNav(xplaneRoot string) (*navCache, error) {
	fix, nav, air := navFiles(xplaneRoot)
	if _, err := os.Stat(fix); err != nil {
		return nil, fmt.Errorf("no navigation data found in this X-Plane installation (%s)", defaultDataDir(xplaneRoot))
	}
	key := fileKey(fix, nav, air)
	navMu.Lock()
	defer navMu.Unlock()
	if navState != nil && navState.key == key {
		return navState, nil
	}
	c := &navCache{key: key}
	if f, err := os.Open(fix); err == nil {
		c.points = append(c.points, parseVfrPoints(f)...)
		f.Close()
	}
	if f, err := os.Open(nav); err == nil {
		c.points = append(c.points, parseNavaids(f)...)
		f.Close()
	}
	if f, err := os.Open(air); err == nil {
		c.airspaces = parseAirspaces(f)
		f.Close()
	}
	navState = c
	return c, nil
}

func newScanner(r io.Reader) *bufio.Scanner {
	s := bufio.NewScanner(r)
	s.Buffer(make([]byte, 64*1024), 1024*1024)
	return s
}

// parseVfrPoints: "lat lon ident region icao_region type [name...]", where
// type packs the ARINC 424 waypoint type (3 chars) little-endian into an
// int - first character 'V' is a VFR waypoint.
func parseVfrPoints(r io.Reader) []NavPoint {
	out := []NavPoint{}
	s := newScanner(r)
	for s.Scan() {
		f := strings.Fields(s.Text())
		if len(f) < 6 {
			continue
		}
		t, err := strconv.Atoi(f[5])
		if err != nil || t&0xff != 'V' {
			continue
		}
		lat, err1 := strconv.ParseFloat(f[0], 64)
		lon, err2 := strconv.ParseFloat(f[1], 64)
		if err1 != nil || err2 != nil {
			continue
		}
		p := NavPoint{Kind: "VRP", Ident: f[2], Lat: lat, Lon: lon, Name: f[2]}
		if f[3] != "ENRT" {
			p.Airport = f[3]
		}
		if len(f) > 6 {
			p.Name = strings.Join(f[6:], " ")
		}
		out = append(out, p)
	}
	return out
}

// parseNavaids: row code 3 (VOR) and 2 (NDB) of earth_nav.dat:
// "code lat lon elev freq range var ident region icao_region name...".
func parseNavaids(r io.Reader) []NavPoint {
	out := []NavPoint{}
	s := newScanner(r)
	for s.Scan() {
		f := strings.Fields(s.Text())
		if len(f) < 10 || (f[0] != "3" && f[0] != "2") {
			continue
		}
		lat, err1 := strconv.ParseFloat(f[1], 64)
		lon, err2 := strconv.ParseFloat(f[2], 64)
		freq, err3 := strconv.Atoi(f[4])
		if err1 != nil || err2 != nil || err3 != nil {
			continue
		}
		p := NavPoint{Ident: f[7], Lat: lat, Lon: lon, Name: strings.Join(f[10:], " ")}
		if f[0] == "3" {
			p.Kind = "VOR"
			p.Freq = fmt.Sprintf("%.2f", float64(freq)/100)
			if v, err := strconv.ParseFloat(f[6], 64); err == nil {
				p.MagVar = v
			}
		} else {
			p.Kind = "NDB"
			p.Freq = strconv.Itoa(freq)
		}
		out = append(out, p)
	}
	return out
}

// parseLimit reads an AL/AH value: "<n> MSL", "GND"/"SFC", "FL<n>" or
// "UNL(TD)". Returns feet MSL (FL taken as 100 ft each) and whether it's
// the ground.
func parseLimit(v string) (ft int, gnd bool) {
	v = strings.ToUpper(strings.TrimSpace(v))
	switch {
	case v == "GND" || v == "SFC" || v == "":
		return 0, true
	case strings.HasPrefix(v, "UNL"):
		return 99999, false
	case strings.HasPrefix(v, "FL"):
		n, _ := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(v, "FL")))
		return n * 100, false
	}
	n, _ := strconv.Atoi(strings.Fields(v)[0])
	return n, false
}

// parseDMS reads "50:13:00 N 008:46:22 E" into lat, lon.
func parseDMS(v string) (lat, lon float64, ok bool) {
	f := strings.Fields(v)
	if len(f) != 4 {
		return 0, 0, false
	}
	conv := func(dms, hemi string) (float64, bool) {
		parts := strings.Split(dms, ":")
		var d float64
		for i, p := range parts {
			n, err := strconv.ParseFloat(p, 64)
			if err != nil {
				return 0, false
			}
			d += n / math.Pow(60, float64(i))
		}
		if hemi == "S" || hemi == "W" {
			d = -d
		}
		return d, true
	}
	la, ok1 := conv(f[0], f[1])
	lo, ok2 := conv(f[2], f[3])
	return la, lo, ok1 && ok2
}

func parseAirspaces(r io.Reader) []Airspace {
	out := []Airspace{}
	var cur *Airspace
	flush := func() {
		if cur != nil && len(cur.Poly) >= 3 {
			cur.minLon, cur.minLat, cur.maxLon, cur.maxLat = 180, 90, -180, -90
			for _, p := range cur.Poly {
				cur.minLon = math.Min(cur.minLon, p[0])
				cur.maxLon = math.Max(cur.maxLon, p[0])
				cur.minLat = math.Min(cur.minLat, p[1])
				cur.maxLat = math.Max(cur.maxLat, p[1])
			}
			out = append(out, *cur)
		}
		cur = nil
	}
	s := newScanner(r)
	for s.Scan() {
		line := strings.TrimRight(s.Text(), "\r")
		if len(line) < 2 {
			continue
		}
		cmd, rest, _ := strings.Cut(line, " ")
		switch cmd {
		case "AC":
			flush()
			cur = &Airspace{Class: strings.TrimSpace(rest)}
		case "AN":
			if cur != nil {
				cur.Name = strings.TrimSpace(rest)
			}
		case "AL":
			if cur != nil {
				cur.LowerFt, cur.LowerGnd = parseLimit(rest)
			}
		case "AH":
			if cur != nil {
				cur.UpperFt, cur.UpperGnd = parseLimit(rest)
			}
		case "DP":
			if cur != nil {
				if lat, lon, ok := parseDMS(rest); ok {
					cur.Poly = append(cur.Poly, [2]float64{lon, lat})
				}
			}
		}
	}
	flush()
	return out
}

// airspacesInBox returns the airspaces overlapping the given box (at most
// `limit`, lower-level/more relevant classes first), coordinates rounded to
// ~10 m to keep the payload small.
func airspacesInBox(all []Airspace, minLon, minLat, maxLon, maxLat float64, limit int) []Airspace {
	out := []Airspace{}
	for _, a := range all {
		if a.maxLon < minLon || a.minLon > maxLon || a.maxLat < minLat || a.minLat > maxLat {
			continue
		}
		c := a
		c.Poly = make([][2]float64, len(a.Poly))
		for i, p := range a.Poly {
			c.Poly[i] = [2]float64{math.Round(p[0]*1e4) / 1e4, math.Round(p[1]*1e4) / 1e4}
		}
		out = append(out, c)
		if len(out) >= limit {
			break
		}
	}
	return out
}

func pointInPoly(lon, lat float64, poly [][2]float64) bool {
	inside := false
	for i, j := 0, len(poly)-1; i < len(poly); j, i = i, i+1 {
		xi, yi := poly[i][0], poly[i][1]
		xj, yj := poly[j][0], poly[j][1]
		if (yi > lat) != (yj > lat) && lon < (xj-xi)*(lat-yi)/(yj-yi)+xi {
			inside = !inside
		}
	}
	return inside
}

// AirspaceHit is one airspace in an AirspaceAlert.
type AirspaceHit struct {
	Name    string `json:"name"`
	Class   string `json:"class"`
	Lower   string `json:"lower"`
	Upper   string `json:"upper"`
	EtaSecs int    `json:"etaSecs,omitempty"`
}

// AirspaceAlert is the map page's "you're in / about to enter" info.
type AirspaceAlert struct {
	Inside []AirspaceHit `json:"inside"`
	Ahead  *AirspaceHit  `json:"ahead,omitempty"`
}

// Classes worth a warning for a VFR pilot; A/B/C/D/CTR need a clearance,
// P/R/Q are prohibited/restricted/danger areas.
var alertClasses = map[string]bool{"A": true, "B": true, "C": true, "D": true, "CTR": true, "P": true, "R": true, "Q": true}

func formatLimit(ft int, gnd bool) string {
	switch {
	case gnd:
		return "GND"
	case ft >= 99999:
		return "UNL"
	default:
		return fmt.Sprintf("%d ft", ft)
	}
}

func (a Airspace) containsAlt(altFt float64) bool {
	lower := float64(a.LowerFt)
	if a.LowerGnd {
		lower = -10000
	}
	return altFt >= lower && altFt <= float64(a.UpperFt)
}

// airspaceAlert checks where the aircraft is now and, projecting along its
// track at its ground speed, which relevant airspace it will enter within
// lookaheadS (checked every 10 s of flight).
func airspaceAlert(all []Airspace, lat, lon, altFt, headingDeg, gsKt float64, lookaheadS int) AirspaceAlert {
	alert := AirspaceAlert{Inside: []AirspaceHit{}}
	const nmPerDegLat = 60.0
	insideNames := map[string]bool{}
	// Only airspaces within ~30 NM matter even for a fast aircraft.
	box := 0.5
	candidates := []Airspace{}
	for _, a := range all {
		if !alertClasses[a.Class] || a.maxLon < lon-box/math.Max(0.2, math.Cos(lat*math.Pi/180)) ||
			a.minLon > lon+box/math.Max(0.2, math.Cos(lat*math.Pi/180)) || a.maxLat < lat-box || a.minLat > lat+box {
			continue
		}
		candidates = append(candidates, a)
	}
	for _, a := range candidates {
		if a.containsAlt(altFt) && pointInPoly(lon, lat, a.Poly) {
			insideNames[a.Name] = true
			alert.Inside = append(alert.Inside, AirspaceHit{Name: a.Name, Class: a.Class,
				Lower: formatLimit(a.LowerFt, a.LowerGnd), Upper: formatLimit(a.UpperFt, a.UpperGnd)})
		}
	}
	if gsKt < 30 {
		return alert // taxiing/parked - no look-ahead
	}
	hdg := headingDeg * math.Pi / 180
	for t := 10; t <= lookaheadS; t += 10 {
		distNm := gsKt * float64(t) / 3600
		pLat := lat + distNm*math.Cos(hdg)/nmPerDegLat
		pLon := lon + distNm*math.Sin(hdg)/(nmPerDegLat*math.Max(0.2, math.Cos(lat*math.Pi/180)))
		for _, a := range candidates {
			if insideNames[a.Name] || !a.containsAlt(altFt) || !pointInPoly(pLon, pLat, a.Poly) {
				continue
			}
			alert.Ahead = &AirspaceHit{Name: a.Name, Class: a.Class, Lower: formatLimit(a.LowerFt, a.LowerGnd),
				Upper: formatLimit(a.UpperFt, a.UpperGnd), EtaSecs: t}
			return alert
		}
	}
	return alert
}
