package main

import (
	"fmt"
	"math"
	"strings"
	"testing"
	"time"
)

// A grid of airports every 0.5° around 50 N 8 E, each with an 800 m runway.
func tourTestAirports() AirportData {
	ap := AirportData{Details: map[string]AirportDetail{}}
	n := 0
	for lat := 47.0; lat <= 53.01; lat += 0.5 {
		for lon := 4.0; lon <= 12.01; lon += 0.5 {
			id := fmt.Sprintf("E%c%c%c", 'A'+n/676%26, 'A'+n/26%26, 'A'+n%26)
			n++
			ap.Airports = append(ap.Airports, []interface{}{id, "Field " + id, lat, lon, 1.0})
			ap.RunwayGeometry = append(ap.RunwayGeometry, RunwayGeometry{Airport: id, Ends: [2]RunwayEnd{{Name: "18", Lat: lat + 0.0036, Lon: lon}, {Name: "36", Lat: lat - 0.0036, Lon: lon}}})
		}
	}
	return ap
}

func identAt(ap AirportData, lat, lon float64) string {
	for _, a := range ap.Airports {
		if math.Abs(a[2].(float64)-lat) < 0.01 && math.Abs(a[3].(float64)-lon) < 0.01 {
			return a[0].(string)
		}
	}
	return ""
}

func checkLegs(t *testing.T, tour Tour, maxLeg float64) {
	t.Helper()
	seen := map[string]bool{}
	for i, l := range tour.Legs {
		if i > 0 && l.From != tour.Legs[i-1].To {
			t.Fatalf("leg %d doesn't start where %d ended: %+v", i, i-1, tour.Legs)
		}
		if l.DistanceNm > maxLeg*1.2+1 {
			t.Fatalf("leg %d too long (%.0f NM)", i, l.DistanceNm)
		}
		if seen[l.To] && i != len(tour.Legs)-1 {
			t.Fatalf("stop %s used twice", l.To)
		}
		seen[l.To] = true
	}
}

func TestGenerateTourModes(t *testing.T) {
	ap := tourTestAirports()
	start := identAt(ap, 50, 8)
	now := time.Date(2026, 10, 4, 12, 0, 0, 0, time.UTC)
	maxLeg := 100 * (90.0 - destGroundMin) / 60 / destRoutingFactor // 1.5 h at 100 kt

	loop, err := generateTour(ap, nil, nil, TourRequest{From: start, Legs: 5, HoursPerLeg: 1.5, TasKt: 100, Mode: "loop", Direction: 0}, now)
	if err != nil {
		t.Fatal(err)
	}
	if len(loop.Legs) != 5 || loop.Legs[0].From != start || loop.Legs[4].To != start {
		t.Fatalf("a loop starts and ends at home: %+v", loop.Legs)
	}
	checkLegs(t, loop, maxLeg)
	if loop.Legs[0].Lat <= 50 {
		t.Fatalf("heading north, the first stop should be north: %+v", loop.Legs[0])
	}

	dest := identAt(ap, 50, 12)
	oneway, err := generateTour(ap, nil, nil, TourRequest{From: start, Legs: 3, HoursPerLeg: 1.5, TasKt: 100, Mode: "oneway", To: dest}, now)
	if err != nil {
		t.Fatal(err)
	}
	if oneway.Legs[2].To != dest {
		t.Fatalf("one way ends at the destination: %+v", oneway.Legs)
	}
	checkLegs(t, oneway, maxLeg)

	explore, err := generateTour(ap, nil, nil, TourRequest{From: start, Legs: 3, HoursPerLeg: 1.5, TasKt: 100, Mode: "explore", Direction: 270}, now)
	if err != nil {
		t.Fatal(err)
	}
	if explore.Legs[2].Lon >= 7 {
		t.Fatalf("exploring west should end up west: %+v", explore.Legs)
	}

	if _, err := generateTour(ap, nil, nil, TourRequest{From: start, Legs: 1, HoursPerLeg: 1.5, TasKt: 100, Mode: "oneway", To: identAt(ap, 53, 12)}, now); err == nil {
		t.Fatal("expected 'too far for one leg'")
	}
	// Re-roll from leg 2: the first leg stays.
	again, err := generateTour(ap, nil, nil, TourRequest{From: start, Legs: 5, HoursPerLeg: 1.5, TasKt: 100, Mode: "loop", Seed: 7, Keep: loop.Legs[:1]}, now)
	if err != nil || again.Legs[0].To != loop.Legs[0].To || again.Legs[4].To != start {
		t.Fatalf("re-roll lost the kept leg: %v %+v", err, again.Legs)
	}
}

func TestTourProgressAndCode(t *testing.T) {
	tour := Tour{Name: "Test", Created: 100000, Legs: []TourLeg{
		{From: "EAAA", To: "EBBB", DistanceNm: 40}, {From: "EBBB", To: "ECCC", DistanceNm: 50}, {From: "ECCC", To: "EAAA", DistanceNm: 45, Note: "secret"},
	}}
	entries := []LogbookEntry{
		{ID: "old", Start: 10, Departure: "EAAA", Arrival: "EBBB"}, // before the tour
		{ID: "f1", Start: 102000, Departure: "EAAA", Arrival: "EBBB", AirborneMin: 30},
		{ID: "f2", Start: 103000, Departure: "EBBB", Arrival: "ECCC", AirborneMin: 35},
	}
	applyProgress(&tour, entries)
	p := tour.Progress
	if p.Done != 2 || p.Next != 2 || p.FlownNm != 90 || p.TotalNm != 135 || p.Finished || tour.Legs[0].FlightID != "f1" {
		t.Fatalf("unexpected progress %+v / %+v", p, tour.Legs)
	}
	tour.Legs[2].ManualDone = true
	applyProgress(&tour, entries)
	if !tour.Progress.Finished {
		t.Fatal("ticked by hand should finish it")
	}

	code, err := tourCode(tour)
	if err != nil || !strings.HasPrefix(code, tourCodeMagic) {
		t.Fatalf("code: %v %q", err, code)
	}
	back, err := parseTourCode(" "+code[:20]+"\n"+code[20:]+" ", time.Unix(5000, 0))
	if err != nil {
		t.Fatal(err)
	}
	if back.Name != "Test" || len(back.Legs) != 3 || back.Legs[2].Note != "" || back.Legs[2].ManualDone || back.Created != 5000 {
		t.Fatalf("unexpected imported tour %+v", back)
	}
	if _, err := parseTourCode("hello", time.Now()); err == nil {
		t.Fatal("expected an error for a non-code")
	}
}

func TestTourStorage(t *testing.T) {
	useTempConfigDir(t)
	tour := Tour{Name: "Stored", Created: 1, Legs: []TourLeg{{From: "EAAA", To: "EBBB", Done: true, FlightID: "x", Note: "nice"}}}
	if err := saveTour(&tour); err != nil || tour.ID == "" {
		t.Fatalf("save: %v %q", err, tour.ID)
	}
	list, err := listTours()
	if err != nil || len(list) != 1 || list[0].Legs[0].Done || list[0].Legs[0].Note != "nice" {
		t.Fatalf("list: %v %+v", err, list)
	}
	if err := deleteTour(tour.ID); err != nil {
		t.Fatal(err)
	}
	if err := deleteTour("../x"); err == nil {
		t.Fatal("expected an invalid id error")
	}
}
