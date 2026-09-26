package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const sampleFix = `I
1200 Version - data cycle 2406, build 20251002, metadata FixXP1200.

 53.760000000   14.230000000  VP319 EDAH ED 86 ECHO
 51.116666667   13.564722222  VP177 EDDC ED 86 CHARLIE 1
 49.951722222    8.340191667  ADEVO EDDF ED 4464727 ADEVO
 54.471666667    6.363055556  HSEEN ENRT ED 86 HSEEN
99
`

const sampleNav = `I
1200 Version - data cycle 2406

 3  50.053741667    8.637091667      491    11420   130      2.000  FFM ENRT ED FRANKFURT VORTAC
12  50.053741667    8.637091667      491    11420   130      0.000  FFM ENRT ED FRANKFURT VORTAC DME
 2  50.000000000    8.000000000        0      401    50      0.000  XYZ ENRT ED SOMEWHERE NDB
99
`

// A 0.2 x 0.2 degree square CTR (GND-2500 ft) and a class C box above it.
const sampleAir = "AC CTR\r\nAN TEST CTR\r\nAL GND\r\nAH 2500 MSL\r\n" +
	"DP 50:00:00 N 008:00:00 E\r\nDP 50:12:00 N 008:00:00 E\r\nDP 50:12:00 N 008:12:00 E\r\nDP 50:00:00 N 008:12:00 E\r\n\r\n" +
	"AC C\r\nAN TEST C\r\nAL 3500 MSL\r\nAH FL100\r\n" +
	"DP 50:00:00 N 008:00:00 E\r\nDP 50:12:00 N 008:00:00 E\r\nDP 50:12:00 N 008:12:00 E\r\nDP 50:00:00 N 008:12:00 E\r\n"

func TestParseNavData(t *testing.T) {
	vrps := parseVfrPoints(strings.NewReader(sampleFix))
	// 86 = 'V' (0x56) + two spaces? - only the low byte matters: 86 is 'V'.
	if len(vrps) != 3 {
		t.Fatalf("expected 3 VFR points (not the RNAV fix), got %+v", vrps)
	}
	if vrps[1].Name != "CHARLIE 1" || vrps[1].Airport != "EDDC" || vrps[2].Airport != "" {
		t.Fatalf("unexpected VRP parse: %+v", vrps)
	}
	nav := parseNavaids(strings.NewReader(sampleNav))
	if len(nav) != 2 || nav[0].Kind != "VOR" || nav[0].Freq != "114.20" || nav[0].MagVar != 2 ||
		nav[0].Name != "FRANKFURT VORTAC" || nav[1].Kind != "NDB" || nav[1].Freq != "401" {
		t.Fatalf("unexpected navaids: %+v", nav)
	}
	air := parseAirspaces(strings.NewReader(sampleAir))
	if len(air) != 2 || air[0].Class != "CTR" || !air[0].LowerGnd || air[0].UpperFt != 2500 ||
		air[1].LowerFt != 3500 || air[1].UpperFt != 10000 || len(air[0].Poly) != 4 {
		t.Fatalf("unexpected airspaces: %+v", air)
	}
	if in := airspacesInBox(air, 8.1, 50.1, 8.15, 50.15, 10); len(in) != 2 {
		t.Fatalf("box query failed: %d", len(in))
	}
	if in := airspacesInBox(air, 9, 51, 10, 52, 10); len(in) != 0 {
		t.Fatalf("box query should be empty, got %d", len(in))
	}
}

func TestAirspaceAlert(t *testing.T) {
	air := parseAirspaces(strings.NewReader(sampleAir))

	// Inside the CTR at 1500 ft.
	a := airspaceAlert(air, 50.1, 8.1, 1500, 90, 100, 120)
	if len(a.Inside) != 1 || a.Inside[0].Name != "TEST CTR" || a.Inside[0].Lower != "GND" || a.Ahead != nil {
		t.Fatalf("expected to be inside the CTR only: %+v", a)
	}
	// West of it at 2000 ft heading east at 120 kt: 0.1 deg lon ~ 3.9 NM at
	// 50N -> about 2 min away.
	a = airspaceAlert(air, 50.1, 7.95, 2000, 90, 120, 120)
	if len(a.Inside) != 0 || a.Ahead == nil || a.Ahead.Name != "TEST CTR" || a.Ahead.EtaSecs > 120 {
		t.Fatalf("expected the CTR ahead: %+v", a)
	}
	// Between CTR top and class C bottom (3000 ft): nothing.
	a = airspaceAlert(air, 50.1, 8.1, 3000, 90, 100, 120)
	if len(a.Inside) != 0 || a.Ahead != nil {
		t.Fatalf("expected nothing at 3000 ft: %+v", a)
	}
	// Parked: no look-ahead.
	if a = airspaceAlert(air, 50.1, 7.99, 2000, 90, 5, 120); a.Ahead != nil {
		t.Fatalf("no look-ahead when slow: %+v", a)
	}
}

