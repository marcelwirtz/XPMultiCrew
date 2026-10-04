package main

import (
	"errors"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestParseMetar(t *testing.T) {
	for _, c := range []struct {
		raw           string
		dir, kt, gust int
		vis, ceiling  int
		category      string
	}{
		{"METAR EDDF 041350Z AUTO VRB01KT CAVOK 21/11 Q1027 NOSIG", -1, 1, 0, 9999, 0, "VFR"},
		{"EDWE 041350Z 25015G27KT 9999 FEW012 BKN025 18/08 Q1028", 250, 15, 27, 9999, 2500, "MVFR"},
		{"EHGG 041355Z 28006KT 3000 BR OVC008 12/11 Q1020 TEMPO 0800 FG", 280, 6, 0, 3000, 800, "IFR"},
		{"KSFO 041356Z 30010KT 1 1/2SM BR VV003 12/11 A3001 RMK AO2", 300, 10, 0, 2413, 300, "LIFR"},
		{"UUEE 041400Z 18005MPS 9999 SCT030 10/05 Q1012", 180, 10, 0, 9999, 0, "VFR"},
	} {
		s := stationFromMetar(c.raw)
		if s.WindDir != c.dir || s.WindKt != c.kt || s.GustKt != c.gust || s.VisM != c.vis || s.CeilingFt != c.ceiling || s.Category != c.category {
			t.Errorf("%s: got %+v", c.raw, s)
		}
	}
}

func TestTafWarning(t *testing.T) {
	now := time.Date(2026, 10, 4, 12, 0, 0, 0, time.UTC)
	taf := "TAF EDDF 041100Z 0412/0518 06004KT CAVOK PROB40 TEMPO 0504/0508 0500 FG OVC002 BECMG 0507/0509 19005KT"
	if w := tafWarning(taf, now, now.Add(4*time.Hour)); w != "" {
		t.Fatalf("fog tomorrow morning is outside the next 4 h, got %q", w)
	}
	if w := tafWarning(taf, now.Add(16*time.Hour), now.Add(20*time.Hour)); w != "PROB40 TEMPO 0504/0508 0500 FG OVC002 (LIFR)" {
		t.Fatalf("unexpected warning %q", w)
	}
	fm := "TAF KJFK 041130Z 0412/0518 18010KT P6SM SCT050 FM041600 20012KT 3SM BR OVC009"
	if w := tafWarning(fm, now, now.Add(5*time.Hour)); !strings.Contains(w, "FM041600") || !strings.HasSuffix(w, "(IFR)") {
		t.Fatalf("unexpected FM warning %q", w)
	}
}

func TestWindProfileInterpolation(t *testing.T) {
	p := windProfile{heightsFt: []float64{300, 2500, 5000}, dirs: []float64{270, 270, 360}, speeds: []float64{5, 15, 20}}
	if d, s, _ := p.at(1400); math.Abs(d-270) > 0.5 || math.Abs(s-10) > 0.1 {
		t.Fatalf("mid-layer wind %v/%v", d, s)
	}
	if d, s, _ := p.at(9000); d != 0 && math.Abs(d-360) > 0.5 || math.Abs(s-20) > 0.1 {
		t.Fatalf("above the top %v/%v", d, s)
	}
}

func TestRunwayWind(t *testing.T) {
	rw := []RunwayGeometry{{Airport: "EAAA", Ends: [2]RunwayEnd{{Name: "09", Lat: 50, Lon: 8}, {Name: "27", Lat: 50, Lon: 8.05}}}}
	got, ok := runwayWind("EAAA", rw, WxStation{Ident: "EAAA", WindDir: 240, WindKt: 20})
	if !ok || got.Runway != "27" || got.HeadKt != 17 || got.CrossKt != 10 {
		t.Fatalf("unexpected runway wind %+v", got)
	}
}

func stubFetch(t *testing.T, answers map[string]string) {
	old := fetchURL
	t.Cleanup(func() { fetchURL = old })
	wxCacheMu.Lock()
	wxCache = map[string]wxCacheEntry{}
	wxCacheMu.Unlock()
	fetchURL = func(u string) ([]byte, error) {
		for prefix, body := range answers {
			if strings.HasPrefix(u, prefix) {
				return []byte(body), nil
			}
		}
		return nil, errors.New("offline")
	}
}

func TestRouteBriefing(t *testing.T) {
	stubFetch(t, map[string]string{
		aviationWxBase + "/metar": `[{"icaoId":"EBBB","rawOb":"METAR EBBB 041350Z 27012KT 9999 BKN012 15/10 Q1020","lat":50.2,"lon":9.0,"elev":100,"name":"East"},
			{"icaoId":"EFAR","rawOb":"METAR EFAR 041350Z 27012KT CAVOK 15/10 Q1020","lat":45.0,"lon":9.0,"elev":100,"name":"Far away"}]`,
		aviationWxBase + "/taf": `[]`,
		openMeteoBase: `{"hourly":{"freezing_level_height":[1200],"geopotential_height_1000hPa":[100],"wind_direction_1000hPa":[270],"wind_speed_1000hPa":[10],
			"geopotential_height_925hPa":[760],"wind_direction_925hPa":[270],"wind_speed_925hPa":[20],
			"geopotential_height_850hPa":[1500],"wind_direction_850hPa":[280],"wind_speed_850hPa":[30]}}`,
	})
	r := PlannedRoute{CruiseFt: 3500, Waypoints: []RouteWaypoint{{Kind: "APT", Ident: "EAAA", Lat: 50.2, Lon: 8.0}, {Kind: "APT", Ident: "EBBB", Lat: 50.2, Lon: 9.0}}}
	airports := testAirports()
	airports.RunwayGeometry = []RunwayGeometry{{Airport: "EBBB", Ends: [2]RunwayEnd{{Name: "08", Lat: 50.2, Lon: 8.98}, {Name: "26", Lat: 50.21, Lon: 9.02}}}}
	b, err := routeBriefing(t.TempDir(), airports, r)
	if err != nil {
		t.Fatal(err)
	}
	if len(b.Stations) != 1 || b.Stations[0].Ident != "EBBB" || b.Stations[0].ElevFt != 328 {
		t.Fatalf("expected only EBBB near the route: %+v", b.Stations)
	}
	if b.LegWinds[1] == nil || b.LegWinds[1].AltFt != 3500 || b.LegWinds[1].DirDeg != 275 || b.LegWinds[1].SpeedKt != 24 {
		t.Fatalf("unexpected leg wind %+v", b.LegWinds[1])
	}
	all := strings.Join(b.Warnings, "\n")
	for _, want := range []string{"EBBB: cloud base ~1528 ft MSL", "Freezing level ~3900 ft"} {
		if !strings.Contains(all, want) {
			t.Errorf("missing warning %q in %v", want, b.Warnings)
		}
	}
	if len(b.RunwayWinds) != 1 || b.RunwayWinds[0].Runway != "26" {
		t.Fatalf("unexpected runway winds %+v", b.RunwayWinds)
	}
}

func TestRouteBriefingOfflineFallback(t *testing.T) {
	stubFetch(t, map[string]string{})
	root := t.TempDir()
	dir := filepath.Join(root, "Output", "real weather")
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(dir, "metar-2026-10-02-12.15.txt"), []byte("2026/10/02 11:50\nEBBB 021150Z 27005KT 2000 BR OVC004 10/09 Q1015\n"), 0644)
	_ = os.WriteFile(filepath.Join(dir, "metar-2026-10-01-12.15.txt"), []byte("2026/10/01 11:50\nEBBB 011150Z 27005KT CAVOK 10/09 Q1015\n"), 0644)
	r := PlannedRoute{CruiseFt: 3500, Waypoints: []RouteWaypoint{{Kind: "APT", Ident: "EAAA", Lat: 50.2, Lon: 8.0}, {Kind: "APT", Ident: "EBBB", Lat: 50.2, Lon: 9.0}}}
	b, err := routeBriefing(root, testAirports(), r)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(b.Source, "2026-10-02-12.15") || len(b.Stations) != 1 || b.Stations[0].Category != "LIFR" {
		t.Fatalf("expected the newest X-Plane METAR: %+v", b)
	}
}
