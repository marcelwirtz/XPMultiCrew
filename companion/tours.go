package main

import (
	"bytes"
	"compress/gzip"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// Tours: a journey over several evenings, flown leg by leg - generated
// from a start airport, the number of evenings, the time per evening and
// the slower aircraft's speed, either as a round trip, one way to a
// destination or exploring in a direction. Stops are picked like the
// "where to tonight?" ideas (custom scenery, new airports, mountain
// airfields), the weather and the actual route are checked on the evening
// itself (auto route + briefing). A leg counts as flown once a recorded
// flight went from its start to its end, and every leg has a note for the
// travel journal. Tours are shared as a code to paste, so a buddy can fly
// the same one.

// TourRequest is the "new tour" form.
type TourRequest struct {
	Name        string    `json:"name"`
	From        string    `json:"from"`
	Legs        int       `json:"legs"`
	HoursPerLeg float64   `json:"hoursPerLeg"`
	TasKt       int       `json:"tasKt"`
	MinRunwayM  int       `json:"minRunwayM"`
	Mode        string    `json:"mode"`         // "loop", "oneway", "explore"
	To          string    `json:"to,omitempty"` // oneway
	Direction   float64   `json:"direction"`    // explore (and loop: the way out), degrees true
	Seed        int       `json:"seed"`
	Keep        []TourLeg `json:"keep,omitempty"` // re-roll: keep these legs, generate the rest
	Style       TourStyle `json:"style"`
}

// TourStyle is how the legs are flown (copied into the tour).
type TourStyle struct {
	CruiseFt        int  `json:"cruiseFt"`
	AvoidControlled bool `json:"avoidControlled"`
	RadioNav        bool `json:"radioNav"`
}

// TourLeg is one evening's flight.
type TourLeg struct {
	From       string   `json:"from"`
	To         string   `json:"to"`
	ToName     string   `json:"toName"`
	Lat        float64  `json:"lat"` // of To
	Lon        float64  `json:"lon"`
	DistanceNm float64  `json:"distanceNm"`
	FlightMin  float64  `json:"flightMin"`
	Tags       []string `json:"tags"`
	Note       string   `json:"note"`       // travel journal
	ManualDone bool     `json:"manualDone"` // ticked by hand (flown without the recorder)

	// Filled in when listed (from the logbook), not stored.
	Done     bool   `json:"done"`
	FlightID string `json:"flightId,omitempty"`
	DoneAt   int64  `json:"doneAt,omitempty"`
}

// Tour is a saved journey.
type Tour struct {
	ID          string      `json:"id"`
	Name        string      `json:"name"`
	Created     int64       `json:"created"`
	From        string      `json:"from"`
	FromName    string      `json:"fromName"`
	FromLat     float64     `json:"fromLat"`
	FromLon     float64     `json:"fromLon"`
	Mode        string      `json:"mode"`
	HoursPerLeg float64     `json:"hoursPerLeg"`
	TasKt       int         `json:"tasKt"`
	MinRunwayM  int         `json:"minRunwayM"`
	Direction   float64     `json:"direction"`
	To          string      `json:"to,omitempty"`
	Legs        []TourLeg   `json:"legs"`
	Style       TourStyle   `json:"style"`
	Notes       []string    `json:"notes,omitempty"` // generator remarks, not stored meaningfully
	Progress    *TourStatus `json:"progress,omitempty"`
}

// TourStatus summarises a tour's progress.
type TourStatus struct {
	Done       int     `json:"done"`
	Next       int     `json:"next"` // index of the next leg, -1 when finished
	FlownNm    float64 `json:"flownNm"`
	TotalNm    float64 `json:"totalNm"`
	FlownMin   float64 `json:"flownMin"`
	Finished   bool    `json:"finished"`
	LastFlight int64   `json:"lastFlight,omitempty"`
}

const (
	tourMaxLegs   = 20
	tourCodeMagic = "XPMCTOUR1:"
)

var tourIDPattern = regexp.MustCompile(`^[0-9]{8}-[0-9]{6}(-[0-9]+)?$`)

func toursDir() (string, error) {
	dir, err := userConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "xpmulticrew-companion", "tours"), nil
}

// airportInterest scores what makes an airport worth a stop, with tags.
func airportInterest(id string, elevFt, runwayM int, scenery, visited map[string]bool) (float64, []string) {
	score, tags := 0.0, []string{}
	if scenery[id] {
		score += 3
		tags = append(tags, "Custom scenery installed")
	}
	if len(visited) > 0 && !visited[id] {
		score += 1.5
		tags = append(tags, "New for you")
	}
	if elevFt >= mountainFieldFt {
		score += 1
		tags = append(tags, fmt.Sprintf("Mountain airfield %d ft", elevFt))
	}
	if runwayM < 700 {
		score += 0.3
		tags = append(tags, fmt.Sprintf("Short runway %d m", runwayM))
	}
	return score, tags
}

