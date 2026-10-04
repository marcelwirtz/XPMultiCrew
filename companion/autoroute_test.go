package main

import (
	"errors"
	"fmt"
	"math"
	"os"
	"strings"
	"testing"
	"time"
)

// No network in tests: the terrain check (and weather) see "offline"
// unless a test stubs fetchURL itself. TestAutoRouteRealData uses the
// real one.
var realFetchURL = fetchURL

func init() {
	fetchURL = func(string) ([]byte, error) { return nil, errors.New("offline (test)") }
}

// A 0.4 x 0.4 degree box at 50-50.4 N, 8.3-8.7 E between two airports.
func boxAirspace(class, name string, lowerFt int, lowerGnd bool, upperFt int, minLat, minLon, maxLat, maxLon float64) Airspace {
	a := Airspace{Name: name, Class: class, LowerFt: lowerFt, LowerGnd: lowerGnd, UpperFt: upperFt,
		Poly: [][2]float64{{minLon, minLat}, {minLon, maxLat}, {maxLon, maxLat}, {maxLon, minLat}}}
	a.minLon, a.minLat, a.maxLon, a.maxLat = minLon, minLat, maxLon, maxLat
	return a
}

func testAirports() AirportData {
	return AirportData{
		Airports: [][]interface{}{
			{"EAAA", "West", 50.2, 8.0, 1.0},
			{"EBBB", "East", 50.2, 9.0, 1.0},
			{"ECCC", "In zone", 50.2, 10.0, 1.0},
		},
		Details: map[string]AirportDetail{"EAAA": {ElevationFt: 300}, "EBBB": {ElevationFt: 3000}},
	}
}

func routeLegsAvoid(t *testing.T, wps []RouteWaypoint, a Airspace) {
	t.Helper()
	proj := projection{lat0: 50, cos0: math.Cos(50 * math.Pi / 180)}
	z := routeZone{a: &a, minX: math.Inf(1), minY: math.Inf(1), maxX: math.Inf(-1), maxY: math.Inf(-1)}
	for _, p := range a.Poly {
		x, y := proj.xy(p[1], p[0])
		z.poly = append(z.poly, [2]float64{x, y})
		z.minX, z.maxX = math.Min(z.minX, x), math.Max(z.maxX, x)
		z.minY, z.maxY = math.Min(z.minY, y), math.Max(z.maxY, y)
	}
	for i := 1; i < len(wps); i++ {
		ax, ay := proj.xy(wps[i-1].Lat, wps[i-1].Lon)
		bx, by := proj.xy(wps[i].Lat, wps[i].Lon)
		if z.hitsSegment(ax, ay, bx, by) {
			t.Fatalf("leg %d crosses %s: %+v", i, a.Name, wps)
		}
	}
}

func TestAutoRouteDirectWhenClear(t *testing.T) {
	res, err := autoRoute("", testAirports(), &navCache{}, AutoRouteRequest{From: "eaaa", To: "EBBB", CruiseFt: 3500})
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Waypoints) != 2 || res.Waypoints[0].Ident != "EAAA" || res.Waypoints[1].Ident != "EBBB" {
		t.Fatalf("expected a direct route, got %+v", res.Waypoints)
	}
	if math.Abs(res.DistanceNm-res.DirectNm) > 0.01 || res.DirectNm < 37 || res.DirectNm > 40 {
		t.Fatalf("unexpected distance %.1f / %.1f", res.DistanceNm, res.DirectNm)
	}
	// EBBB is 3000 ft high - a 3500 ft cruise is too low there.
	if !strings.Contains(strings.Join(res.Notes, "\n"), "less than 1000 ft") {
		t.Fatalf("expected an altitude note, got %v", res.Notes)
	}
}

func TestAutoRouteAvoidsRestrictedArea(t *testing.T) {
	r := boxAirspace("R", "ED-R 1", 0, true, 6000, 50.0, 8.3, 50.4, 8.7)
	nav := &navCache{airspaces: []Airspace{r}}
	res, err := autoRoute("", testAirports(), nav, AutoRouteRequest{From: "EAAA", To: "EBBB", CruiseFt: 3500})
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Waypoints) < 3 {
		t.Fatalf("expected a detour, got %+v", res.Waypoints)
	}
	routeLegsAvoid(t, res.Waypoints, r)
	// Above the area it's direct again.
	res, err = autoRoute("", testAirports(), nav, AutoRouteRequest{From: "EAAA", To: "EBBB", CruiseFt: 7500})
	if err != nil || len(res.Waypoints) != 2 {
		t.Fatalf("expected direct above the area: %v %+v", err, res.Waypoints)
	}
}

