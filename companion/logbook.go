package main

import (
	"fmt"
	"math"
	"sort"
	"strings"
	"sync"
	"time"
)

// Logbook: every recorded flight (flights.go) as a logbook entry, with
// who flew along, totals, flying buddies, the airports you've been to and
// milestones ("first night landing", "10 hours together"...). Each
// companion records the flight with everyone in the session, so both
// pilots of a flight together end up with the same entries without any
// syncing.

// LogbookEntry is one flight.
type LogbookEntry struct {
	ID          string       `json:"id"`
	Start       int64        `json:"start"`
	Icao        string       `json:"icao"`
	Departure   string       `json:"departure"`
	Arrival     string       `json:"arrival"`
	AirborneMin float64      `json:"airborneMin"`
	DistanceNm  float64      `json:"distanceNm"`
	MaxAltFt    float64      `json:"maxAltFt"`
	Landings    int          `json:"landings"`
	BestScore   int          `json:"bestScore"`
	Buddies     []string     `json:"buddies"`
	Night       bool         `json:"night"` // a landing in the dark
	Track       [][2]float64 `json:"track"` // [lon, lat], thinned out for the overview map
}

// LogbookTotals sums everything up.
type LogbookTotals struct {
	Flights         int     `json:"flights"`
	AirborneMin     float64 `json:"airborneMin"`
	DistanceNm      float64 `json:"distanceNm"`
	Landings        int     `json:"landings"`
	Airports        int     `json:"airports"`
	FlightsTogether int     `json:"flightsTogether"`
	MinTogether     float64 `json:"minTogether"`
	BestScore       int     `json:"bestScore"`
	NightLandings   int     `json:"nightLandings"`
}

// BuddyStat is one flying buddy.
type BuddyStat struct {
	Callsign string  `json:"callsign"`
	Flights  int     `json:"flights"`
	Minutes  float64 `json:"minutes"`
	Last     int64   `json:"last"`
}

// VisitedAirport is one airport on the overview map.
type VisitedAirport struct {
	Ident  string  `json:"ident"`
	Name   string  `json:"name"`
	Lat    float64 `json:"lat"`
	Lon    float64 `json:"lon"`
	Visits int     `json:"visits"`
}

// Milestone is a first or a record.
type Milestone struct {
	Title    string `json:"title"`
	Detail   string `json:"detail"`
	Date     int64  `json:"date"`
	FlightID string `json:"flightId"`
}

// Logbook is what the Logbook page shows.
type Logbook struct {
	Entries    []LogbookEntry   `json:"entries"` // newest first
	Totals     LogbookTotals    `json:"totals"`
	Buddies    []BuddyStat      `json:"buddies"`
	Airports   []VisitedAirport `json:"airports"`
	Milestones []Milestone      `json:"milestones"` // newest first
}

const (
	logTrackPoints   = 80
	butterScore      = 90   // Butter-Board score that counts as a butter landing
	mountainFieldFt  = 2000 // airport elevation for "mountain airfield"
	nightAfterSunset = 30 * time.Minute
)

var (
	logEntryMu    sync.Mutex
	logEntryCache = map[string]LogbookEntry{} // flight files don't change once saved
)

// sunTimes returns sunrise and sunset (UTC) for the day of t.
func sunTimes(lat, lon float64, t time.Time) (rise, set time.Time, ok bool) {
	set, ok = sunsetUTC(lat, lon, t)
	if !ok {
		return
	}
	// The sunrise equation is symmetric around solar noon.
	noon := solarNoonUTC(lon, t)
	rise = noon.Add(-set.Sub(noon))
	return rise, set, true
}

func solarNoonUTC(lon float64, t time.Time) time.Time {
	const rad = math.Pi / 180
	jd := float64(t.Truncate(24*time.Hour).Unix())/86400 + 2440587.5
	n := math.Ceil(jd - 2451545.0 + 0.0008)
	jStar := n - lon/360
	m := math.Mod(357.5291+0.98560028*jStar, 360)
	c := 1.9148*math.Sin(m*rad) + 0.02*math.Sin(2*m*rad) + 0.0003*math.Sin(3*m*rad)
	lambda := math.Mod(m+c+180+102.9372, 360)
	transit := 2451545.0 + jStar + 0.0053*math.Sin(m*rad) - 0.0069*math.Sin(2*lambda*rad)
	return time.Unix(int64((transit-2440587.5)*86400), 0).UTC()
}

