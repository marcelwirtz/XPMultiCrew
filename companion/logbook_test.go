package main

import (
	"strings"
	"testing"
	"time"
)

func TestIsNight(t *testing.T) {
	day := time.Date(2026, 10, 4, 12, 0, 0, 0, time.UTC)
	if isNight(50, 8.6, day) {
		t.Fatal("noon is not night")
	}
	if !isNight(50, 8.6, time.Date(2026, 10, 4, 18, 30, 0, 0, time.UTC)) {
		t.Fatal("18:30Z in October at Frankfurt is night")
	}
	if !isNight(50, 8.6, time.Date(2026, 10, 4, 4, 0, 0, 0, time.UTC)) {
		t.Fatal("04:00Z is before sunrise")
	}
}

func TestLogbookEntryFromFlight(t *testing.T) {
	f := &Flight{ID: "f1", Start: 1000, Departure: "EAAA", Arrival: "EBBB",
		Track:    [][9]float64{{0, 50, 8, 300, 0, 0, 0, 0, 1}, {60, 50, 8.1, 2000, 100, 100, 0, 90, 0}, {120, 50, 8.2, 300, 60, 60, 0, 90, 1}},
		Peers:    []PeerTrack{{ID: 7, Callsign: "BUDDY", Track: [][4]float64{{0, 50, 8, 300}}}, {ID: 8, Callsign: "", Track: nil}},
		Landings: []Landing{{Own: true, Time: time.Date(2026, 10, 4, 19, 0, 0, 0, time.UTC).Unix(), Lat: 50, Lon: 8.2, Score: 92}},
	}
	e := logbookEntry(f)
	if len(e.Buddies) != 1 || e.Buddies[0] != "BUDDY" || !e.Night || e.BestScore != 92 || len(e.Track) != 3 {
		t.Fatalf("unexpected entry %+v", e)
	}
}

func TestBuildLogbook(t *testing.T) {
	ap := testAirports() // EBBB is 3000 ft high
	day := int64(1790000000)
	entries := []LogbookEntry{
		{ID: "a", Start: day, Departure: "EAAA", Arrival: "EAAA", AirborneMin: 40, DistanceNm: 30, MaxAltFt: 3000, BestScore: 70, Buddies: []string{}},
		{ID: "b", Start: day + 86400, Departure: "EAAA", Arrival: "EBBB", AirborneMin: 50, DistanceNm: 60, MaxAltFt: 6500, BestScore: 95, Buddies: []string{"BUDDY"}, Night: true},
		{ID: "c", Start: day + 2*86400, Departure: "EBBB", Arrival: "EAAA", AirborneMin: 45, DistanceNm: 61, MaxAltFt: 4500, BestScore: 60, Buddies: []string{"BUDDY", "OTHER"}},
	}
	lb := buildLogbook(entries, ap)
	if lb.Entries[0].ID != "c" || lb.Totals.Flights != 3 || lb.Totals.AirborneMin != 135 || lb.Totals.FlightsTogether != 2 ||
		lb.Totals.MinTogether != 95 || lb.Totals.Airports != 2 || lb.Totals.BestScore != 95 || lb.Totals.NightLandings != 1 {
		t.Fatalf("unexpected totals %+v", lb.Totals)
	}
	if len(lb.Buddies) != 2 || lb.Buddies[0].Callsign != "BUDDY" || lb.Buddies[0].Flights != 2 || lb.Buddies[0].Minutes != 95 {
		t.Fatalf("unexpected buddies %+v", lb.Buddies)
	}
	titles := []string{}
	for _, m := range lb.Milestones {
		titles = append(titles, m.Title)
	}
	all := strings.Join(titles, "|")
	for _, want := range []string{"First recorded flight", "First flight together", "First mountain airfield", "First night landing",
		"First butter landing", "Longest flight so far", "Highest flight so far", "First hour in the air"} {
		if !strings.Contains(all, want) {
			t.Errorf("missing milestone %q in %s", want, all)
		}
	}
	if lb.Milestones[0].FlightID != "c" {
		t.Fatalf("milestones should be newest first: %+v", lb.Milestones)
	}
}
