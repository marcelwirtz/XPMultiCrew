package main

import (
	"testing"
	"time"
)

// flightSim drives a flightRecorder with a 1 Hz aircraft moving north.
type flightSim struct {
	r     flightRecorder
	now   time.Time
	pos   MapPosition
	saved []*Flight
	peers []MapPosition
}

func newFlightSim() *flightSim {
	return &flightSim{now: time.Unix(1790000000, 0), pos: MapPosition{Lat: 50, Lon: 8, HasFlight: true, OnGround: true}}
}

func (s *flightSim) run(seconds int, gsKt, altFt float64, onGround bool) {
	for i := 0; i < seconds; i++ {
		s.now = s.now.Add(time.Second)
		s.pos.GroundspeedKt = gsKt
		s.pos.AltFt = altFt
		s.pos.OnGround = onGround
		s.pos.Lat += gsKt / 3600 / 60 // nm per second -> degrees
		p := s.pos
		if f := s.r.feed(s.now, &p, s.peers, []FormationPeer{{ID: 7, ICAO: "PA28", Callsign: "DEFGH"}}, "C172", "DEABC"); f != nil {
			s.saved = append(s.saved, f)
		}
	}
}

func TestFlightRecorderWholeFlight(t *testing.T) {
	s := newFlightSim()
	s.run(600, 0, 400, true) // parked 10 min - not part of the flight
	s.r.addEvent(s.now, "checklist", "Before Start")
	s.run(120, 0, 400, true)  // still parked
	s.run(300, 12, 400, true) // taxi out
	s.run(40, 60, 400, true)  // takeoff roll
	if s.r.recording() != nil {
		t.Fatal("no flight before takeoff")
	}
	s.peers = []MapPosition{{ID: 7, Lat: 50.01, Lon: 8, AltFt: 2000}}
	s.run(1200, 100, 3000, false) // 20 min flight
	if s.r.recording() == nil {
		t.Fatal("flight should be recording")
	}
	s.r.addEvent(s.now, "waypoint", "EDFE")
	s.r.addLanding(Landing{Key: "0:1", Time: s.now.Unix(), VsFpm: -120, Score: 88, Airport: "EDFE", Runway: "25"})
	s.peers = nil
	s.run(60, 40, 400, true)  // rollout
	s.run(200, 10, 400, true) // taxi in
	if len(s.saved) != 0 {
		t.Fatal("flight must not end while taxiing")
	}
	s.run(61, 0, 400, true) // parked
	if len(s.saved) != 1 {
		t.Fatalf("expected the flight saved after parking, got %d", len(s.saved))
	}
	f := s.saved[0]
	sum := summarize(f)
	if sum.AirborneMin < 19.5 || sum.AirborneMin > 20.5 {
		t.Fatalf("airborne time wrong: %v", sum.AirborneMin)
	}
	// Taxi-out included, the long parking before it not.
	if len(f.Track) < 300+40+1200+60+200 || len(f.Track) > 300+40+1200+60+200+62+2 {
		t.Fatalf("track length %d", len(f.Track))
	}
	if f.Icao != "C172" || f.Callsign != "DEABC" || !flightIDPattern.MatchString(f.ID) {
		t.Fatalf("flight metadata wrong: %+v", f.ID)
	}
	// The peer's first seconds fall in the takeoff confirmation, recorded
	// before the flight started.
	if len(f.Peers) != 1 || f.Peers[0].Callsign != "DEFGH" || len(f.Peers[0].Track) < 1195 {
		t.Fatalf("peer track wrong: %+v", len(f.Peers))
	}
	kinds := []string{}
	for _, e := range f.Events {
		kinds = append(kinds, e.Kind)
	}
	want := []string{"checklist", "takeoff", "waypoint", "landing"}
	if len(kinds) != len(want) {
		t.Fatalf("events %v, want %v", kinds, want)
	}
	for i := range want {
		if kinds[i] != want[i] {
			t.Fatalf("events %v, want %v", kinds, want)
		}
	}
	if f.Events[0].T != 0 {
		t.Fatalf("checklist done while parked belongs at the start: %v", f.Events[0].T)
	}
	if len(f.Landings) != 1 || sum.BestScore != 88 || sum.Landings != 1 {
		t.Fatalf("landings wrong: %+v", sum)
	}
}

