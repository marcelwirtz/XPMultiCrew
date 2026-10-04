package main

import (
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Weather briefing along a planned route: METARs and TAFs of the stations
// near it (aviationweather.gov, falling back to the METAR file X-Plane last
// downloaded), winds aloft per leg at the leg's altitude and the freezing
// level (open-meteo.com), runway wind at departure and destination, and
// warnings when the planned altitudes don't fit the weather. Real-world
// weather, as X-Plane's live weather uses it - for the sim only.

// WxStation is one reporting station near the route.
type WxStation struct {
	Ident     string  `json:"ident"`
	Name      string  `json:"name"`
	Lat       float64 `json:"lat"`
	Lon       float64 `json:"lon"`
	ElevFt    int     `json:"elevFt"`
	Metar     string  `json:"metar"`
	Taf       string  `json:"taf,omitempty"`
	WindDir   int     `json:"windDir"` // -1 variable
	WindKt    int     `json:"windKt"`
	GustKt    int     `json:"gustKt,omitempty"`
	VisM      int     `json:"visM"`                // 9999 = 10 km or more
	CeilingFt int     `json:"ceilingFt,omitempty"` // lowest BKN/OVC/VV above ground, 0 = none
	Category  string  `json:"category"`            // VFR, MVFR, IFR, LIFR
	AlongNm   float64 `json:"alongNm"`             // where along the route it is
	OffNm     float64 `json:"offNm"`               // how far off the route
	TafWarn   string  `json:"tafWarn,omitempty"`   // the worst TAF group in the next hours
}

// LegWind is the wind for the leg to waypoint i at that leg's altitude.
type LegWind struct {
	DirDeg  float64 `json:"dirDeg"`
	SpeedKt float64 `json:"speedKt"`
	AltFt   int     `json:"altFt"`
}

// RunwayWind is the best runway for the reported wind at an airport.
type RunwayWind struct {
	Airport  string `json:"airport"`
	Station  string `json:"station"`
	Runway   string `json:"runway"`
	HeadKt   int    `json:"headKt"` // negative = tailwind
	CrossKt  int    `json:"crossKt"`
	GustKt   int    `json:"gustKt,omitempty"`
	WindText string `json:"windText"`
}

// RouteBriefing is what the map's briefing panel shows.
type RouteBriefing struct {
	Source          string       `json:"source"`
	FetchedAt       string       `json:"fetchedAt"`
	Stations        []WxStation  `json:"stations"`
	LegWinds        []*LegWind   `json:"legWinds"` // index = waypoint index, [0] nil
	FreezingLevelFt int          `json:"freezingLevelFt,omitempty"`
	RunwayWinds     []RunwayWind `json:"runwayWinds"`
	Warnings        []string     `json:"warnings"`
	TotalNm         float64      `json:"totalNm"`
	Terrain         [][2]float64 `json:"terrain"` // [along NM, highest ground ft within 1 NM]
	TerrainSource   string       `json:"terrainSource,omitempty"`
}

const (
	wxCorridorNm   = 20 // stations up to this far off the route
	wxMaxStations  = 14
	wxCacheTTL     = 10 * time.Minute
	wxHTTPTimeout  = 30 * time.Second // Wikidata can take ~15 s on a cold query
	wxUserAgent    = "XPMultiCrew-companion (flight sim companion; +https://github.com/marcelwirtz/XPMultiCrew)"
	aviationWxBase = "https://aviationweather.gov/api/data"
	openMeteoBase  = "https://api.open-meteo.com/v1/forecast"
)

// fetchURL is a var so tests can stub the network.
var fetchURL = func(u string) ([]byte, error) {
	client := &http.Client{Timeout: wxHTTPTimeout}
	req, err := http.NewRequest(http.MethodGet, u, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", wxUserAgent)
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusNoContent {
		return nil, fmt.Errorf("%s returned %s", req.URL.Host, resp.Status)
	}
	return io.ReadAll(io.LimitReader(resp.Body, 8<<20))
}

var nowUTC = func() time.Time { return time.Now().UTC() }

type wxCacheEntry struct {
	at   time.Time
	body []byte
}

var (
	wxCacheMu sync.Mutex
	wxCache   = map[string]wxCacheEntry{}
)

// fetchCached keeps answers for wxCacheTTL so reopening the briefing
// doesn't hammer the weather services.
func fetchCached(u string) ([]byte, error) {
	return fetchCachedFor(u, wxCacheTTL)
}

func fetchCachedFor(u string, ttl time.Duration) ([]byte, error) {
	wxCacheMu.Lock()
	e, ok := wxCache[u]
	wxCacheMu.Unlock()
	if ok && nowUTC().Sub(e.at) < ttl {
		return e.body, nil
	}
	body, err := fetchURL(u)
	if err != nil {
		return nil, err
	}
	wxCacheMu.Lock()
	wxCache[u] = wxCacheEntry{at: nowUTC(), body: body}
	wxCacheMu.Unlock()
	return body, nil
}

// --- METAR / TAF parsing ---

var (
	reWind  = regexp.MustCompile(`^(\d{3}|VRB)(\d{2,3})(?:G(\d{2,3}))?(KT|MPS)$`)
	reVis   = regexp.MustCompile(`^(\d{4})(NDV)?$`)
	reVisSM = regexp.MustCompile(`^(?:(\d+) )?(\d+)(?:/(\d+))?SM$`)
	reCloud = regexp.MustCompile(`^(FEW|SCT|BKN|OVC|VV)(\d{3}|///)`)
	reTime  = regexp.MustCompile(`^\d{6}Z$`)
	reDigit = regexp.MustCompile(`^\d$`)
)

type wxObs struct {
	windDir, windKt, gustKt int
	visM                    int
	ceilingFt               int
	hasWind, hasVis         bool
}

// parseWxGroups reads wind, visibility and the ceiling from METAR/TAF
// tokens (anything else is ignored).
func parseWxGroups(tokens []string) wxObs {
	o := wxObs{windDir: -1, visM: 9999}
	for i := 0; i < len(tokens); i++ {
		t := tokens[i]
		switch {
		case t == "CAVOK":
			o.visM, o.hasVis = 9999, true
		case reWind.MatchString(t):
			m := reWind.FindStringSubmatch(t)
			spd, _ := strconv.Atoi(m[2])
			gst, _ := strconv.Atoi(m[3])
			if m[4] == "MPS" {
				spd, gst = int(math.Round(float64(spd)*1.944)), int(math.Round(float64(gst)*1.944))
			}
			o.windKt, o.gustKt, o.hasWind = spd, gst, true
			if m[1] != "VRB" {
				o.windDir, _ = strconv.Atoi(m[1])
			}
		case reVis.MatchString(t) && !o.hasVis:
			o.visM, _ = strconv.Atoi(reVis.FindStringSubmatch(t)[1])
			o.hasVis = true
		case strings.HasSuffix(t, "SM") && !o.hasVis:
			s := t
			if i > 0 && reDigit.MatchString(tokens[i-1]) {
				s = tokens[i-1] + " " + t
			}
			if m := reVisSM.FindStringSubmatch(strings.TrimPrefix(s, "P")); m != nil {
				whole, _ := strconv.ParseFloat(m[1], 64)
				num, _ := strconv.ParseFloat(m[2], 64)
				den := 1.0
				if m[3] != "" {
					den, _ = strconv.ParseFloat(m[3], 64)
				}
				o.visM, o.hasVis = int(math.Min(9999, (whole+num/den)*1609)), true
			}
		case reCloud.MatchString(t):
			m := reCloud.FindStringSubmatch(t)
			if m[1] == "BKN" || m[1] == "OVC" || m[1] == "VV" {
				if h, err := strconv.Atoi(m[2]); err == nil && (o.ceilingFt == 0 || h*100 < o.ceilingFt) {
					o.ceilingFt = h * 100
					if o.ceilingFt == 0 {
						o.ceilingFt = 1 // on the ground: keep it distinguishable from "none"
					}
				}
			}
		}
	}
	return o
}

// flightCategory: LIFR < 500 ft / 1.6 km, IFR < 1000 ft / 5 km, MVFR up
// to 3000 ft / 8 km, else VFR.
func flightCategory(ceilingFt, visM int) string {
	c := ceilingFt
	if c == 0 {
		c = 99999
	}
	switch {
	case c < 500 || visM < 1600:
		return "LIFR"
	case c < 1000 || visM < 5000:
		return "IFR"
	case c <= 3000 || visM <= 8000:
		return "MVFR"
	}
	return "VFR"
}

var categoryRank = map[string]int{"VFR": 0, "MVFR": 1, "IFR": 2, "LIFR": 3}

// metarTokens strips the "METAR"/"SPECI"/"AUTO" prefix and everything from
// the trend/remarks on.
func metarTokens(raw string) []string {
	out := []string{}
	for _, t := range strings.Fields(raw) {
		if t == "RMK" || t == "TEMPO" || t == "BECMG" || t == "NOSIG" {
			break
		}
		out = append(out, t)
	}
	return out
}

func stationFromMetar(raw string) WxStation {
	o := parseWxGroups(metarTokens(raw))
	s := WxStation{Metar: strings.TrimSpace(raw), WindDir: o.windDir, WindKt: o.windKt, GustKt: o.gustKt, VisM: o.visM, CeilingFt: o.ceilingFt}
	s.Category = flightCategory(o.ceilingFt, o.visM)
	return s
}

// tafPeriod parses "ddhh/ddhh" relative to now (the TAF's month).
func tafPeriod(tok string, now time.Time) (time.Time, time.Time, bool) {
	var d1, h1, d2, h2 int
	if n, _ := fmt.Sscanf(tok, "%2d%2d/%2d%2d", &d1, &h1, &d2, &h2); n != 4 || len(tok) != 9 {
		return time.Time{}, time.Time{}, false
	}
	at := func(d, h int) time.Time {
		t := time.Date(now.Year(), now.Month(), d, 0, 0, 0, 0, time.UTC).Add(time.Duration(h) * time.Hour)
		if t.Sub(now) > 15*24*time.Hour {
			t = t.AddDate(0, -1, 0)
		} else if now.Sub(t) > 15*24*time.Hour {
			t = t.AddDate(0, 1, 0)
		}
		return t
	}
	return at(d1, h1), at(d2, h2), true
}

// tafWarning returns the worst TAF group (base forecast or change group)
// that is worse than VFR and valid within [from, to], or "".
func tafWarning(raw string, from, to time.Time) string {
	tokens := strings.Fields(raw)
	type group struct {
		toks       []string
		start, end time.Time
		ok         bool
	}
	groups := []*group{}
	cur := &group{}
	groups = append(groups, cur)
	for i := 0; i < len(tokens); i++ {
		t := tokens[i]
		switch {
		case t == "TEMPO" || t == "BECMG" || strings.HasPrefix(t, "PROB"):
			if !strings.HasPrefix(t, "PROB") && len(cur.toks) == 1 && strings.HasPrefix(cur.toks[0], "PROB") {
				cur.toks = append(cur.toks, t) // "PROB40 TEMPO"
				continue
			}
			cur = &group{toks: []string{t}}
			groups = append(groups, cur)
		case strings.HasPrefix(t, "FM") && len(t) == 8:
			cur = &group{toks: []string{t}}
			groups = append(groups, cur)
			var d, h, m int
			if n, _ := fmt.Sscanf(t[2:], "%2d%2d%2d", &d, &h, &m); n == 3 {
				s, _, _ := tafPeriod(fmt.Sprintf("%02d%02d/%02d%02d", d, h, d, h), from)
				cur.start, cur.end, cur.ok = s.Add(time.Duration(m)*time.Minute), s.Add(30*time.Hour), true
			}
		default:
			cur.toks = append(cur.toks, t)
			if s, e, ok := tafPeriod(t, from); ok && !cur.ok {
				cur.start, cur.end, cur.ok = s, e, true
			}
		}
	}
	worst, worstRank := "", 0
	for _, g := range groups {
		if !g.ok || g.end.Before(from) || g.start.After(to) {
			continue
		}
		o := parseWxGroups(g.toks)
		if !o.hasVis && o.ceilingFt == 0 {
			continue
		}
		cat := flightCategory(o.ceilingFt, o.visM)
		if r := categoryRank[cat]; r > worstRank {
			text := strings.Join(g.toks, " ")
			if g == groups[0] {
				text = "TAF " + text
			}
			worst, worstRank = fmt.Sprintf("%s (%s)", strings.TrimSpace(text), cat), r
		}
	}
	return worst
}

// --- Data sources ---

type awcMetar struct {
	IcaoID string   `json:"icaoId"`
	RawOb  string   `json:"rawOb"`
	Lat    float64  `json:"lat"`
	Lon    float64  `json:"lon"`
	Elev   *float64 `json:"elev"` // metres
	Name   string   `json:"name"`
}

type awcTaf struct {
	IcaoID string `json:"icaoId"`
	RawTAF string `json:"rawTAF"`
}

func bboxParam(minLat, minLon, maxLat, maxLon float64) string {
	return fmt.Sprintf("%.2f,%.2f,%.2f,%.2f", minLat, minLon, maxLat, maxLon)
}

// onlineStations fetches METARs and TAFs in the box from aviationweather.gov.
func onlineStations(minLat, minLon, maxLat, maxLon float64) ([]WxStation, error) {
	bbox := url.QueryEscape(bboxParam(minLat, minLon, maxLat, maxLon))
	var tafBody []byte
	var tafErr error
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		tafBody, tafErr = fetchCached(aviationWxBase + "/taf?format=json&bbox=" + bbox)
	}()
	body, err := fetchCached(aviationWxBase + "/metar?format=json&bbox=" + bbox)
	wg.Wait()
	if err != nil {
		return nil, err
	}
	var metars []awcMetar
	if len(strings.TrimSpace(string(body))) > 0 {
		if err := json.Unmarshal(body, &metars); err != nil {
			return nil, fmt.Errorf("unexpected METAR answer: %v", err)
		}
	}
	tafs := map[string]string{}
	if tafErr == nil {
		var list []awcTaf
		if json.Unmarshal(tafBody, &list) == nil {
			for _, t := range list {
				tafs[t.IcaoID] = t.RawTAF
			}
		}
	}
	out := []WxStation{}
	seen := map[string]bool{}
	for _, m := range metars {
		if seen[m.IcaoID] || m.RawOb == "" {
			continue
		}
		seen[m.IcaoID] = true
		s := stationFromMetar(m.RawOb)
		s.Ident, s.Name, s.Lat, s.Lon, s.Taf = m.IcaoID, m.Name, m.Lat, m.Lon, tafs[m.IcaoID]
		if m.Elev != nil {
			s.ElevFt = int(math.Round(*m.Elev * 3.28084))
		}
		out = append(out, s)
	}
	return out, nil
}

// latestXPlaneMetars reads the newest Output/real weather/metar-*.txt.
func latestXPlaneMetars(xplaneRoot string) (map[string]string, string, error) {
	files, _ := filepath.Glob(filepath.Join(xplaneRoot, "Output", "real weather", "metar-*.txt"))
	if len(files) == 0 {
		return nil, "", fmt.Errorf("no METAR file from X-Plane found")
	}
	sort.Strings(files) // metar-YYYY-MM-DD-HH.MM.txt sorts by time
	path := files[len(files)-1]
	f, err := os.Open(path)
	if err != nil {
		return nil, "", err
	}
	defer f.Close()
	out := map[string]string{}
	s := newScanner(f)
	for s.Scan() {
		line := strings.TrimSpace(s.Text())
		f := strings.Fields(line)
		if len(f) < 3 || !reTime.MatchString(f[1]) {
			continue
		}
		out[f[0]] = line
	}
	stamp := strings.TrimSuffix(strings.TrimPrefix(filepath.Base(path), "metar-"), ".txt")
	return out, stamp, s.Err()
}

func offlineStations(xplaneRoot string, airports AirportData, minLat, minLon, maxLat, maxLon float64) ([]WxStation, string, error) {
	metars, stamp, err := latestXPlaneMetars(xplaneRoot)
	if err != nil {
		return nil, "", err
	}
	out := []WxStation{}
	for _, a := range airports.Airports {
		if len(a) < 5 {
			continue
		}
		id, _ := a[0].(string)
		raw, ok := metars[id]
		if !ok {
			continue
		}
		lat, lon := toFloat(a[2]), toFloat(a[3])
		if lat < minLat || lat > maxLat || lon < minLon || lon > maxLon {
			continue
		}
		s := stationFromMetar(raw)
		s.Ident, s.Lat, s.Lon, s.ElevFt = id, lat, lon, airports.Details[id].ElevationFt
		s.Name, _ = a[1].(string)
		out = append(out, s)
	}
	return out, stamp, nil
}

// openMeteoWinds asks for winds on pressure levels (with their heights) and
// the freezing level at the given points, for the current hour.
type windProfile struct {
	heightsFt []float64 // ascending
	dirs      []float64
	speeds    []float64
	freezeFt  float64
}

var windLevels = []int{1000, 925, 850, 700, 600}

func openMeteoWinds(points [][2]float64) ([]windProfile, error) {
	if len(points) == 0 {
		return nil, nil
	}
	lats, lons := []string{}, []string{}
	for _, p := range points {
		lats = append(lats, fmt.Sprintf("%.3f", p[0]))
		lons = append(lons, fmt.Sprintf("%.3f", p[1]))
	}
	vars := []string{"freezing_level_height"}
	for _, l := range windLevels {
		vars = append(vars, fmt.Sprintf("wind_speed_%dhPa", l), fmt.Sprintf("wind_direction_%dhPa", l), fmt.Sprintf("geopotential_height_%dhPa", l))
	}
	u := fmt.Sprintf("%s?latitude=%s&longitude=%s&hourly=%s&wind_speed_unit=kn&forecast_hours=1&timezone=GMT",
		openMeteoBase, strings.Join(lats, ","), strings.Join(lons, ","), strings.Join(vars, ","))
	body, err := fetchCached(u)
	if err != nil {
		return nil, err
	}
	type omPoint struct {
		Hourly map[string]json.RawMessage `json:"hourly"`
	}
	var list []omPoint
	if len(points) == 1 {
		var one omPoint
		if err := json.Unmarshal(body, &one); err != nil {
			return nil, err
		}
		list = []omPoint{one}
	} else if err := json.Unmarshal(body, &list); err != nil {
		return nil, err
	}
	first := func(h map[string]json.RawMessage, key string) (float64, bool) {
		var v []*float64
		if json.Unmarshal(h[key], &v) != nil || len(v) == 0 || v[0] == nil {
			return 0, false
		}
		return *v[0], true
	}
	out := make([]windProfile, len(list))
	for i, p := range list {
		type lvl struct{ h, d, s float64 }
		lv := []lvl{}
		for _, l := range windLevels {
			h, ok1 := first(p.Hourly, fmt.Sprintf("geopotential_height_%dhPa", l))
			d, ok2 := first(p.Hourly, fmt.Sprintf("wind_direction_%dhPa", l))
			s, ok3 := first(p.Hourly, fmt.Sprintf("wind_speed_%dhPa", l))
			if ok1 && ok2 && ok3 {
				lv = append(lv, lvl{h * 3.28084, d, s})
			}
		}
		sort.Slice(lv, func(a, b int) bool { return lv[a].h < lv[b].h })
		for _, l := range lv {
			out[i].heightsFt = append(out[i].heightsFt, l.h)
			out[i].dirs = append(out[i].dirs, l.d)
			out[i].speeds = append(out[i].speeds, l.s)
		}
		if f, ok := first(p.Hourly, "freezing_level_height"); ok {
			out[i].freezeFt = f * 3.28084
		}
	}
	return out, nil
}

// at interpolates the wind vector at altFt.
func (w windProfile) at(altFt float64) (dir, speed float64, ok bool) {
	n := len(w.heightsFt)
	if n == 0 {
		return 0, 0, false
	}
	vec := func(i int) (float64, float64) {
		r := w.dirs[i] * math.Pi / 180
		return w.speeds[i] * math.Sin(r), w.speeds[i] * math.Cos(r)
	}
	var u, v float64
	switch {
	case altFt <= w.heightsFt[0]:
		u, v = vec(0)
	case altFt >= w.heightsFt[n-1]:
		u, v = vec(n - 1)
	default:
		for i := 1; i < n; i++ {
			if altFt <= w.heightsFt[i] {
				t := (altFt - w.heightsFt[i-1]) / (w.heightsFt[i] - w.heightsFt[i-1])
				u0, v0 := vec(i - 1)
				u1, v1 := vec(i)
				u, v = u0+(u1-u0)*t, v0+(v1-v0)*t
				break
			}
		}
	}
	return math.Mod(math.Atan2(u, v)*180/math.Pi+360, 360), math.Hypot(u, v), true
}

// --- The briefing ---

// alongRoute projects a point onto the route: distance along it and off it.
func alongRoute(wps []RouteWaypoint, lat, lon float64) (along, off float64) {
	proj := newProjection(lat)
	px, py := proj.xy(lat, lon)
	best, bestAlong, acc := math.Inf(1), 0.0, 0.0
	for i := 1; i < len(wps); i++ {
		ax, ay := proj.xy(wps[i-1].Lat, wps[i-1].Lon)
		bx, by := proj.xy(wps[i].Lat, wps[i].Lon)
		dx, dy := bx-ax, by-ay
		l2 := dx*dx + dy*dy
		t := 0.0
		if l2 > 0 {
			t = math.Max(0, math.Min(1, ((px-ax)*dx+(py-ay)*dy)/l2))
		}
		d := math.Hypot(px-(ax+t*dx), py-(ay+t*dy))
		if d < best {
			best, bestAlong = d, acc+t*math.Sqrt(l2)
		}
		acc += math.Sqrt(l2)
	}
	return bestAlong, best
}

func legAltitude(r PlannedRoute, i int) int {
	if r.Waypoints[i].AltFt > 0 {
		return r.Waypoints[i].AltFt
	}
	if r.CruiseFt > 0 {
		return r.CruiseFt
	}
	return 3500
}

// runwayWind picks the runway with the most headwind for a METAR wind.
func runwayWind(airport string, runways []RunwayGeometry, st WxStation) (RunwayWind, bool) {
	if st.WindDir < 0 && st.WindKt > 3 {
		return RunwayWind{}, false // variable and not calm: no useful runway pick
	}
	best := RunwayWind{HeadKt: -1000}
	for _, r := range runways {
		if r.Airport != airport {
			continue
		}
		for e := 0; e < 2; e++ {
			from, to := r.Ends[e], r.Ends[1-e]
			hdg := trueBearing(from.Lat, from.Lon, to.Lat, to.Lon)
			diff := (float64(st.WindDir) - hdg) * math.Pi / 180
			head := float64(st.WindKt) * math.Cos(diff)
			cross := math.Abs(float64(st.WindKt) * math.Sin(diff))
			if st.WindDir < 0 {
				head, cross = 0, 0
			}
			if int(math.Round(head)) > best.HeadKt {
				best = RunwayWind{Airport: airport, Station: st.Ident, Runway: from.Name, HeadKt: int(math.Round(head)),
					CrossKt: int(math.Round(cross)), GustKt: st.GustKt}
			}
		}
	}
	if best.Runway == "" {
		return RunwayWind{}, false
	}
	switch {
	case st.WindDir < 0 && st.WindKt <= 3:
		best.WindText = fmt.Sprintf("%s VRB%02dKT", st.Ident, st.WindKt)
	default:
		best.WindText = fmt.Sprintf("%s %03d°/%d kt", st.Ident, st.WindDir, st.WindKt)
		if st.GustKt > 0 {
			best.WindText += fmt.Sprintf(" G%d", st.GustKt)
		}
	}
	return best, true
}

func trueBearing(lat1, lon1, lat2, lon2 float64) float64 {
	const rad = math.Pi / 180
	y := math.Sin((lon2-lon1)*rad) * math.Cos(lat2*rad)
	x := math.Cos(lat1*rad)*math.Sin(lat2*rad) - math.Sin(lat1*rad)*math.Cos(lat2*rad)*math.Cos((lon2-lon1)*rad)
	return math.Mod(math.Atan2(y, x)/rad+360, 360)
}

// routeBriefing builds the briefing for a route.
func routeBriefing(xplaneRoot string, airports AirportData, r PlannedRoute) (RouteBriefing, error) {
	if len(r.Waypoints) < 2 {
		return RouteBriefing{}, fmt.Errorf("plan a route first (at least two waypoints)")
	}
	b := RouteBriefing{Stations: []WxStation{}, LegWinds: make([]*LegWind, len(r.Waypoints)), RunwayWinds: []RunwayWind{}, Warnings: []string{}, Terrain: [][2]float64{}}
	minLat, minLon, maxLat, maxLon := 90.0, 180.0, -90.0, -180.0
	for i, w := range r.Waypoints {
		minLat, maxLat = math.Min(minLat, w.Lat), math.Max(maxLat, w.Lat)
		minLon, maxLon = math.Min(minLon, w.Lon), math.Max(maxLon, w.Lon)
		if i > 0 {
			b.TotalNm += greatCircleNm(r.Waypoints[i-1].Lat, r.Waypoints[i-1].Lon, w.Lat, w.Lon)
		}
	}
	padLat := wxCorridorNm / 60.0
	padLon := padLat / math.Max(0.2, math.Cos((minLat+maxLat)/2*math.Pi/180))
	minLat, maxLat, minLon, maxLon = minLat-padLat, maxLat+padLat, minLon-padLon, maxLon+padLon

	// Winds aloft at each leg's midpoint, fetched while the METARs load.
	mids := [][2]float64{}
	for i := 1; i < len(r.Waypoints); i++ {
		a, c := r.Waypoints[i-1], r.Waypoints[i]
		mids = append(mids, [2]float64{(a.Lat + c.Lat) / 2, (a.Lon + c.Lon) / 2})
	}
	var profiles []windProfile
	var windErr error
	windDone := make(chan struct{})
	go func() {
		defer close(windDone)
		profiles, windErr = openMeteoWinds(mids)
	}()
	var terrain RouteTerrain
	var terrainErr error
	terrainDone := make(chan struct{})
	go func() {
		defer close(terrainDone)
		terrain, terrainErr = routeTerrain(xplaneRoot, r.Waypoints, 2)
	}()

	now := nowUTC()
	b.FetchedAt = now.Format("2006-01-02 15:04Z")
	stations, err := onlineStations(minLat, minLon, maxLat, maxLon)
	b.Source = "METAR/TAF: aviationweather.gov · winds: Open-Meteo.com"
	if err != nil {
		var stamp string
		var offErr error
		stations, stamp, offErr = offlineStations(xplaneRoot, airports, minLat, minLon, maxLat, maxLon)
		if offErr != nil {
			return RouteBriefing{}, fmt.Errorf("no weather: %v (and %v)", err, offErr)
		}
		b.Source = "X-Plane's last downloaded METARs (" + stamp + "Z, no TAFs)"
		b.Warnings = append(b.Warnings, "Offline: using the METARs X-Plane downloaded at "+stamp+"Z - may be old ("+err.Error()+")")
	}

	// Stations near the route, ordered along it; airports on the route first.
	onRoute := map[string]bool{}
	for _, w := range r.Waypoints {
		if w.Kind == "APT" {
			onRoute[w.Ident] = true
		}
	}
	near := []WxStation{}
	for _, s := range stations {
		s.AlongNm, s.OffNm = alongRoute(r.Waypoints, s.Lat, s.Lon)
		if s.OffNm <= wxCorridorNm {
			near = append(near, s)
		}
	}
	sort.SliceStable(near, func(i, j int) bool {
		if onRoute[near[i].Ident] != onRoute[near[j].Ident] {
			return onRoute[near[i].Ident]
		}
		return near[i].OffNm < near[j].OffNm
	})
	if len(near) > wxMaxStations {
		near = near[:wxMaxStations]
	}
	sort.SliceStable(near, func(i, j int) bool { return near[i].AlongNm < near[j].AlongNm })
	for i := range near {
		if near[i].Taf != "" {
			near[i].TafWarn = tafWarning(near[i].Taf, now, now.Add(4*time.Hour))
		}
	}
	b.Stations = near
	if len(near) == 0 {
		b.Warnings = append(b.Warnings, "No weather station within 20 NM of the route")
	}

	// Winds aloft at each leg's altitude.
	<-windDone
	maxLegAlt := 0
	if windErr == nil && len(profiles) == len(mids) {
		freeze := math.Inf(1)
		for i, p := range profiles {
			alt := legAltitude(r, i+1)
			maxLegAlt = max(maxLegAlt, alt)
			if d, s, ok := p.at(float64(alt)); ok {
				b.LegWinds[i+1] = &LegWind{DirDeg: math.Round(d), SpeedKt: math.Round(s), AltFt: alt}
			}
			if p.freezeFt > 0 {
				freeze = math.Min(freeze, p.freezeFt)
			}
		}
		if !math.IsInf(freeze, 1) {
			b.FreezingLevelFt = int(math.Round(freeze/100) * 100)
		}
	} else {
		for i := 1; i < len(r.Waypoints); i++ {
			maxLegAlt = max(maxLegAlt, legAltitude(r, i))
		}
		msg := "no data"
		if windErr != nil {
			msg = windErr.Error()
		}
		b.Warnings = append(b.Warnings, "No winds aloft ("+msg+") - legs use X-Plane's current wind")
	}

	// Runway wind at departure and destination (their own METAR or the
	// nearest one within 15 NM).
	for _, end := range []RouteWaypoint{r.Waypoints[0], r.Waypoints[len(r.Waypoints)-1]} {
		if end.Kind != "APT" {
			continue
		}
		var st *WxStation
		bestNm := 15.0
		for i := range stations {
			d := greatCircleNm(end.Lat, end.Lon, stations[i].Lat, stations[i].Lon)
			if stations[i].Ident == end.Ident {
				d = -1
			}
			if d < bestNm {
				st, bestNm = &stations[i], d
			}
		}
		if st == nil {
			continue
		}
		if rw, ok := runwayWind(end.Ident, airports.RunwayGeometry, *st); ok {
			b.RunwayWinds = append(b.RunwayWinds, rw)
			if rw.CrossKt >= 15 {
				b.Warnings = append(b.Warnings, fmt.Sprintf("%s: %d kt crosswind on runway %s", end.Ident, rw.CrossKt, rw.Runway))
			}
			if rw.HeadKt < 0 {
				b.Warnings = append(b.Warnings, fmt.Sprintf("%s: tailwind on every runway (%s)", end.Ident, rw.WindText))
			}
		}
		if st.GustKt >= 25 || st.WindKt >= 25 {
			b.Warnings = append(b.Warnings, fmt.Sprintf("%s: strong wind %s", end.Ident, st.Metar))
		}
	}

	// Ceilings and visibility against the planned altitudes.
	for _, s := range near {
		legIdx := legAt(r.Waypoints, s.AlongNm)
		alt := legAltitude(r, legIdx)
		if s.CeilingFt > 0 {
			baseMSL := s.ElevFt + s.CeilingFt
			if baseMSL < alt+500 {
				b.Warnings = append(b.Warnings, fmt.Sprintf("%s: cloud base ~%d ft MSL (%d ft above ground) - below or close to the planned %d ft", s.Ident, baseMSL, s.CeilingFt, alt))
			}
		}
		if categoryRank[s.Category] >= categoryRank["IFR"] {
			b.Warnings = append(b.Warnings, fmt.Sprintf("%s: %s conditions now", s.Ident, s.Category))
		} else if s.VisM < 5000 {
			b.Warnings = append(b.Warnings, fmt.Sprintf("%s: visibility %d m", s.Ident, s.VisM))
		}
		if s.TafWarn != "" && categoryRank[s.Category] < categoryRank["IFR"] {
			b.Warnings = append(b.Warnings, fmt.Sprintf("%s forecast within 4 h: %s", s.Ident, s.TafWarn))
		}
	}
	// Ground clearance of the planned altitudes.
	<-terrainDone
	if terrainErr != nil {
		b.Warnings = append(b.Warnings, "Terrain not checked ("+terrainErr.Error()+")")
	} else {
		b.Terrain, b.TerrainSource = terrain.Profile, terrain.Source
		for i := 1; i < len(r.Waypoints); i++ {
			ground := terrain.LegMaxFt[i]
			if ground <= 0 {
				continue
			}
			touches := i == 1 || i == len(r.Waypoints)-1
			legNm := greatCircleNm(r.Waypoints[i-1].Lat, r.Waypoints[i-1].Lon, r.Waypoints[i].Lat, r.Waypoints[i].Lon)
			if alt := float64(legAltitude(r, i)); alt < ground+terrainClearance(touches, legNm) {
				name := r.Waypoints[i].Ident
				if name == "" {
					name = fmt.Sprintf("WPT%d", i+1)
				}
				b.Warnings = append(b.Warnings, fmt.Sprintf("Leg to %s: ground up to %.0f ft within 1 NM - %.0f ft is too low", name, ground, alt))
			}
		}
	}
	if b.FreezingLevelFt > 0 && b.FreezingLevelFt < maxLegAlt+1000 {
		b.Warnings = append(b.Warnings, fmt.Sprintf("Freezing level ~%d ft - icing risk in clouds or rain at the planned altitude", b.FreezingLevelFt))
	}
	return b, nil
}

// legAt returns the index of the waypoint ending the leg at alongNm.
func legAt(wps []RouteWaypoint, alongNm float64) int {
	acc := 0.0
	for i := 1; i < len(wps); i++ {
		acc += greatCircleNm(wps[i-1].Lat, wps[i-1].Lon, wps[i].Lat, wps[i].Lon)
		if alongNm <= acc {
			return i
		}
	}
	return len(wps) - 1
}
