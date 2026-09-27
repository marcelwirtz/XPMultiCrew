package main

import (
	"compress/gzip"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

// Debrief: every flight is recorded automatically from the plugin's 1 Hz
// SELF_POS/PEER_POS - from engine start through takeoff and landing until
// the engine is shut down again - together with events
// (takeoff, landings with their Butter-Board rating, finished checklists,
// route waypoints) and the tracks of everyone else in the session. Saved
// as one gzipped JSON file per flight for the Debrief page.

// FlightEvent is a marker on the debrief timeline.
type FlightEvent struct {
	T     float64 `json:"t"` // seconds since the flight's start
	Kind  string  `json:"kind"`
	Label string  `json:"label"`
	Lat   float64 `json:"lat"`
	Lon   float64 `json:"lon"`
}

// PeerTrack is another aircraft of the session during our flight.
type PeerTrack struct {
	ID       uint32 `json:"id"`
	Callsign string `json:"callsign"`
	Icao     string `json:"icao"`
	// [t, lat, lon, alt_ft]
	Track [][4]float64 `json:"track"`
}

// Flight is one recorded flight.
type Flight struct {
	ID        string `json:"id"`
	Start     int64  `json:"start"` // unix seconds
	End       int64  `json:"end"`
	Icao      string `json:"icao"`
	Callsign  string `json:"callsign"`
	Departure string `json:"departure"`
	Arrival   string `json:"arrival"`
	// [t, lat, lon, alt_ft, gs_kt, ias_kt, vs_fpm, heading, on_ground]
	Track    [][9]float64  `json:"track"`
	Events   []FlightEvent `json:"events"`
	Peers    []PeerTrack   `json:"peers"`
	Landings []Landing     `json:"landings"`
}

// FlightSummary is one row of the Debrief page's flight list.
type FlightSummary struct {
	ID          string  `json:"id"`
	Start       int64   `json:"start"`
	End         int64   `json:"end"`
	Icao        string  `json:"icao"`
	Callsign    string  `json:"callsign"`
	Departure   string  `json:"departure"`
	Arrival     string  `json:"arrival"`
	AirborneMin float64 `json:"airborneMin"`
	DistanceNm  float64 `json:"distanceNm"`
	MaxAltFt    float64 `json:"maxAltFt"`
	Landings    int     `json:"landings"`
	BestScore   int     `json:"bestScore"` // -1 without a rated landing
	Peers       int     `json:"peers"`
}

// RecordingInfo tells the frontend a flight is being recorded right now.
type RecordingInfo struct {
	Start       int64   `json:"start"`
	AirborneMin float64 `json:"airborneMin"`
	Takeoff     bool    `json:"takeoff"`
}

func nmBetween(lat1, lon1, lat2, lon2 float64) float64 {
	dlat := toRad(lat2 - lat1)
	dlon := toRad(lon2 - lon1)
	a := math.Sin(dlat/2)*math.Sin(dlat/2) + math.Cos(toRad(lat1))*math.Cos(toRad(lat2))*math.Sin(dlon/2)*math.Sin(dlon/2)
	return 2 * earthRadiusM * math.Asin(math.Min(1, math.Sqrt(a))) / 1852
}

func summarize(f *Flight) FlightSummary {
	s := FlightSummary{ID: f.ID, Start: f.Start, End: f.End, Icao: f.Icao, Callsign: f.Callsign,
		Departure: f.Departure, Arrival: f.Arrival, Landings: len(f.Landings), BestScore: -1, Peers: len(f.Peers)}
	for i, p := range f.Track {
		s.MaxAltFt = math.Max(s.MaxAltFt, p[3])
		if i > 0 {
			prev := f.Track[i-1]
			s.DistanceNm += nmBetween(prev[1], prev[2], p[1], p[2])
			if p[8] == 0 || prev[8] == 0 {
				s.AirborneMin += (p[0] - prev[0]) / 60
			}
		}
	}
	for _, l := range f.Landings {
		if l.Score > s.BestScore {
			s.BestScore = l.Score
		}
	}
	s.DistanceNm = math.Round(s.DistanceNm*10) / 10
	s.AirborneMin = math.Round(s.AirborneMin*10) / 10
	return s
}

// --- Recorder -----------------------------------------------------------------

const (
	prerollMax        = 30 * time.Minute // taxi-out kept before takeoff
	parkedFor         = 60 * time.Second // standing still this long = parked
	airborneConfirm   = 3                // samples in the air before it's a takeoff
	finishEngineOff   = 5 * time.Second  // after landing: engine off this long ends the flight
	finishStillFor    = 60 * time.Second // same, for plugins that don't report the engine
	finishStillMax    = 15 * time.Minute // engine left running: standing still this long ends it anyway
	finishNoDataFor   = 2 * time.Minute  // X-Plane gone
	jumpNm            = 3.0              // position jump between two samples = reposition
	minAirborneMinute = 1.0              // shorter "flights" (hops) are dropped
)

type sample struct {
	at  time.Time
	pos MapPosition
}

// flightRecorder is pure (time passed in) - see flights_test.go.
type flightRecorder struct {
	preroll        []sample
	flight         *Flight
	start          time.Time
	airborneRun    int
	landedOnce     bool
	stillSince     time.Time
	engineOffSince time.Time
	lastSample     time.Time
	lastPos        *MapPosition
	lastOnGround   bool
	peerIdx        map[uint32]int
	pendingEvents  []FlightEvent
}

// feed adds one 1 Hz sample. Returns a finished flight (to be saved) or nil.
func (r *flightRecorder) feed(now time.Time, self *MapPosition, peers []MapPosition, peerInfo []FormationPeer,
	icao, callsign string) *Flight {
	var done *Flight
	if r.flight != nil && !r.lastSample.IsZero() && now.Sub(r.lastSample) > finishNoDataFor {
		done = r.finish()
	}
	if self == nil || !self.HasFlight {
		return done
	}
	if r.lastPos != nil && nmBetween(r.lastPos.Lat, r.lastPos.Lon, self.Lat, self.Lon) > jumpNm {
		// Reposition or new flight loaded: whatever was going on is over.
		if f := r.finish(); f != nil {
			done = f
		}
		r.preroll = nil
		r.pendingEvents = nil
		r.airborneRun = 0
	}
	if r.flight != nil && icao != "" && r.flight.Icao != "" && icao != r.flight.Icao {
		if f := r.finish(); f != nil {
			done = f
		}
		r.preroll = nil
	}
	pos := *self
	r.lastPos = &pos
	r.lastSample = now

	if r.flight == nil {
		r.preroll = append(r.preroll, sample{now, pos})
		for len(r.preroll) > 0 && now.Sub(r.preroll[0].at) > prerollMax {
			r.preroll = r.preroll[1:]
		}
		if pos.OnGround {
			r.airborneRun = 0
		} else {
			r.airborneRun++
		}
		if r.airborneRun >= airborneConfirm {
			r.begin(icao, callsign)
		}
		r.lastOnGround = pos.OnGround
		return done
	}

	r.add(now, pos, peers, peerInfo)
	if r.lastOnGround && !pos.OnGround {
		r.event(now, "takeoff", "Takeoff", pos.Lat, pos.Lon)
	}
	if !r.lastOnGround && pos.OnGround {
		r.landedOnce = true
	}
	r.lastOnGround = pos.OnGround
	if pos.OnGround && pos.GroundspeedKt < 2 {
		if r.stillSince.IsZero() {
			r.stillSince = now
		}
	} else {
		r.stillSince = time.Time{}
	}
	if !pos.HasEngine || pos.EnginesRunning || !pos.OnGround {
		r.engineOffSince = time.Time{}
	} else if r.engineOffSince.IsZero() {
		r.engineOffSince = now
	}
	if r.landedOnce && !r.stillSince.IsZero() {
		still := now.Sub(r.stillSince)
		switch {
		case pos.HasEngine && !r.engineOffSince.IsZero() && now.Sub(r.engineOffSince) >= finishEngineOff:
			return r.finish()
		case !pos.HasEngine && still >= finishStillFor:
			return r.finish()
		case still >= finishStillMax:
			return r.finish()
		}
	}
	return done
}

// begin starts a flight at the takeoff, with everything since the engine
// start before it (or, without engine data, since the aircraft was last
// parked).
func (r *flightRecorder) begin(icao, callsign string) {
	startIdx := 0
	var stillStart time.Time
	for i := range r.preroll {
		s := r.preroll[i]
		if s.pos.HasEngine {
			if !s.pos.EnginesRunning && s.pos.OnGround {
				startIdx = i + 1 // engine still off here - the flight starts after it
			}
			continue
		}
		if s.pos.GroundspeedKt < 2 && s.pos.OnGround {
			if stillStart.IsZero() {
				stillStart = s.at
			}
			if s.at.Sub(stillStart) >= parkedFor {
				startIdx = i // parked here - the flight starts at the end of it
			}
		} else {
			stillStart = time.Time{}
		}
	}
	samples := r.preroll[startIdx:]
	r.start = samples[0].at
	r.flight = &Flight{Start: r.start.Unix(), Icao: icao, Callsign: callsign, Track: [][9]float64{},
		Events: []FlightEvent{}, Peers: []PeerTrack{}, Landings: []Landing{}}
	r.peerIdx = map[uint32]int{}
	r.landedOnce = false
	r.stillSince = time.Time{}
	r.engineOffSince = time.Time{}
	wasOnGround := true
	takeoffLogged := false
	for _, s := range samples {
		r.add(s.at, s.pos, nil, nil)
		if wasOnGround && !s.pos.OnGround && !takeoffLogged {
			r.event(s.at, "takeoff", "Takeoff", s.pos.Lat, s.pos.Lon)
			takeoffLogged = true
		}
		wasOnGround = s.pos.OnGround
	}
	r.preroll = nil
	r.airborneRun = 0
	// Events from before the takeoff carry unix seconds (see addEvent).
	for _, e := range r.pendingEvents {
		if e.T >= float64(r.start.Unix())-prerollMax.Seconds() {
			e.T = math.Max(0, e.T-float64(r.start.Unix()))
			r.flight.Events = append(r.flight.Events, e)
		}
	}
	r.pendingEvents = nil
}

func (r *flightRecorder) add(now time.Time, p MapPosition, peers []MapPosition, info []FormationPeer) {
	t := now.Sub(r.start).Seconds()
	g := 0.0
	if p.OnGround {
		g = 1
	}
	r.flight.Track = append(r.flight.Track, [9]float64{t, p.Lat, p.Lon, math.Round(p.AltFt), math.Round(p.GroundspeedKt),
		math.Round(p.IasKt), math.Round(p.VsFpm), math.Round(p.Heading), g})
	for _, peer := range peers {
		idx, ok := r.peerIdx[peer.ID]
		if !ok {
			pt := PeerTrack{ID: peer.ID, Track: [][4]float64{}}
			for _, fi := range info {
				if fi.ID == peer.ID {
					pt.Callsign, pt.Icao = fi.Callsign, fi.ICAO
				}
			}
			r.flight.Peers = append(r.flight.Peers, pt)
			idx = len(r.flight.Peers) - 1
			r.peerIdx[peer.ID] = idx
		}
		r.flight.Peers[idx].Track = append(r.flight.Peers[idx].Track, [4]float64{t, peer.Lat, peer.Lon, math.Round(peer.AltFt)})
	}
}

func (r *flightRecorder) event(now time.Time, kind, label string, lat, lon float64) {
	if r.flight == nil {
		return
	}
	r.flight.Events = append(r.flight.Events, FlightEvent{T: now.Sub(r.start).Seconds(), Kind: kind, Label: label, Lat: lat, Lon: lon})
}

// addEvent records an event from the frontend (checklist done, waypoint
// passed) at the current position. Before takeoff it's kept for the flight
// about to start (a checklist done while taxiing belongs to it).
func (r *flightRecorder) addEvent(now time.Time, kind, label string) {
	lat, lon := 0.0, 0.0
	if r.lastPos != nil {
		lat, lon = r.lastPos.Lat, r.lastPos.Lon
	}
	if r.flight != nil {
		r.event(now, kind, label, lat, lon)
		return
	}
	if len(r.preroll) == 0 {
		return
	}
	// Times are relative to the flight's start, which isn't known yet:
	// stored as unix seconds until begin() converts them.
	r.pendingEvents = append(r.pendingEvents, FlightEvent{T: float64(now.Unix()), Kind: kind, Label: label, Lat: lat, Lon: lon})
	if len(r.pendingEvents) > 50 {
		r.pendingEvents = r.pendingEvents[1:]
	}
}

// addLanding attaches a rated landing of our own.
func (r *flightRecorder) addLanding(l Landing) {
	if r.flight == nil {
		return
	}
	r.flight.Landings = append(r.flight.Landings, l)
	label := fmt.Sprintf("%s %.0f fpm - %d", map[bool]string{true: "Touch & go", false: "Landing"}[l.TouchAndGo], l.VsFpm, l.Score)
	if l.Runway != "" {
		label += " (" + l.Airport + " " + l.Runway + ")"
	}
	t := float64(l.Time) - float64(r.flight.Start)
	if t < 0 || len(r.flight.Track) == 0 || t > r.flight.Track[len(r.flight.Track)-1][0]+30 {
		t = r.flight.Track[len(r.flight.Track)-1][0]
	}
	kind := "landing"
	if l.TouchAndGo {
		kind = "touchgo"
	}
	r.flight.Events = append(r.flight.Events, FlightEvent{T: t, Kind: kind, Label: label, Lat: l.Lat, Lon: l.Lon})
}

// finish ends the current flight; nil if there was none or it was too short.
func (r *flightRecorder) finish() *Flight {
	f := r.flight
	r.flight = nil
	r.peerIdx = nil
	r.airborneRun = 0
	r.stillSince = time.Time{}
	r.engineOffSince = time.Time{}
	r.landedOnce = false
	if f == nil || len(f.Track) < 2 {
		return nil
	}
	sort.SliceStable(f.Events, func(i, j int) bool { return f.Events[i].T < f.Events[j].T })
	f.End = f.Start + int64(f.Track[len(f.Track)-1][0])
	if summarize(f).AirborneMin < minAirborneMinute {
		return nil
	}
	f.ID = time.Unix(f.Start, 0).UTC().Format("20060102-150405")
	return f
}

func (r *flightRecorder) recording() *RecordingInfo {
	if r.flight == nil {
		return nil
	}
	s := summarize(r.flight)
	return &RecordingInfo{Start: r.flight.Start, AirborneMin: s.AirborneMin, Takeoff: true}
}

// snapshot is a copy of the flight in progress (the Debrief page can show
// it live), nil if none.
func (r *flightRecorder) snapshot() *Flight {
	if r.flight == nil {
		return nil
	}
	raw, err := json.Marshal(r.flight)
	if err != nil {
		return nil
	}
	var f Flight
	if json.Unmarshal(raw, &f) != nil {
		return nil
	}
	f.ID = "current"
	f.End = f.Start
	if len(f.Track) > 0 {
		f.End = f.Start + int64(f.Track[len(f.Track)-1][0])
	}
	sort.SliceStable(f.Events, func(i, j int) bool { return f.Events[i].T < f.Events[j].T })
	return &f
}

// --- Storage ------------------------------------------------------------------

var flightIDPattern = regexp.MustCompile(`^[0-9]{8}-[0-9]{6}$`)

func flightsDir() (string, error) {
	dir, err := userConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "xpmulticrew-companion", "flights"), nil
}