// generateTour picks the stops - pure, see tours_test.go.
func generateTour(airports AirportData, scenery, visited map[string]bool, req TourRequest, now time.Time) (Tour, error) {
	from, ok := airportByIdent(airports, req.From)
	if !ok {
		return Tour{}, fmt.Errorf("unknown airport %q", req.From)
	}
	if req.Legs < 1 || req.Legs > tourMaxLegs {
		return Tour{}, fmt.Errorf("a tour has 1 to %d legs", tourMaxLegs)
	}
	if req.HoursPerLeg <= 0 {
		req.HoursPerLeg = 1.5
	}
	if req.TasKt < 40 {
		req.TasKt = 100
	}
	switch req.Mode {
	case "loop", "oneway", "explore":
	default:
		req.Mode = "loop"
	}
	if req.Mode == "loop" && req.Legs < 2 {
		return Tour{}, errors.New("a round trip needs at least 2 legs")
	}
	var dest RouteWaypoint
	if req.Mode == "oneway" {
		if dest, ok = airportByIdent(airports, req.To); !ok {
			return Tour{}, fmt.Errorf("unknown destination %q", req.To)
		}
	}
	airborne := req.HoursPerLeg*60 - destGroundMin
	if airborne < 20 {
		return Tour{}, errors.New("give each evening at least 45 minutes")
	}
	maxLeg := float64(req.TasKt) * airborne / 60 / destRoutingFactor
	minLeg := math.Max(20, maxLeg*0.5)
	if req.Mode == "oneway" {
		need := greatCircleNm(from.Lat, from.Lon, dest.Lat, dest.Lon)
		if need > maxLeg*float64(req.Legs) {
			return Tour{}, fmt.Errorf("%s is %.0f NM away - %d legs of at most %.0f NM don't get there; add legs or time per evening", dest.Ident, need, req.Legs, maxLeg)
		}
	}

	t := Tour{Name: strings.TrimSpace(req.Name), Created: now.Unix(), From: from.Ident, FromName: from.Name, FromLat: from.Lat, FromLon: from.Lon,
		Mode: req.Mode, HoursPerLeg: req.HoursPerLeg, TasKt: req.TasKt, MinRunwayM: req.MinRunwayM, Direction: req.Direction, To: dest.Ident, Legs: []TourLeg{}}
	t.Style = req.Style
	rwy := longestRunways(airports.RunwayGeometry)
	used := map[string]bool{from.Ident: true}
	cur := from
	for _, k := range req.Keep {
		if len(t.Legs) >= req.Legs {
			break
		}
		t.Legs = append(t.Legs, k)
		used[k.To] = true
		if w, ok := airportByIdent(airports, k.To); ok {
			cur = w
		}
	}

	for k := len(t.Legs); k < req.Legs; k++ {
		left := req.Legs - k // legs still to go, including this one
		// Where this leg should head for.
		var target *RouteWaypoint
		heading := req.Direction
		switch {
		case req.Mode == "oneway":
			target = &dest
		case req.Mode == "loop" && k >= req.Legs/2:
			target = &from
		}
		if target != nil && left == 1 {
			// The last leg ends at the target.
			d := greatCircleNm(cur.Lat, cur.Lon, target.Lat, target.Lon)
			if d > maxLeg*1.2 {
				return Tour{}, fmt.Errorf("couldn't get back within reach of %s - try more legs or more time per evening", target.Ident)
			}
			t.Legs = append(t.Legs, tourLeg(airports, cur, *target, rwy, scenery, visited, req.TasKt))
			break
		}
		type cand struct {
			w     RouteWaypoint
			score float64
		}
		var best *cand
		for _, a := range airports.Airports {
			if len(a) < 5 || toFloat(a[4]) != 1 {
				continue
			}
			id, _ := a[0].(string)
			name, _ := a[1].(string)
			if used[id] || !isIcaoIdent(id) || isMilitary(id, name) || (target != nil && id == target.Ident) {
				continue
			}
			lat, lon := toFloat(a[2]), toFloat(a[3])
			if math.Abs(lat-cur.Lat) > maxLeg/60+0.1 {
				continue
			}
			d := greatCircleNm(cur.Lat, cur.Lon, lat, lon)
			if d < minLeg || d > maxLeg || rwy[id] < req.MinRunwayM || rwy[id] == 0 {
				continue
			}
			interest, _ := airportInterest(id, airports.Details[id].ElevationFt, rwy[id], scenery, visited)
			score := interest + (1 - math.Abs(d/(maxLeg*0.8)-1)) + jitter(id, now, req.Seed)*1.5
			if target != nil {
				// Head for the target, using the legs that are left evenly,
				// and never end up out of reach of it.
				toT := greatCircleNm(lat, lon, target.Lat, target.Lon)
				if toT > maxLeg*float64(left-1) {
					continue
				}
				ideal := greatCircleNm(cur.Lat, cur.Lon, target.Lat, target.Lon) * float64(left-1) / float64(left)
				score -= 6 * math.Abs(toT-ideal) / maxLeg
				// ...and roughly in its direction, no zigzag.
				off := angleDiff(trueBearing(cur.Lat, cur.Lon, lat, lon), trueBearing(cur.Lat, cur.Lon, target.Lat, target.Lon))
				score -= 2 * math.Max(0, off-30) / 90
			} else {
				score += 2 * math.Cos((trueBearing(cur.Lat, cur.Lon, lat, lon)-heading)*math.Pi/180)
				if req.Mode == "loop" {
					// Out and back: on the way out, stay where the rest of
					// the tour can still bring us home.
					if greatCircleNm(lat, lon, from.Lat, from.Lon) > maxLeg*float64(left-1) {
						continue
					}
				}
			}
			if best == nil || score > best.score {
				best = &cand{w: RouteWaypoint{Kind: "APT", Ident: id, Name: name, Lat: lat, Lon: lon}, score: score}
			}
		}
		if best == nil {
			return Tour{}, fmt.Errorf("no suitable stop for leg %d from %s (runway %d m, %.0f-%.0f NM)", k+1, cur.Ident, req.MinRunwayM, minLeg, maxLeg)
		}
		t.Legs = append(t.Legs, tourLeg(airports, cur, best.w, rwy, scenery, visited, req.TasKt))
		used[best.w.Ident] = true
		cur = best.w
	}
	if t.Name == "" {
		t.Name = fmt.Sprintf("%s tour (%d legs)", from.Ident, len(t.Legs))
	}
	return t, nil
}