func TestFlightRecorderDropsHopsAndHandlesJumps(t *testing.T) {
	s := newFlightSim()
	s.run(30, 50, 400, true)
	s.run(20, 60, 450, false) // 20 s in the air: too short
	s.run(100, 0, 400, true)
	if len(s.saved) != 0 {
		t.Fatal("a 20 s hop is no flight")
	}

	s.run(200, 100, 3000, false) // flying
	s.pos.Lat += 1               // placed somewhere else by the map
	s.run(5, 0, 400, true)
	if len(s.saved) != 1 {
		t.Fatalf("a reposition ends the flight, got %d", len(s.saved))
	}
	if s.r.recording() != nil {
		t.Fatal("nothing recording after the jump")
	}
}

func TestFlightRecorderNoDataEndsFlight(t *testing.T) {
	s := newFlightSim()
	s.run(10, 60, 400, true)
	s.run(300, 100, 3000, false)
	s.now = s.now.Add(3 * time.Minute) // X-Plane crashed
	if f := s.r.feed(s.now, nil, nil, nil, "", ""); f == nil {
		t.Fatal("flight should end after no data")
	}
}

func TestFlightSaveLoad(t *testing.T) {
	useTempConfigDir(t)
	s := newFlightSim()
	s.run(10, 60, 400, true)
	s.run(200, 100, 3000, false)
	s.run(100, 0, 400, true)
	if len(s.saved) != 1 {
		t.Fatal("expected a flight")
	}
	f := s.saved[0]
	if err := saveFlight(f); err != nil {
		t.Fatal(err)
	}
	var idx flightIndex
	list, err := idx.list()
	if err != nil || len(list) != 1 || list[0].ID != f.ID {
		t.Fatalf("list: %+v %v", list, err)
	}
	back, err := loadFlight(f.ID)
	if err != nil || len(back.Track) != len(f.Track) {
		t.Fatalf("load: %v", err)
	}
	if _, err := loadFlight("../../etc/passwd"); err == nil {
		t.Fatal("ids must be validated")
	}
	if err := deleteFlight(f.ID); err != nil {
		t.Fatal(err)
	}
	idx.forget(f.ID)
	if list, _ := idx.list(); len(list) != 0 {
		t.Fatal("flight not deleted")
	}
}

// With engine data (plugin v0.4.0+) the flight runs from engine start to
// engine shutdown - waiting on the runway with the engine running doesn't
// end it.
func TestFlightRecorderEngineStartToShutdown(t *testing.T) {
	s := newFlightSim()
	s.pos.HasEngine = true
	s.run(120, 0, 400, true) // parked, engine off
	s.pos.EnginesRunning = true
	s.run(90, 0, 400, true) // engine running, warming up
	s.run(60, 12, 400, true)
	s.run(200, 100, 3000, false)
	s.run(20, 40, 400, true)
	s.run(300, 0, 400, true) // stopped on the runway, engine running
	if len(s.saved) != 0 {
		t.Fatal("a running engine keeps the flight going")
	}
	s.run(60, 10, 400, true) // taxi in
	s.run(30, 0, 400, true)
	s.pos.EnginesRunning = false
	s.run(4, 0, 400, true)
	if len(s.saved) != 0 {
		t.Fatal("ends only after the engine has been off a few seconds")
	}
	s.run(3, 0, 400, true)
	if len(s.saved) != 1 {
		t.Fatalf("engine shutdown should end the flight, got %d", len(s.saved))
	}
	f := s.saved[0]
	// Starts at engine start (warm-up included), the parking before it not.
	want := 90 + 60 + 200 + 20 + 300 + 60 + 30 + 5
	if n := len(f.Track); n < want-3 || n > want+3 {
		t.Fatalf("track has %d samples, want about %d", n, want)
	}
}

func TestFlightRecorderEngineLeftRunning(t *testing.T) {
	s := newFlightSim()
	s.pos.HasEngine = true
	s.pos.EnginesRunning = true
	s.run(10, 60, 400, true)
	s.run(200, 100, 3000, false)
	s.run(20, 40, 400, true)
	s.run(14*60, 0, 400, true)
	if len(s.saved) != 0 {
		t.Fatal("too early")
	}
	s.run(62, 0, 400, true)
	if len(s.saved) != 1 {
		t.Fatal("15 min standing still ends the flight even with the engine running")
	}
}
