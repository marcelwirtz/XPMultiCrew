package main

import (
	"fmt"
	"hash/fnv"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

// "Where to tonight?": destination ideas for an evening's flight from an
// airport - reachable in the time there is at the slower aircraft's speed,
// with a long enough runway, VFR weather now and back before sunset when
// it's a round trip. Airports with installed custom scenery, ones you've
// never flown to (recorded flights) and mountain airfields score higher; a
// small per-day shuffle (or "more ideas") keeps it from always suggesting
// the same three.

// DestinationRequest is what the map's "Where to?" panel sends.
type DestinationRequest struct {
	From       string  `json:"from"`
	Hours      float64 `json:"hours"` // time available for the whole evening
	TasKt      int     `json:"tasKt"` // the slower aircraft
	RoundTrip  bool    `json:"roundTrip"`
	MinRunwayM int     `json:"minRunwayM"`
	Seed       int     `json:"seed"` // "more ideas" shuffles differently
}

// DestinationIdea is one suggestion.
type DestinationIdea struct {
	Ident      string   `json:"ident"`
	Name       string   `json:"name"`
	Lat        float64  `json:"lat"`
	Lon        float64  `json:"lon"`
	ElevFt     int      `json:"elevFt"`
	DistanceNm float64  `json:"distanceNm"`
	BearingDeg float64  `json:"bearingDeg"`
	FlightMin  float64  `json:"flightMin"` // one way, with a little for routing
	RunwayM    int      `json:"runwayM"`
	Tags       []string `json:"tags"`
	Category   string   `json:"category,omitempty"` // weather nearby, if any
	Metar      string   `json:"metar,omitempty"`
	Score      float64  `json:"-"`
}

// DestinationIdeas is the panel's answer.
type DestinationIdeas struct {
	Ideas  []DestinationIdea `json:"ideas"`
	Notes  []string          `json:"notes"`
	Sunset string            `json:"sunset,omitempty"` // at the departure, "18:52Z"
}

const (
	destRoutingFactor = 1.15 // a VFR route is a bit longer than the straight line
	destGroundMin     = 15.0 // taxi, run-up, pattern per leg
	destMinBearingGap = 50.0 // spread the ideas over different directions
	destCount         = 3
)

// sunsetUTC: NOAA's sunrise equation (good to a few minutes). ok is false
// when the sun doesn't set (or rise) that day.
func sunsetUTC(lat, lon float64, day time.Time) (time.Time, bool) {
	const rad = math.Pi / 180
	jd := float64(day.Truncate(24*time.Hour).Unix())/86400 + 2440587.5 // midnight UTC
	n := math.Ceil(jd - 2451545.0 + 0.0008)
	jStar := n - lon/360
	m := math.Mod(357.5291+0.98560028*jStar, 360)
	c := 1.9148*math.Sin(m*rad) + 0.02*math.Sin(2*m*rad) + 0.0003*math.Sin(3*m*rad)
	lambda := math.Mod(m+c+180+102.9372, 360)
	transit := 2451545.0 + jStar + 0.0053*math.Sin(m*rad) - 0.0069*math.Sin(2*lambda*rad)
	sinDecl := math.Sin(lambda*rad) * math.Sin(23.4397*rad)
	cosDecl := math.Cos(math.Asin(sinDecl))
	cosW := (math.Sin(-0.833*rad) - math.Sin(lat*rad)*sinDecl) / (math.Cos(lat*rad) * cosDecl)
	if cosW < -1 || cosW > 1 {
		return time.Time{}, false
	}
	set := transit + math.Acos(cosW)/rad/360
	return time.Unix(int64((set-2440587.5)*86400), 0).UTC(), true
}

var (
	customSceneryMu   sync.Mutex
	customSceneryKey  string
	customSceneryApts map[string]bool
)

// customSceneryAirports lists the airports in Custom Scenery packs' own
// apt.dat files (cached until a pack is added or removed).
func customSceneryAirports(xplaneRoot string) map[string]bool {
	dir := filepath.Join(xplaneRoot, "Custom Scenery")
	files, _ := filepath.Glob(filepath.Join(dir, "*", "Earth nav data", "apt.dat"))
	key := strings.Join(files, "|")
	customSceneryMu.Lock()
	defer customSceneryMu.Unlock()
	if customSceneryApts != nil && key == customSceneryKey {
		return customSceneryApts
	}
	out := map[string]bool{}
	for _, path := range files {
		f, err := os.Open(path)
		if err != nil {
			continue
		}
		s := newScanner(f)
		for s.Scan() {
			fl := strings.Fields(s.Text())
			if len(fl) >= 5 && (fl[0] == "1" || fl[0] == "16" || fl[0] == "17") {
				out[fl[4]] = true
			}
		}
		f.Close()
	}
	customSceneryKey, customSceneryApts = key, out
	return out
}

// longestRunways returns each airport's longest runway in metres.
func longestRunways(runways []RunwayGeometry) map[string]int {
	out := map[string]int{}
	for _, r := range runways {
		l := int(greatCircleNm(r.Ends[0].Lat, r.Ends[0].Lon, r.Ends[1].Lat, r.Ends[1].Lon) * 1852)
		if l > out[r.Airport] {
			out[r.Airport] = l
		}
	}
	return out
}

// jitter is a stable pseudo-random 0..1 per airport, day and seed.
func jitter(ident string, day time.Time, seed int) float64 {
	h := fnv.New32a()
	fmt.Fprintf(h, "%s|%s|%d", ident, day.Format("2006-01-02"), seed)
	return float64(h.Sum32()%10000) / 10000
}

// suggestDestinations: see the comment at the top of the file. visited
// holds airports from recorded flights, scenery the custom-scenery ones.
func suggestDestinations(airports AirportData, stations []WxStation, visited, scenery map[string]bool, req DestinationRequest, now time.Time) (DestinationIdeas, error) {
	from, ok := airportByIdent(airports, req.From)
	if !ok {
		return DestinationIdeas{}, fmt.Errorf("unknown airport %q", req.From)
	}
	if req.Hours <= 0 {
		req.Hours = 1.5
	}
	if req.TasKt < 40 {
		req.TasKt = 100
	}
	legs := 1.0
	if req.RoundTrip {
		legs = 2
	}
	airborneMin := req.Hours*60/legs - destGroundMin
	if airborneMin < 15 {
		return DestinationIdeas{}, fmt.Errorf("%.1f h is too short for a flight there%s", req.Hours, map[bool]string{true: " and back", false: ""}[req.RoundTrip])
	}
	maxNm := float64(req.TasKt) * airborneMin / 60 / destRoutingFactor
	minNm := math.Max(15, maxNm*0.45)
	ideal := maxNm * 0.8

	res := DestinationIdeas{Ideas: []DestinationIdea{}, Notes: []string{}}
	sunset, hasSunset := sunsetUTC(from.Lat, from.Lon, now)
	if hasSunset && sunset.Before(now) {
		sunset, hasSunset = sunsetUTC(from.Lat, from.Lon, now.Add(24*time.Hour))
	}
	if hasSunset {
		res.Sunset = sunset.Format("15:04Z")
	}

	rwy := longestRunways(airports.RunwayGeometry)
	cands := []DestinationIdea{}
	for _, a := range airports.Airports {
		if len(a) < 5 || toFloat(a[4]) != 1 {
			continue
		}
		id, _ := a[0].(string)
		name, _ := a[1].(string)
		if id == from.Ident || !isIcaoIdent(id) || isMilitary(id, name) {
			continue
		}
		lat, lon := toFloat(a[2]), toFloat(a[3])
		if math.Abs(lat-from.Lat) > maxNm/60+0.1 {
			continue
		}
		d := greatCircleNm(from.Lat, from.Lon, lat, lon)
		if d < minNm || d > maxNm || rwy[id] < req.MinRunwayM || rwy[id] == 0 {
			continue
		}
		idea := DestinationIdea{Ident: id, Name: name, Lat: lat, Lon: lon, ElevFt: airports.Details[id].ElevationFt,
			DistanceNm: d, BearingDeg: trueBearing(from.Lat, from.Lon, lat, lon), RunwayM: rwy[id], Tags: []string{}}
		idea.FlightMin = d * destRoutingFactor / float64(req.TasKt) * 60

		score := 1 - math.Abs(d/ideal-1) // prefer using the time there is
		if scenery[id] {
			score += 3
			idea.Tags = append(idea.Tags, "Custom scenery installed")
		}
		if len(visited) > 0 && !visited[id] {
			score += 1.5
			idea.Tags = append(idea.Tags, "New for you")
		} else if visited[id] {
			score -= 0.5
			idea.Tags = append(idea.Tags, "Been there")
		}
		if idea.ElevFt >= mountainFieldFt {
			score += 1.5
			idea.Tags = append(idea.Tags, fmt.Sprintf("Mountain airfield %d ft", idea.ElevFt))
		}
		if idea.RunwayM < 800 {
			score += 0.8
			idea.Tags = append(idea.Tags, fmt.Sprintf("Short runway %d m", idea.RunwayM))
		}

		// Weather: the station at the airport or the nearest within 20 NM.
		var st *WxStation
		best := 20.0
		for i := range stations {
			sd := greatCircleNm(lat, lon, stations[i].Lat, stations[i].Lon)
			if stations[i].Ident == id {
				sd = -1
			}
			if sd < best {
				st, best = &stations[i], sd
			}
		}
		if st != nil {
			idea.Category, idea.Metar = st.Category, st.Metar
			switch st.Category {
			case "IFR", "LIFR":
				continue
			case "MVFR":
				score -= 2
				idea.Tags = append(idea.Tags, "Marginal weather")
			}
			if st.TafWarn != "" {
				score -= 1
				idea.Tags = append(idea.Tags, "Forecast worsening")
			}
		}

		// Daylight: there before sunset, and home again for a round trip.
		if hasSunset {
			done := now.Add(time.Duration((idea.FlightMin*legs + destGroundMin*legs) * float64(time.Minute)))
			arrSunset, ok := sunsetUTC(lat, lon, now)
			if req.RoundTrip {
				arrSunset, ok = sunset, true
			}
			if ok && done.After(arrSunset) {
				score -= 3
				idea.Tags = append(idea.Tags, map[bool]string{true: "Back after sunset", false: "Arrival after sunset"}[req.RoundTrip])
			}
		}
		idea.Score = score + jitter(id, now, req.Seed)*1.5
		cands = append(cands, idea)
	}
	sort.Slice(cands, func(i, j int) bool { return cands[i].Score > cands[j].Score })

	// Best few, spread over different directions.
	for gap := destMinBearingGap; gap >= 0 && len(res.Ideas) < destCount; gap -= 25 {
		for _, c := range cands {
			if len(res.Ideas) >= destCount {
				break
			}
			ok := true
			for _, p := range res.Ideas {
				if p.Ident == c.Ident || angleDiff(p.BearingDeg, c.BearingDeg) < gap {
					ok = false
					break
				}
			}
			if ok {
				res.Ideas = append(res.Ideas, c)
			}
		}
	}
	if len(res.Ideas) == 0 {
		res.Notes = append(res.Notes, fmt.Sprintf("No airport with a %d m runway and VFR weather %0.f-%0.f NM away", req.MinRunwayM, minNm, maxNm))
	}
	res.Notes = append(res.Notes, fmt.Sprintf("Range for %.1f h%s at %d kt: %.0f-%.0f NM", req.Hours,
		map[bool]string{true: " there and back", false: " one way"}[req.RoundTrip], req.TasKt, minNm, maxNm))
	if len(stations) == 0 {
		res.Notes = append(res.Notes, "No weather data - ideas aren't checked against the weather")
	}
	return res, nil
}

// destinationIdeas gathers the inputs for suggestDestinations.
func destinationIdeas(xplaneRoot string, airports AirportData, visited map[string]bool, req DestinationRequest) (DestinationIdeas, error) {
	from, ok := airportByIdent(airports, req.From)
	if !ok {
		return DestinationIdeas{}, fmt.Errorf("unknown airport %q", req.From)
	}
	hours := req.Hours
	if hours <= 0 {
		hours = 1.5
	}
	tas := math.Max(40, float64(req.TasKt))
	reach := tas*hours + 20
	padLat := reach / 60
	padLon := padLat / math.Max(0.2, math.Cos(from.Lat*math.Pi/180))
	now := nowUTC()
	stations, err := onlineStations(from.Lat-padLat, from.Lon-padLon, from.Lat+padLat, from.Lon+padLon)
	note := ""
	if err != nil {
		var stamp string
		var offErr error
		stations, stamp, offErr = offlineStations(xplaneRoot, airports, from.Lat-padLat, from.Lon-padLon, from.Lat+padLat, from.Lon+padLon)
		if offErr != nil {
			stations = nil
		} else {
			note = "Weather from X-Plane's METARs of " + stamp + "Z (offline)"
		}
	}
	for i := range stations {
		if stations[i].Taf != "" {
			stations[i].TafWarn = tafWarning(stations[i].Taf, now, now.Add(time.Duration(hours*float64(time.Hour))))
		}
	}
	res, err := suggestDestinations(airports, stations, visited, customSceneryAirports(xplaneRoot), req, now)
	if err == nil && note != "" {
		res.Notes = append(res.Notes, note)
	}
	return res, err
}
