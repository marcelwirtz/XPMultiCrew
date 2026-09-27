package main

import (
	"math"
	"os"
	"strings"
	"testing"
)

const sampleAirportBlock = `1   400 0 0 XTST Test Field
1302 datum_lat 50.000000
1302 datum_lon 8.000000
100 30.00 1 0 0.25 1 1 1 09  50.00000000  8.00000000  100.00  0.00 3 10 1 1 27  50.00000000  8.02800000    0.00  0.00 3 10 1 1
110 1 0.25 0.00 Apron
111 49.99900000 8.00500000
112 49.99900000 8.01000000 49.99850000 8.01200000
111 49.99800000 8.01000000
113 49.99800000 8.00500000
1201 49.99950000 8.00600000 both 0 A_start
1201 49.99950000 8.01000000 both 1 A_end
1202 0 1 twoway taxiway A
1202 0 1 twoway runway 09/27
1300 49.99850000 8.00700000 90.0 tie_down props Stand 1
19 50.00030000 8.00100000 1 WS
14 49.99700000 8.01400000 30 0 Tower Viewpoint
21 50.00030000 8.00300000 2 90.00 3.50 09 PAPI-4L
50 12345 ATIS
1054 118005 Test Tower
1   500 0 0 XNXT Next Airport
100 30.00 1 0 0.25 1 1 1 09  51.0 9.0 0 0 0 0 0 0 27  51.0 9.1 0 0 0 0 0 0
`

func TestParseAirportBlock(t *testing.T) {
	lay, err := parseAirportBlock(strings.NewReader(sampleAirportBlock), 50, 8)
	if err != nil {
		t.Fatal(err)
	}
	if lay.Ident != "XTST" || lay.Name != "Test Field" || lay.ElevationFt != 400 {
		t.Fatalf("header wrong: %+v", lay)
	}
	if len(lay.Runways) != 1 {
		t.Fatalf("the next airport's runway must not be included: %+v", lay.Runways)
	}
	rw := lay.Runways[0]
	if rw.Surface != "Asphalt" || rw.WidthM != 30 || math.Abs(rw.LengthM-2002) > 5 ||
		math.Abs(rw.Ends[0].HeadingTrue-90) > 0.2 || math.Abs(rw.Ends[1].HeadingTrue-270) > 0.2 ||
		rw.Ends[0].DisplacedM != 100 || rw.Ends[0].PapiDeg != 3.5 || rw.Ends[1].PapiDeg != 0 {
		t.Fatalf("runway wrong: %+v", rw)
	}
	if len(lay.Pavement) != 1 || len(lay.Pavement[0]) != 1 || len(lay.Pavement[0][0]) < 10 {
		t.Fatalf("pavement ring with bezier expected: %+v", lay.Pavement)
	}
	if len(lay.Taxiways) != 1 || lay.Taxiways[0].Name != "A" {
		t.Fatalf("taxiways (without runway edges) wrong: %+v", lay.Taxiways)
	}
	if len(lay.Parking) != 1 || lay.Parking[0].Name != "Stand 1" || lay.Parking[0].Type != "tie_down" {
		t.Fatalf("parking wrong: %+v", lay.Parking)
	}
	if len(lay.Windsocks) != 1 || lay.Tower == nil {
		t.Fatalf("windsock/tower missing")
	}
	if len(lay.Frequencies) != 2 || lay.Frequencies[0].MHz != "123.45" || lay.Frequencies[1].Type != "TWR" {
		t.Fatalf("frequencies wrong: %+v", lay.Frequencies)
	}
}

func TestBezierRingPlainAndCurved(t *testing.T) {
	plain := bezierRing([]pathNode{{p: [2]float64{0, 0}}, {p: [2]float64{10, 0}}, {p: [2]float64{10, 10}}}, true)
	if len(plain) != 3 {
		t.Fatalf("plain ring keeps its corners only: %v", plain)
	}
	curved := bezierRing([]pathNode{{p: [2]float64{0, 0}, c: [2]float64{5, 5}, hasC: true}, {p: [2]float64{10, 0}}}, false)
	if len(curved) != 9 || curved[4][1] <= 0 {
		t.Fatalf("curve should bulge towards the control point: %v", curved)
	}
}