// isMilitary spots air bases - not for a sightseeing stop: German
// military fields (ICAO ET..) and apt.dat's naming elsewhere ("Buechel
// AB", "Ramstein Air Base", "... AFB", "(mil)").
func isMilitary(ident, name string) bool {
	if strings.HasPrefix(ident, "ET") {
		return true
	}
	n := " " + strings.ToUpper(strings.NewReplacer("(", " ", ")", " ", "-", " ").Replace(name)) + " "
	for _, k := range []string{" AB ", " AFB ", " AIR BASE ", " AIRBASE ", " MIL ", " MILITARY ", " NAS ", " RAF ", " HELIPORT "} {
		if strings.Contains(n, k) {
			return true
		}
	}
	return false
}

func tourLeg(airports AirportData, from, to RouteWaypoint, rwy map[string]int, scenery, visited map[string]bool, tasKt int) TourLeg {
	d := greatCircleNm(from.Lat, from.Lon, to.Lat, to.Lon)
	_, tags := airportInterest(to.Ident, airports.Details[to.Ident].ElevationFt, rwy[to.Ident], scenery, visited)
	return TourLeg{From: from.Ident, To: to.Ident, ToName: to.Name, Lat: to.Lat, Lon: to.Lon, DistanceNm: math.Round(d),
		FlightMin: math.Round(d * destRoutingFactor / float64(tasKt) * 60), Tags: tags}
}

// applyProgress marks legs flown from the logbook: in order, a recorded
// flight from the leg's start to its end after the previous leg (and not
// before the tour was made).
func applyProgress(t *Tour, entries []LogbookEntry) {
	chrono := append([]LogbookEntry{}, entries...)
	sort.Slice(chrono, func(i, j int) bool { return chrono[i].Start < chrono[j].Start })
	st := &TourStatus{Next: -1}
	since := t.Created - 3600
	for i := range t.Legs {
		l := &t.Legs[i]
		l.Done, l.FlightID, l.DoneAt = false, "", 0
		st.TotalNm += l.DistanceNm
		for _, e := range chrono {
			if e.Start >= since && e.Departure == l.From && e.Arrival == l.To {
				l.Done, l.FlightID, l.DoneAt = true, e.ID, e.Start
				since = e.Start
				st.FlownMin += e.AirborneMin
				break
			}
		}
		if l.ManualDone {
			l.Done = true
		}
		if l.Done {
			st.Done++
			st.FlownNm += l.DistanceNm
			st.LastFlight = max(st.LastFlight, l.DoneAt)
		} else if st.Next < 0 {
			st.Next = i
		}
	}
	st.Finished = st.Done == len(t.Legs)
	st.FlownMin = math.Round(st.FlownMin)
	t.Progress = st
}