func TestAutoRouteControlledAirspaceOptional(t *testing.T) {
	c := boxAirspace("C", "TEST C", 0, true, 10000, 50.0, 8.3, 50.4, 8.7)
	nav := &navCache{airspaces: []Airspace{c}}
	res, err := autoRoute("", testAirports(), nav, AutoRouteRequest{From: "EAAA", To: "EBBB", CruiseFt: 3500})
	if err != nil || len(res.Waypoints) != 2 {
		t.Fatalf("expected direct through controlled airspace: %v %+v", err, res.Waypoints)
	}
	if !strings.Contains(strings.Join(res.Notes, "\n"), "Clearance needed: TEST C (C)") {
		t.Fatalf("expected a clearance note, got %v", res.Notes)
	}
	res, err = autoRoute("", testAirports(), nav, AutoRouteRequest{From: "EAAA", To: "EBBB", CruiseFt: 3500, AvoidControlled: true})
	if err != nil {
		t.Fatal(err)
	}
	routeLegsAvoid(t, res.Waypoints, c)
}

func TestAutoRouteDucksUnderShelf(t *testing.T) {
	c := boxAirspace("C", "TEST C", 2500, false, 10000, 50.0, 8.3, 50.4, 8.7)
	nav := &navCache{airspaces: []Airspace{c}}
	airports := testAirports()
	airports.Details["EBBB"] = AirportDetail{ElevationFt: 200}
	res, err := autoRoute("", airports, nav, AutoRouteRequest{From: "EAAA", To: "EBBB", CruiseFt: 3500, AvoidControlled: true})
	if err != nil || len(res.Waypoints) != 2 || res.Waypoints[1].AltFt != 2300 {
		t.Fatalf("expected direct at 2300 ft under the class C: %v %+v", err, res.Waypoints)
	}
	if !strings.Contains(strings.Join(res.Notes, "\n"), "down to 2300 ft") {
		t.Fatalf("expected a lower-leg note, got %v", res.Notes)
	}
	// A floor too low to fly under (below 1000 ft en route): around it.
	c = boxAirspace("C", "LOW C", 1100, false, 10000, 50.0, 8.3, 50.4, 8.7)
	res, err = autoRoute("", airports, &navCache{airspaces: []Airspace{c}}, AutoRouteRequest{From: "EAAA", To: "EBBB", CruiseFt: 3500, AvoidControlled: true})
	if err != nil {
		t.Fatal(err)
	}
	routeLegsAvoid(t, res.Waypoints, c)
}

func TestHighestFree(t *testing.T) {
	for _, c := range []struct {
		lo, hi float64
		bands  [][2]float64
		want   float64
		ok     bool
	}{
		{1500, 3500, nil, 3500, true},
		{1500, 3500, [][2]float64{{2000, 10500}}, 2000, true},
		{1500, 3500, [][2]float64{{math.Inf(-1), 3000}}, 3500, true},
		{1500, 3500, [][2]float64{{math.Inf(-1), 4000}}, 0, false},
		{1500, 5500, [][2]float64{{3000, 6000}, {math.Inf(-1), 2500}}, 3000, true},
		{1500, 5500, [][2]float64{{2800, 6000}, {math.Inf(-1), 3000}}, 0, false},
	} {
		got, ok := highestFree(c.lo, c.hi, c.bands)
		if ok != c.ok || (ok && got != c.want) {
			t.Errorf("highestFree(%v, %v, %v) = %v, %v; want %v, %v", c.lo, c.hi, c.bands, got, ok, c.want, c.ok)
		}
	}
}

func TestAutoRouteEntersControlZoneViaReportingPoint(t *testing.T) {
	ctr := boxAirspace("CTR", "ECCC CTR", 0, true, 2500, 50.1, 9.85, 50.3, 10.15)
	nav := &navCache{
		airspaces: []Airspace{ctr},
		points: []NavPoint{
			{Kind: "VRP", Ident: "S", Name: "SIERRA", Airport: "ECCC", Lat: 50.1, Lon: 10.0},
			{Kind: "VRP", Ident: "W", Name: "WHISKEY", Airport: "ECCC", Lat: 50.2, Lon: 9.85},
		},
	}
	res, err := autoRoute("", testAirports(), nav, AutoRouteRequest{From: "EBBB", To: "ECCC", CruiseFt: 3500, AvoidControlled: true})
	if err != nil {
		t.Fatal(err)
	}
	n := len(res.Waypoints)
	if n < 3 || res.Waypoints[n-2].Ident != "W" {
		t.Fatalf("expected entry via WHISKEY, got %+v", res.Waypoints)
	}
	if !strings.Contains(strings.Join(res.Notes, "\n"), "ECCC CTR") {
		t.Fatalf("expected the destination CTR in the notes, got %v", res.Notes)
	}
}

func TestAutoRouteErrors(t *testing.T) {
	if _, err := autoRoute("", testAirports(), &navCache{}, AutoRouteRequest{From: "EAAA", To: "XXXX"}); err == nil {
		t.Fatal("expected an unknown-airport error")
	}
	if _, err := autoRoute("", testAirports(), &navCache{}, AutoRouteRequest{From: "EAAA", To: "EAAA"}); err == nil {
		t.Fatal("expected a same-airport error")
	}
}