const sampleApproachNav = `I
1200 Version
 4  51.524888889    7.630886111      462    10915    18  21300.522 IDWE EDLW ED 06 ILS-cat-II
 6  51.518569444    7.609402778      462    10915    18 300060.522 IDWE EDLW ED 06 GS
14  51.522198194    7.623229722      417    94670   0.0     60.500  R06 EDLW ED 06 LPV
16  51.516016250    7.605664861      401    94670  50.0 300060.500  R06 EDLW ED 06 E06A
12  51.521727778    7.618311111      397    11130    25      0.000 IDWW EDLW ED DORTMUND DME-ILS
99
`

func TestParseApproaches(t *testing.T) {
	got := parseApproaches(strings.NewReader(sampleApproachNav))["EDLW"]
	if len(got) != 2 {
		t.Fatalf("expected ILS + LPV, got %+v", got)
	}
	ils := got[0]
	if ils.Kind != "ILS-cat-II" || ils.Freq != "109.15" || ils.CourseMag != 59 || ils.Course != 60.5 || ils.GlideDeg != 3 {
		t.Fatalf("ILS wrong: %+v", ils)
	}
	if got[1].Kind != "LPV" || got[1].GlideDeg != 3 || got[1].Freq != "" {
		t.Fatalf("LPV wrong: %+v", got[1])
	}
	lay := &AirportLayout{Approaches: got}
	if v := magVarAt(lay, nil); math.Abs(v-1.5) > 0.01 {
		t.Fatalf("magvar from the ILS: %v", v)
	}
}

// Runs against a real X-Plane installation when XPLANE_ROOT is set.
func TestLoadAirportLayoutReal(t *testing.T) {
	root := os.Getenv("XPLANE_ROOT")
	if root == "" {
		t.Skip("XPLANE_ROOT not set")
	}
	for _, ident := range []string{"EDLW", "EDDF", "EDFE", "EDLM", "LFMN"} {
		lay, err := loadAirportLayout(root, ident)
		if err != nil {
			t.Fatal(err)
		}
		t.Logf("%s %s: %d runways, %d pavement, %d taxi edges, %d parking, %d approaches, magvar %.1f, freqs %d",
			lay.Ident, lay.Name, len(lay.Runways), len(lay.Pavement), len(lay.Taxiways), len(lay.Parking), len(lay.Approaches), lay.MagVar, len(lay.Frequencies))
		for _, r := range lay.Runways {
			t.Logf("  %s/%s %.0fx%.0f %s papi %.1f/%.1f", r.Ends[0].Name, r.Ends[1].Name, r.LengthM, r.WidthM, r.Surface, r.Ends[0].PapiDeg, r.Ends[1].PapiDeg)
		}
		for _, a := range lay.Approaches {
			t.Logf("  %+v", a)
		}
	}
}

func TestSearchAndNearestAirports(t *testing.T) {
	list := [][]interface{}{
		{"EDDF", "Frankfurt am Main", 50.033333, 8.570556, 1},
		{"EDFE", "Frankfurt-Egelsbach", 49.960833, 8.6415, 1.0},
		{"EDFZ", "Mainz-Finthen", 49.9675, 8.1472, 1},
		{"EDDH", "Hamburg", 53.63, 9.99, 1},
		{"XHEL", "Frankfurt Heliport", 50.0, 8.6, 17},
	}
	got := searchAirports(list, "edf", 10)
	if len(got) != 2 || got[0].Ident != "EDFE" || got[1].Ident != "EDFZ" {
		t.Fatalf("prefix search: %+v", got)
	}
	got = searchAirports(list, "frankfurt", 10)
	if len(got) != 3 {
		t.Fatalf("name search: %+v", got)
	}
	if got := searchAirports(list, "EDDF", 10); got[0].Ident != "EDDF" {
		t.Fatalf("exact match first: %+v", got)
	}
	near := nearestAirports(list, 50.0, 8.6, 60, 5)
	if len(near) != 3 || near[0].Ident != "EDDF" || near[len(near)-1].Ident != "EDFZ" {
		t.Fatalf("nearest: %+v", near)
	}
}