func TestRouteEncodeDecodeAndFms(t *testing.T) {
	r := PlannedRoute{Name: "Test Tour", CruiseFt: 3500, TasKt: 110, Waypoints: []RouteWaypoint{
		{Kind: "APT", Ident: "EDDF", Name: "Frankfurt, Main", Lat: 50.0333, Lon: 8.5705},
		{Kind: "VRP", Ident: "VP177", Name: "CHARLIE 1", Lat: 51.1167, Lon: 13.5647},
		{Kind: "USR", Ident: "", Name: "", Lat: 50.5, Lon: 9.25},
		{Kind: "VOR", Ident: "FFM", Name: "FRANKFURT", Lat: 50.0537, Lon: 8.6371},
		{Kind: "APT", Ident: "EDFE", Name: "Egelsbach", Lat: 49.9608, Lon: 8.6436},
	}}
	payload, err := encodeRoute(r)
	if err != nil {
		t.Fatal(err)
	}
	if strings.ContainsAny(payload, " \n") {
		t.Fatalf("payload must be a single token: %q", payload)
	}
	back, ok := decodeRoute(payload)
	if !ok || len(back.Waypoints) != 5 || back.Name != "Test Tour" || back.CruiseFt != 3500 ||
		back.Waypoints[0].Name != "Frankfurt Main" || back.Waypoints[1].Name != "CHARLIE 1" {
		t.Fatalf("round trip failed: %+v", back)
	}
	if _, ok := decodeRoute("v1|x|0|0|APT,A,B,999,0;APT,C,D,1,1"); ok {
		t.Fatal("a route with only one valid waypoint must be rejected")
	}

	fms, err := fmsRoute(r, "2406")
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"CYCLE 2406\n", "ADEP EDDF\n", "ADES EDFE\n", "NUMENR 5\n",
		"1 EDDF ADEP 0.000000 50.033300 8.570500\n", "11 VP177 DRCT 3500.000000",
		"28 +50.500_+009.250 DRCT 3500.000000", "3 FFM DRCT", "1 EDFE ADES 0.000000"} {
		if !strings.Contains(fms, want) {
			t.Fatalf("fms missing %q:\n%s", want, fms)
		}
	}

	root := t.TempDir()
	dir := defaultDataDir(root)
	_ = os.MkdirAll(dir, 0755)
	_ = os.WriteFile(filepath.Join(dir, "earth_fix.dat"), []byte(sampleFix), 0644)
	name, err := exportFms(root, r)
	if err != nil || name != "Test_Tour.fms" {
		t.Fatalf("export: %q %v", name, err)
	}
	if raw, _ := os.ReadFile(filepath.Join(root, "Output", "FMS plans", name)); !strings.Contains(string(raw), "CYCLE 2406") {
		t.Fatalf("exported file wrong:\n%s", raw)
	}
}

func TestParseSharedRoute(t *testing.T) {
	payload, _ := encodeRoute(PlannedRoute{Waypoints: []RouteWaypoint{{Kind: "APT", Ident: "EDDF", Lat: 50, Lon: 8}, {Kind: "APT", Ident: "EDFE", Lat: 49.9, Lon: 8.6}}})
	c := NewPluginClient()
	c.applyStatusMessage("ROUTE_SHARED 1111 " + payload + "\n")
	if r := c.SharedRoute(); r == nil || r.FromSenderID != 1111 || len(r.Route.Waypoints) != 2 {
		t.Fatalf("unexpected shared route: %+v", r)
	}
	c.applyStatusMessage("ROUTE_SHARED \n")
	if c.SharedRoute() != nil {
		t.Fatal("empty ROUTE_SHARED should clear it")
	}
}

func TestParseFms(t *testing.T) {
	v1100 := "I\r\n1100 Version\r\nCYCLE 1710\r\nADEP KCUB\r\nADES KRDU\r\nNUMENR 4\r\n" +
		"1 KCUB ADEP 0.000000 33.970470 -80.995247\r\n3 CTF DRCT 6000.000000 34.650497 -80.274918\r\n" +
		"28 +34.900_-080.000 DRCT 7000.000000 34.900000 -80.000000\r\n11 NOMOE V155 6500.000000 34.880920 -79.996437\r\n" +
		"1 KRDU ADES 435.000000 35.877640 -78.787476\r\n"
	r, err := parseFms(v1100, "test")
	if err != nil || len(r.Waypoints) != 5 || r.CruiseFt != 7000 || r.Waypoints[0].Kind != "APT" ||
		r.Waypoints[1].Kind != "VOR" || r.Waypoints[2].Kind != "USR" || r.Waypoints[3].Kind != "FIX" ||
		r.Waypoints[3].Ident != "NOMOE" {
		t.Fatalf("v1100 parse: %+v %v", r, err)
	}
	v3 := "I\n3 version\n1\n2\n1 EDDF 0 50.0333 8.5706\n2 XYZ 3000 50.1 8.7\n"
	if r, err := parseFms(v3, "old"); err != nil || len(r.Waypoints) != 2 || r.Waypoints[1].Kind != "NDB" {
		t.Fatalf("v3 parse: %+v %v", r, err)
	}
	if _, err := parseFms("hello", "x"); err == nil {
		t.Fatal("garbage should be rejected")
	}
	// Round trip through our own export.
	out, _ := fmsRoute(r, "2406")
	back, err := parseFms(out, "back")
	if err != nil || len(back.Waypoints) != len(r.Waypoints) || back.Waypoints[3].Kind != "FIX" {
		t.Fatalf("export/import round trip: %+v %v", back, err)
	}
}

func TestParseWind(t *testing.T) {
	w := parseWind("1000:270:10;5000:280:25;bad;9000:300:x")
	if len(w) != 2 || w[1] != (WindLayer{AltFt: 5000, FromDeg: 280, SpeedKt: 25}) {
		t.Fatalf("got %+v", w)
	}
}