func saveFlight(f *Flight) error {
	dir, err := flightsDir()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	path := filepath.Join(dir, f.ID+".json.gz")
	tmp := path + ".tmp"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	zw := gzip.NewWriter(out)
	if err := json.NewEncoder(zw).Encode(f); err != nil {
		zw.Close()
		out.Close()
		os.Remove(tmp)
		return err
	}
	if err := zw.Close(); err != nil {
		out.Close()
		os.Remove(tmp)
		return err
	}
	if err := out.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, path)
}

func loadFlight(id string) (*Flight, error) {
	if !flightIDPattern.MatchString(id) {
		return nil, errors.New("invalid flight id")
	}
	dir, err := flightsDir()
	if err != nil {
		return nil, err
	}
	in, err := os.Open(filepath.Join(dir, id+".json.gz"))
	if err != nil {
		return nil, err
	}
	defer in.Close()
	zr, err := gzip.NewReader(in)
	if err != nil {
		return nil, err
	}
	defer zr.Close()
	var f Flight
	if err := json.NewDecoder(zr).Decode(&f); err != nil {
		return nil, err
	}
	return &f, nil
}

func deleteFlight(id string) error {
	if !flightIDPattern.MatchString(id) {
		return errors.New("invalid flight id")
	}
	dir, err := flightsDir()
	if err != nil {
		return err
	}
	return os.Remove(filepath.Join(dir, id+".json.gz"))
}

// flightIndex caches summaries, so the list doesn't decompress every
// flight each time the page opens.
type flightIndex struct {
	mu    sync.Mutex
	cache map[string]FlightSummary
}

func (x *flightIndex) list() ([]FlightSummary, error) {
	dir, err := flightsDir()
	if err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return []FlightSummary{}, nil
		}
		return nil, err
	}
	x.mu.Lock()
	defer x.mu.Unlock()
	if x.cache == nil {
		x.cache = map[string]FlightSummary{}
	}
	out := []FlightSummary{}
	for _, e := range entries {
		id, ok := strings.CutSuffix(e.Name(), ".json.gz")
		if !ok || !flightIDPattern.MatchString(id) {
			continue
		}
		s, cached := x.cache[id]
		if !cached {
			f, err := loadFlight(id)
			if err != nil {
				continue
			}
			s = summarize(f)
			x.cache[id] = s
		}
		out = append(out, s)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Start > out[j].Start })
	return out, nil
}

func (x *flightIndex) forget(id string) {
	x.mu.Lock()
	defer x.mu.Unlock()
	delete(x.cache, id)
}