// isNight: more than 30 min after sunset or before sunrise - by the real
// clock the landing was recorded with (the sim's own time isn't recorded).
func isNight(lat, lon float64, at time.Time) bool {
	rise, set, ok := sunTimes(lat, lon, at)
	if !ok {
		return false // midnight sun or polar night - leave it
	}
	return at.After(set.Add(nightAfterSunset)) || at.Before(rise.Add(-nightAfterSunset))
}

// logbookEntry turns a recorded flight into a logbook entry.
func logbookEntry(f *Flight) LogbookEntry {
	s := summarize(f)
	e := LogbookEntry{ID: f.ID, Start: f.Start, Icao: f.Icao, Departure: f.Departure, Arrival: f.Arrival,
		AirborneMin: s.AirborneMin, DistanceNm: s.DistanceNm, MaxAltFt: math.Round(s.MaxAltFt), Landings: s.Landings,
		BestScore: s.BestScore, Buddies: []string{}, Track: [][2]float64{}}
	seen := map[string]bool{}
	for _, p := range f.Peers {
		name := strings.TrimSpace(p.Callsign)
		if name == "" {
			name = fmt.Sprintf("#%d", p.ID)
		}
		if !seen[name] && len(p.Track) > 0 {
			seen[name] = true
			e.Buddies = append(e.Buddies, name)
		}
	}
	for _, l := range f.Landings {
		if l.Own && isNight(l.Lat, l.Lon, time.Unix(l.Time, 0).UTC()) {
			e.Night = true
		}
	}
	step := max(1, len(f.Track)/logTrackPoints)
	for i := 0; i < len(f.Track); i += step {
		e.Track = append(e.Track, [2]float64{math.Round(f.Track[i][2]*1e4) / 1e4, math.Round(f.Track[i][1]*1e4) / 1e4})
	}
	if n := len(f.Track); n > 0 && (n-1)%step != 0 {
		e.Track = append(e.Track, [2]float64{math.Round(f.Track[n-1][2]*1e4) / 1e4, math.Round(f.Track[n-1][1]*1e4) / 1e4})
	}
	return e
}

// loadLogbookEntries reads (and caches) the entries for the given flights.
func loadLogbookEntries(summaries []FlightSummary) []LogbookEntry {
	out := []LogbookEntry{}
	for _, s := range summaries {
		logEntryMu.Lock()
		e, ok := logEntryCache[s.ID]
		logEntryMu.Unlock()
		if !ok {
			f, err := loadFlight(s.ID)
			if err != nil {
				continue
			}
			e = logbookEntry(f)
			logEntryMu.Lock()
			logEntryCache[s.ID] = e
			logEntryMu.Unlock()
		}
		out = append(out, e)
	}
	return out
}