func saveTour(t *Tour) error {
	dir, err := toursDir()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	if t.ID == "" {
		base := time.Now().Format("20060102-150405")
		t.ID = base
		for n := 2; ; n++ {
			if _, err := os.Stat(filepath.Join(dir, t.ID+".json")); os.IsNotExist(err) {
				break
			}
			t.ID = fmt.Sprintf("%s-%d", base, n)
		}
	}
	if !tourIDPattern.MatchString(t.ID) {
		return errors.New("invalid tour id")
	}
	stored := *t
	stored.Progress, stored.Notes = nil, nil
	stored.Legs = append([]TourLeg{}, t.Legs...)
	for i := range stored.Legs {
		stored.Legs[i].Done, stored.Legs[i].FlightID, stored.Legs[i].DoneAt = false, "", 0
	}
	raw, err := json.MarshalIndent(stored, "", "  ")
	if err != nil {
		return err
	}
	tmp := filepath.Join(dir, t.ID+".json.tmp")
	if err := os.WriteFile(tmp, raw, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, filepath.Join(dir, t.ID+".json"))
}

func listTours() ([]Tour, error) {
	dir, err := toursDir()
	if err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return []Tour{}, nil
		}
		return nil, err
	}
	out := []Tour{}
	for _, e := range entries {
		id, ok := strings.CutSuffix(e.Name(), ".json")
		if !ok || !tourIDPattern.MatchString(id) {
			continue
		}
		raw, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			continue
		}
		var t Tour
		if json.Unmarshal(raw, &t) != nil {
			continue
		}
		t.ID = id
		out = append(out, t)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Created > out[j].Created })
	return out, nil
}

func deleteTour(id string) error {
	if !tourIDPattern.MatchString(id) {
		return errors.New("invalid tour id")
	}
	dir, err := toursDir()
	if err != nil {
		return err
	}
	return os.Remove(filepath.Join(dir, id+".json"))
}

// tourCode packs a tour (without progress and journal) into a short text
// to paste, e.g. into Discord.
func tourCode(t Tour) (string, error) {
	share := t
	share.ID, share.Progress, share.Notes = "", nil, nil
	share.Legs = append([]TourLeg{}, t.Legs...)
	for i := range share.Legs {
		share.Legs[i] = TourLeg{From: t.Legs[i].From, To: t.Legs[i].To, ToName: t.Legs[i].ToName, Lat: t.Legs[i].Lat, Lon: t.Legs[i].Lon,
			DistanceNm: t.Legs[i].DistanceNm, FlightMin: t.Legs[i].FlightMin, Tags: t.Legs[i].Tags}
	}
	raw, err := json.Marshal(share)
	if err != nil {
		return "", err
	}
	var buf bytes.Buffer
	zw := gzip.NewWriter(&buf)
	if _, err := zw.Write(raw); err != nil {
		return "", err
	}
	if err := zw.Close(); err != nil {
		return "", err
	}
	return tourCodeMagic + base64.RawURLEncoding.EncodeToString(buf.Bytes()), nil
}

func parseTourCode(code string, now time.Time) (Tour, error) {
	code = strings.Join(strings.Fields(code), "")
	payload, ok := strings.CutPrefix(code, tourCodeMagic)
	if !ok {
		return Tour{}, errors.New("that's not a tour code (it starts with " + tourCodeMagic + ")")
	}
	gz, err := base64.RawURLEncoding.DecodeString(payload)
	if err != nil {
		return Tour{}, errors.New("the tour code is damaged - copy it again")
	}
	zr, err := gzip.NewReader(bytes.NewReader(gz))
	if err != nil {
		return Tour{}, errors.New("the tour code is damaged - copy it again")
	}
	raw, err := io.ReadAll(io.LimitReader(zr, 1<<20))
	if err != nil {
		return Tour{}, errors.New("the tour code is damaged - copy it again")
	}
	var t Tour
	if err := json.Unmarshal(raw, &t); err != nil || len(t.Legs) == 0 || len(t.Legs) > tourMaxLegs {
		return Tour{}, errors.New("the tour code doesn't contain a tour")
	}
	t.ID, t.Created = "", now.Unix()
	return t, nil
}
