package main

import (
	"math"
	"strings"
	"testing"
	"time"
)

func TestSunset(t *testing.T) {
	// Frankfurt, 4 Oct 2026: sunset about 16:58 UTC.
	got, ok := sunsetUTC(50.05, 8.6, time.Date(2026, 10, 4, 12, 0, 0, 0, time.UTC))
	want := time.Date(2026, 10, 4, 16, 58, 0, 0, time.UTC)
	if !ok || math.Abs(got.Sub(want).Minutes()) > 6 {
		t.Fatalf("sunset %v, want ~%v", got, want)
	}
	if _, ok := sunsetUTC(80, 0, time.Date(2026, 6, 21, 12, 0, 0, 0, time.UTC)); ok {
		t.Fatal("expected midnight sun at 80 N in June")
	}
}

func destTestAirports() AirportData {
	ap := AirportData{
		Airports: [][]interface{}{
			{"EHOM", "Home", 50.0, 8.0, 1.0},
			{"ENEA", "Near east", 50.0, 9.0, 1.0},     // 38.6 NM
			{"ENOR", "North", 50.8, 8.0, 1.0},         // 48 NM
			{"ESCN", "Scenery south", 49.2, 8.0, 1.0}, // 48 NM
			{"EWST", "West", 50.0, 6.9, 1.0},          // 42 NM
			{"ETOO", "Too far", 52.0, 8.0, 1.0},
			{"ESHT", "Short rwy", 50.6, 8.6, 1.0},
			{"EIFR", "Foggy", 50.4, 7.4, 1.0},
			{"XUL1", "Ultralight", 50.0, 8.9, 1.0},
		},
		Details: map[string]AirportDetail{},
	}
	for _, a := range ap.Airports {
		id := a[0].(string)
		lat, lon := a[2].(float64), a[3].(float64)
		length := 0.012 // ~800 m north-south
		if id == "ESHT" {
			length = 0.003
		}
		ap.RunwayGeometry = append(ap.RunwayGeometry, RunwayGeometry{Airport: id, Ends: [2]RunwayEnd{{Name: "18", Lat: lat + length/2, Lon: lon}, {Name: "36", Lat: lat - length/2, Lon: lon}}})
	}
	return ap
}

func TestSuggestDestinations(t *testing.T) {
	now := time.Date(2026, 10, 4, 12, 0, 0, 0, time.UTC)
	stations := []WxStation{{Ident: "EIFR", Lat: 50.4, Lon: 7.4, Category: "IFR", Metar: "EIFR 041200Z 00000KT 0800 FG OVC002"}}
	req := DestinationRequest{From: "EHOM", Hours: 1.0, TasKt: 100, MinRunwayM: 500}
	res, err := suggestDestinations(destTestAirports(), stations, map[string]bool{"ENEA": true}, map[string]bool{"ESCN": true}, req, now)
	if err != nil {
		t.Fatal(err)
	}
	ids := []string{}
	for _, i := range res.Ideas {
		ids = append(ids, i.Ident)
	}
	got := strings.Join(ids, " ")
	if len(res.Ideas) != 3 || res.Ideas[0].Ident != "ESCN" {
		t.Fatalf("expected 3 ideas with the custom-scenery one first, got %s (%v)", got, res.Notes)
	}
	for _, bad := range []string{"ETOO", "ESHT", "EIFR", "XUL1", "EHOM"} {
		if strings.Contains(got, bad) {
			t.Fatalf("%s should not be suggested: %s", bad, got)
		}
	}
	if !strings.Contains(strings.Join(res.Ideas[0].Tags, ","), "Custom scenery installed") {
		t.Fatalf("missing tag: %+v", res.Ideas[0])
	}
	// A round trip: 1.5 h = 2 x (30 min airborne + 15 min ground) -> 43 NM.
	req.RoundTrip = true
	req.Hours = 1.5
	res, _ = suggestDestinations(destTestAirports(), nil, nil, nil, req, now)
	for _, i := range res.Ideas {
		if i.DistanceNm > 43.5 {
			t.Fatalf("too far for a 1.5 h round trip: %+v", i)
		}
	}
	// Late in the day: arriving after sunset is flagged.
	late := time.Date(2026, 10, 4, 16, 40, 0, 0, time.UTC)
	res, _ = suggestDestinations(destTestAirports(), nil, nil, nil, DestinationRequest{From: "EHOM", Hours: 1.0, TasKt: 100, MinRunwayM: 500}, late)
	if len(res.Ideas) == 0 || !strings.Contains(strings.Join(res.Ideas[0].Tags, ","), "after sunset") {
		t.Fatalf("expected a sunset tag: %+v", res.Ideas)
	}
}