// buildLogbook adds up the entries (any order) - pure, see logbook_test.go.
func buildLogbook(entries []LogbookEntry, airports AirportData) Logbook {
	lb := Logbook{Entries: entries, Buddies: []BuddyStat{}, Airports: []VisitedAirport{}, Milestones: []Milestone{}}
	sort.Slice(lb.Entries, func(i, j int) bool { return lb.Entries[i].Start > lb.Entries[j].Start })
	lb.Totals.BestScore = -1

	// Chronological pass for totals and milestones.
	chrono := append([]LogbookEntry{}, lb.Entries...)
	sort.Slice(chrono, func(i, j int) bool { return chrono[i].Start < chrono[j].Start })
	buddies := map[string]*BuddyStat{}
	visits := map[string]int{}
	add := func(e LogbookEntry, title, detail string) {
		lb.Milestones = append(lb.Milestones, Milestone{Title: title, Detail: detail, Date: e.Start, FlightID: e.ID})
	}
	thresholds := func(before, after float64, steps []float64, f func(float64)) {
		for _, s := range steps {
			if before < s && after >= s {
				f(s)
			}
		}
	}
	var longest, highest float64
	firstMountain, firstNight, firstButter := false, false, false
	for k, e := range chrono {
		t := &lb.Totals
		prevHours, prevTogether, prevAirports := t.AirborneMin/60, t.FlightsTogether, len(visits)
		t.Flights++
		t.AirborneMin += e.AirborneMin
		t.DistanceNm += e.DistanceNm
		t.Landings += e.Landings
		if e.BestScore > t.BestScore {
			t.BestScore = e.BestScore
		}
		if e.Night {
			t.NightLandings++
		}
		route := strings.Trim(e.Departure+" → "+e.Arrival, " →")
		if k == 0 {
			add(e, "First recorded flight", route)
		}
		if len(e.Buddies) > 0 {
			if t.FlightsTogether == 0 {
				add(e, "First flight together", "with "+strings.Join(e.Buddies, ", ")+" · "+route)
			}
			t.FlightsTogether++
			t.MinTogether += e.AirborneMin
			for _, b := range e.Buddies {
				st := buddies[b]
				if st == nil {
					st = &BuddyStat{Callsign: b}
					buddies[b] = st
				}
				st.Flights++
				st.Minutes += e.AirborneMin
				st.Last = e.Start
			}
		}
		for _, id := range []string{e.Departure, e.Arrival} {
			if id == "" {
				continue
			}
			if visits[id] == 0 && airports.Details[id].ElevationFt >= mountainFieldFt && !firstMountain {
				firstMountain = true
				add(e, "First mountain airfield", fmt.Sprintf("%s (%d ft)", id, airports.Details[id].ElevationFt))
			}
			visits[id]++
		}
		if e.Night && !firstNight {
			firstNight = true
			add(e, "First night landing", route)
		}
		if e.BestScore >= butterScore && !firstButter {
			firstButter = true
			add(e, "First butter landing", fmt.Sprintf("%d points · %s", e.BestScore, route))
		}
		if k > 0 && e.DistanceNm > longest && e.DistanceNm >= 50 {
			add(e, "Longest flight so far", fmt.Sprintf("%.0f NM · %s", e.DistanceNm, route))
		}
		if k > 0 && e.MaxAltFt > highest+500 && e.MaxAltFt >= 5000 {
			add(e, "Highest flight so far", fmt.Sprintf("%.0f ft", e.MaxAltFt))
		}
		longest, highest = math.Max(longest, e.DistanceNm), math.Max(highest, e.MaxAltFt)
		thresholds(prevHours, t.AirborneMin/60, []float64{1, 5, 10, 25, 50, 100, 250, 500}, func(h float64) {
			add(e, map[bool]string{true: "First hour in the air", false: fmt.Sprintf("%.0f hours in the air", h)}[h == 1], "")
		})
		thresholds(float64(prevTogether), float64(t.FlightsTogether), []float64{5, 10, 25, 50, 100}, func(n float64) {
			add(e, fmt.Sprintf("%.0f flights together", n), "")
		})
		thresholds(float64(prevAirports), float64(len(visits)), []float64{5, 10, 25, 50, 100}, func(n float64) {
			add(e, fmt.Sprintf("%.0f airports visited", n), "")
		})
	}
	lb.Totals.Airports = len(visits)
	lb.Totals.AirborneMin = math.Round(lb.Totals.AirborneMin)
	lb.Totals.MinTogether = math.Round(lb.Totals.MinTogether)
	lb.Totals.DistanceNm = math.Round(lb.Totals.DistanceNm)
	for _, b := range buddies {
		b.Minutes = math.Round(b.Minutes)
		lb.Buddies = append(lb.Buddies, *b)
	}
	sort.Slice(lb.Buddies, func(i, j int) bool { return lb.Buddies[i].Minutes > lb.Buddies[j].Minutes })
	for id, n := range visits {
		if w, ok := airportByIdent(airports, id); ok {
			lb.Airports = append(lb.Airports, VisitedAirport{Ident: id, Name: w.Name, Lat: w.Lat, Lon: w.Lon, Visits: n})
		}
	}
	sort.Slice(lb.Airports, func(i, j int) bool { return lb.Airports[i].Ident < lb.Airports[j].Ident })
	sort.SliceStable(lb.Milestones, func(i, j int) bool { return lb.Milestones[i].Date > lb.Milestones[j].Date })
	return lb
}