// TestAutoRouteRealData runs against a real X-Plane install when
// XPLANE_ROOT is set (skipped otherwise).
func TestAutoRouteRealData(t *testing.T) {
	root := os.Getenv("XPLANE_ROOT")
	if root == "" {
		t.Skip("XPLANE_ROOT not set")
	}
	fetchURL = realFetchURL
	t.Cleanup(func() { fetchURL = func(string) ([]byte, error) { return nil, errors.New("offline (test)") } })
	airports, err := loadAirports(root)
	if err != nil {
		t.Fatal(err)
	}
	nav, err := loadNav(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, c := range []AutoRouteRequest{
		{From: "EDFE", To: "EDFM", CruiseFt: 3500, AvoidControlled: true},
		{From: "EDFE", To: "EDDS", CruiseFt: 3500, AvoidControlled: true},
		{From: "EDKB", To: "EDHL", CruiseFt: 4500, AvoidControlled: true},
		{From: "EDKB", To: "EDHL", CruiseFt: 4500},
		{From: "EDKB", To: "EDHL", CruiseFt: 4500, AvoidControlled: true, RadioNav: true},
		{From: "EDFE", To: "EDDS", CruiseFt: 3500, AvoidControlled: true, RadioNav: true},
		{From: "EDDH", To: "EDXF", CruiseFt: 2500, AvoidControlled: true},
		{From: "EDLM", To: "EDWR", CruiseFt: 2500, AvoidControlled: true},
		{From: "EDLM", To: "EDWR", CruiseFt: 3500, AvoidControlled: true, RadioNav: true},
	} {
		start := time.Now()
		res, err := autoRoute(root, airports, nav, c)
		if err != nil {
			t.Errorf("%+v: %v", c, err)
			continue
		}
		ids := []string{}
		for _, w := range res.Waypoints {
			id := w.Ident
			if w.Kind == "USR" {
				id = "usr[" + w.Name + "]"
			}
			if w.AltFt > 0 {
				id += fmt.Sprintf("@%d", w.AltFt)
			}
			ids = append(ids, id)
		}
		t.Logf("%s-%s %d ft avoid=%v radio=%v: %.0f NM (direct %.0f) in %v: %s\n  %s", c.From, c.To, c.CruiseFt, c.AvoidControlled, c.RadioNav,
			res.DistanceNm, res.DirectNm, time.Since(start).Round(time.Millisecond), strings.Join(ids, " "), strings.Join(res.Notes, "\n  "))
	}
}

func TestVorFixAndIdents(t *testing.T) {
	vors := []NavPoint{{Kind: "VOR", Ident: "ABC", Lat: 50, Lon: 8, MagVar: 2}, {Kind: "NDB", Ident: "N", Lat: 50, Lon: 8.01}}
	// 10 NM due east of the VOR: true 090, 2° east variation -> radial 088.
	if got := vorFix(vors, 50, 8+10.0/(60*math.Cos(50*math.Pi/180))); got != "ABC R088/10" {
		t.Fatalf("unexpected fix %q", got)
	}
	if got := vorFix(vors, 52, 8); got != "" {
		t.Fatalf("expected no fix 120 NM away, got %q", got)
	}
	for id, want := range map[string]bool{"EDFE": true, "XEDBX": false, "XEDC": false, "ED01": false, "EDF": false} {
		if isIcaoIdent(id) != want {
			t.Errorf("isIcaoIdent(%q) != %v", id, want)
		}
	}
}

func TestAutoRouteDangerAreasAreSoft(t *testing.T) {
	// Small danger area right on the direct line: cheap to go around.
	small := boxAirspace("Q", "SMALL GLIDER", 0, true, 6000, 50.15, 8.45, 50.25, 8.55)
	res, err := autoRoute("", testAirports(), &navCache{airspaces: []Airspace{small}}, AutoRouteRequest{From: "EAAA", To: "EBBB", CruiseFt: 3500})
	if err != nil {
		t.Fatal(err)
	}
	routeLegsAvoid(t, res.Waypoints, small)
	if strings.Contains(strings.Join(res.Notes, "\n"), "danger") {
		t.Fatalf("no danger-area note expected, got %v", res.Notes)
	}
	// A long one across the whole corridor: crossed, and said so.
	wall := boxAirspace("Q", "LONG PJE", 0, true, 15000, 48.0, 8.45, 52.0, 8.55)
	res, err = autoRoute("", testAirports(), &navCache{airspaces: []Airspace{wall}}, AutoRouteRequest{From: "EAAA", To: "EBBB", CruiseFt: 3500})
	if err != nil || len(res.Waypoints) != 2 {
		t.Fatalf("expected direct through the danger area: %v %+v", err, res.Waypoints)
	}
	if !strings.Contains(strings.Join(res.Notes, "\n"), "Crosses danger areas - check whether they're active (FIS/NOTAM): LONG PJE") {
		t.Fatalf("expected a danger-area note, got %v", res.Notes)
	}
	// The same as a restricted area: no way through.
	wall.Class = "R"
	if _, err := autoRoute("", testAirports(), &navCache{airspaces: []Airspace{wall}}, AutoRouteRequest{From: "EAAA", To: "EBBB", CruiseFt: 3500}); err == nil {
		t.Fatal("expected no route through a restricted area")
	}
}
