package main

import (
	"container/heap"
	"errors"
	"fmt"
	"math"
	"sort"
	"strings"
)

// Automatic VFR route planning: an A* search from one airport to another
// over the VFR reporting points, VORs, NDBs and airports X-Plane ships
// (navdata.go, airports.go), plus helper points just outside airspaces that
// are in the way. Every leg gets its own altitude: the highest one up to the
// planned cruise that keeps clear (200 ft below, 500 ft above) of
// prohibited/restricted areas (their activity times aren't in X-Plane's
// data, so they count as always active) and, if asked to, of controlled
// airspace (B/C/D/CTR) - so a route can duck under a class C shelf instead
// of going around it. Danger areas (gliding, parachuting...) may be crossed
// when going around them is a long detour; the notes then list them. An
// airport inside a control zone is entered and left via its own reporting
// points. Finally every leg is checked against the terrain (terrain.go) and
// raised to 1000 ft above it where the airspace allows.

// AutoRouteRequest is what the map's "Auto route" form sends.
type AutoRouteRequest struct {
	From            string `json:"from"`
	To              string `json:"to"`
	CruiseFt        int    `json:"cruiseFt"`
	AvoidControlled bool   `json:"avoidControlled"`
	RadioNav        bool   `json:"radioNav"` // prefer VORs/NDBs (flying without GPS)
}

// AutoRouteResult is the planned route (each waypoint's AltFt is the
// altitude for the leg to it) plus hints for the pilot.
type AutoRouteResult struct {
	Waypoints  []RouteWaypoint `json:"waypoints"`
	DistanceNm float64         `json:"distanceNm"`
	DirectNm   float64         `json:"directNm"`
	Notes      []string        `json:"notes"`
}

const (
	autoRouteMaxNodes   = 6000
	autoRouteClearNm    = 1.5  // helper points this far outside an airspace
	autoRouteFloorBufFt = 200  // kept below an airspace's lower limit...
	autoRouteCeilBufFt  = 500  // ...and above its upper limit
	autoRouteMinFt      = 1000 // lowest en-route altitude MSL...
	autoRouteMinAglFt   = 1000 // ...and at least this far above the airports
	autoRouteLowLegNm   = 25   // legs this short out of/into an airport may stay lower...
	autoRouteLowAglFt   = 700  // ...down to this far above it
	autoRouteFixVorNm   = 60   // helper points get a radial/DME from a VOR this close
	autoRouteDangerNm   = 15   // crossing a danger area costs like this much detour
)

// routeNode is a candidate waypoint in a local flat projection (x/y in NM).
type routeNode struct {
	wp       RouteWaypoint
	x, y     float64
	penalty  float64 // extra cost (NM) for using it, so routes stay simple
	terminal int     // 1 = departure, 2 = destination or one of their reporting points
	around   string  // helper points: the airspace they lead around
}

// routeZone is an airspace projected the same way.
type routeZone struct {
	a                      *Airspace
	poly                   [][2]float64
	minX, minY, maxX, maxY float64
	terminal               int // a control zone around the departure (1) / destination (2)
}

type projection struct{ lat0, cos0 float64 }

func newProjection(lat0 float64) projection {
	return projection{lat0: lat0, cos0: math.Max(0.1, math.Cos(lat0*math.Pi/180))}
}

func (p projection) xy(lat, lon float64) (float64, float64) {
	return lon * 60 * p.cos0, lat * 60
}

func (p projection) latLon(x, y float64) (float64, float64) {
	return y / 60, x / (60 * p.cos0)
}

func (p projection) zone(a *Airspace) *routeZone {
	z := &routeZone{a: a, minX: math.Inf(1), minY: math.Inf(1), maxX: math.Inf(-1), maxY: math.Inf(-1)}
	for _, pt := range a.Poly {
		x, y := p.xy(pt[1], pt[0])
		z.poly = append(z.poly, [2]float64{x, y})
		z.minX, z.maxX = math.Min(z.minX, x), math.Max(z.maxX, x)
		z.minY, z.maxY = math.Min(z.minY, y), math.Max(z.maxY, y)
	}
	return z
}

// greatCircleNm is the distance between two points in nautical miles.
func greatCircleNm(lat1, lon1, lat2, lon2 float64) float64 {
	const rad = math.Pi / 180
	dLat := (lat2 - lat1) * rad
	dLon := (lon2 - lon1) * rad
	h := math.Sin(dLat/2)*math.Sin(dLat/2) + math.Cos(lat1*rad)*math.Cos(lat2*rad)*math.Sin(dLon/2)*math.Sin(dLon/2)
	return 2 * 3440.065 * math.Asin(math.Min(1, math.Sqrt(h)))
}

func isControlledClass(c string) bool {
	return c == "B" || c == "C" || c == "D" || c == "CTR"
}

func isNoGoClass(c string) bool {
	return c == "P" || c == "R" || c == "Q" || c == "A"
}

// band is the altitude range a zone takes away, with the 500 ft buffer.
func (z *routeZone) band() (lo, hi float64) {
	lo = float64(z.a.LowerFt) - autoRouteFloorBufFt
	if z.a.LowerGnd {
		lo = math.Inf(-1)
	}
	return lo, float64(z.a.UpperFt) + autoRouteCeilBufFt
}

func pointInPolyXY(x, y float64, poly [][2]float64) bool {
	inside := false
	for i, j := 0, len(poly)-1; i < len(poly); j, i = i, i+1 {
		xi, yi := poly[i][0], poly[i][1]
		xj, yj := poly[j][0], poly[j][1]
		if (yi > y) != (yj > y) && x < (xj-xi)*(y-yi)/(yj-yi)+xi {
			inside = !inside
		}
	}
	return inside
}

func segmentsCross(ax, ay, bx, by, cx, cy, dx, dy float64) bool {
	d1 := (dx-cx)*(ay-cy) - (dy-cy)*(ax-cx)
	d2 := (dx-cx)*(by-cy) - (dy-cy)*(bx-cx)
	d3 := (bx-ax)*(cy-ay) - (by-ay)*(cx-ax)
	d4 := (bx-ax)*(dy-ay) - (by-ay)*(dx-ax)
	return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0))
}

func (z *routeZone) hitsSegment(ax, ay, bx, by float64) bool {
	if math.Max(ax, bx) < z.minX || math.Min(ax, bx) > z.maxX || math.Max(ay, by) < z.minY || math.Min(ay, by) > z.maxY {
		return false
	}
	if pointInPolyXY(ax, ay, z.poly) || pointInPolyXY(bx, by, z.poly) {
		return true
	}
	for i, j := 0, len(z.poly)-1; i < len(z.poly); j, i = i, i+1 {
		if segmentsCross(ax, ay, bx, by, z.poly[j][0], z.poly[j][1], z.poly[i][0], z.poly[i][1]) {
			return true
		}
	}
	return false
}

func (z *routeZone) contains(x, y float64) bool {
	return x >= z.minX && x <= z.maxX && y >= z.minY && y <= z.maxY && pointInPolyXY(x, y, z.poly)
}

// highestFree returns the highest altitude in [lo, hi] outside all bands
// (an altitude right at a band's edge is fine).
func highestFree(lo, hi float64, bands [][2]float64) (float64, bool) {
	top := hi
	for moved := true; moved && top >= lo; {
		moved = false
		for _, b := range bands {
			if b[0] < top && top < b[1] {
				top, moved = b[0], true
			}
		}
	}
	return top, top >= lo
}

// airportByIdent finds an airport in the compact list (values are float64
// after the JSON cache round trip, but may be ints when freshly parsed).
func airportByIdent(data AirportData, ident string) (RouteWaypoint, bool) {
	ident = strings.ToUpper(strings.TrimSpace(ident))
	for _, a := range data.Airports {
		if len(a) < 5 {
			continue
		}
		if id, _ := a[0].(string); id == ident {
			name, _ := a[1].(string)
			return RouteWaypoint{Kind: "APT", Ident: id, Name: name, Lat: toFloat(a[2]), Lon: toFloat(a[3])}, true
		}
	}
	return RouteWaypoint{}, false
}

func toFloat(v interface{}) float64 {
	switch n := v.(type) {
	case float64:
		return n
	case float32:
		return float64(n)
	case int:
		return float64(n)
	}
	return 0
}

// autoRoute plans the route; see the comment at the top of the file.
func autoRoute(xplaneRoot string, airports AirportData, nav *navCache, req AutoRouteRequest) (AutoRouteResult, error) {
	from, ok := airportByIdent(airports, req.From)
	if !ok {
		return AutoRouteResult{}, fmt.Errorf("unknown airport %q", req.From)
	}
	to, ok := airportByIdent(airports, req.To)
	if !ok {
		return AutoRouteResult{}, fmt.Errorf("unknown airport %q", req.To)
	}
	if from.Ident == to.Ident {
		return AutoRouteResult{}, errors.New("departure and destination are the same airport")
	}
	if req.CruiseFt <= 0 {
		req.CruiseFt = 3500
	}
	direct := greatCircleNm(from.Lat, from.Lon, to.Lat, to.Lon)
	// Wider corridor and longer legs on the second try.
	for _, try := range []struct{ margin, maxLeg float64 }{
		{math.Max(25, direct*0.3), 40},
		{math.Max(60, direct*0.7), 80},
	} {
		if wps, raise, ok := searchRoute(xplaneRoot, airports, nav, req, from, to, try.margin, try.maxLeg); ok {
			res := AutoRouteResult{Waypoints: wps, DirectNm: direct}
			for i := 1; i < len(wps); i++ {
				res.DistanceNm += greatCircleNm(wps[i-1].Lat, wps[i-1].Lon, wps[i].Lat, wps[i].Lon)
			}
			terrainNotes := checkTerrain(xplaneRoot, wps, req.CruiseFt, raise)
			res.Notes = append(routeNotes(airports, nav, req, wps), terrainNotes...)
			return res, nil
		}
	}
	what := "prohibited/restricted areas"
	if req.AvoidControlled {
		what = "controlled airspace and prohibited/restricted areas"
	}
	return AutoRouteResult{}, fmt.Errorf("no route found up to %d ft around %s - try a higher cruise altitude", req.CruiseFt, what)
}

func elevationOf(airports AirportData, ident string) float64 {
	return float64(airports.Details[ident].ElevationFt)
}

func searchRoute(xplaneRoot string, airports AirportData, nav *navCache, req AutoRouteRequest, from, to RouteWaypoint, marginNm, maxLegNm float64) ([]RouteWaypoint, func(int, float64) (float64, bool), bool) {
	proj := newProjection((from.Lat + to.Lat) / 2)
	fx, fy := proj.xy(from.Lat, from.Lon)
	tx, ty := proj.xy(to.Lat, to.Lon)
	minX, maxX := math.Min(fx, tx)-marginNm, math.Max(fx, tx)+marginNm
	minY, maxY := math.Min(fy, ty)-marginNm, math.Max(fy, ty)+marginNm
	inBox := func(x, y float64) bool { return x >= minX && x <= maxX && y >= minY && y <= maxY }
	boxMinLat, boxMinLon := proj.latLon(minX, minY)
	boxMaxLat, boxMaxLon := proj.latLon(maxX, maxY)

	cruise := float64(req.CruiseFt)
	fromElev, toElev := elevationOf(airports, from.Ident), elevationOf(airports, to.Ident)
	enrouteMin := math.Min(cruise, math.Max(autoRouteMinFt, math.Max(fromElev, toElev)+autoRouteMinAglFt))
	lowest := math.Min(enrouteMin, math.Min(fromElev, toElev)+autoRouteLowAglFt)

	// Airspaces in the corridor that take away altitudes. The airports' own
	// control zones are entered via reporting points.
	zones := []*routeZone{}
	for i := range nav.airspaces {
		a := &nav.airspaces[i]
		noGo, ctl := isNoGoClass(a.Class), isControlledClass(a.Class)
		if !noGo && !ctl {
			continue
		}
		if a.maxLon < boxMinLon || a.minLon > boxMaxLon || a.maxLat < boxMinLat || a.minLat > boxMaxLat {
			continue // quick reject before projecting
		}
		z := proj.zone(a)
		if z.maxX < minX || z.minX > maxX || z.maxY < minY || z.minY > maxY {
			continue
		}
		if a.LowerGnd && (ctl || noGo) {
			if z.contains(fx, fy) {
				z.terminal = 1
			} else if z.contains(tx, ty) {
				z.terminal = 2
			}
		}
		lo, hi := z.band()
		if z.terminal == 0 && (ctl && !req.AvoidControlled || hi <= lowest || lo >= cruise) {
			continue // never in the way
		}
		zones = append(zones, z)
	}
	terminalZoned := [3]bool{}
	for _, z := range zones {
		terminalZoned[z.terminal] = true
	}

	// What using each kind of point costs (NM): reporting points and navaids
	// are what a VFR pilot can find, helper points only when nothing else
	// gets around an airspace.
	w := struct{ vrp, navaid, airport, helper float64 }{1, 1.5, 3, 10}
	if req.RadioNav {
		w = struct{ vrp, navaid, airport, helper float64 }{2, 0.5, 4, 20}
	}
	nodes := []routeNode{}
	add := func(wp RouteWaypoint, penalty float64, terminal int) bool {
		x, y := proj.xy(wp.Lat, wp.Lon)
		if terminal == 0 {
			if !inBox(x, y) {
				return false
			}
			// Points inside an airport's control zone would be shortcuts
			// through it.
			for _, z := range zones {
				if z.terminal != 0 && z.contains(x, y) {
					return false
				}
			}
		}
		nodes = append(nodes, routeNode{wp: wp, x: x, y: y, penalty: penalty, terminal: terminal})
		return true
	}
	add(from, 0, 1) // node 0
	add(to, 0, 2)   // node 1
	for _, p := range nav.points {
		wp := RouteWaypoint{Kind: p.Kind, Ident: p.Ident, Name: p.Name, Lat: p.Lat, Lon: p.Lon}
		switch {
		case p.Kind == "VRP" && p.Airport == from.Ident:
			add(wp, w.vrp/2, 1)
		case p.Kind == "VRP" && p.Airport == to.Ident:
			add(wp, w.vrp/2, 2)
		case p.Kind == "VRP":
			if !req.RadioNav {
				add(wp, w.vrp, 0)
			}
		default:
			add(wp, w.navaid, 0)
		}
	}
	for _, a := range airports.Airports {
		if len(a) < 5 || toFloat(a[4]) != 1 {
			continue
		}
		id, _ := a[0].(string)
		if id == from.Ident || id == to.Ident {
			continue
		}
		name, _ := a[1].(string)
		if isMilitary(id, name) {
			continue // overflying an air base isn't a VFR waypoint
		}
		penalty := w.airport
		if !isIcaoIdent(id) {
			// Ultralight/private strips ("XEDBX", "DE-0042"): hard to spot.
			if req.RadioNav {
				continue
			}
			penalty = w.helper
		}
		add(RouteWaypoint{Kind: "APT", Ident: id, Name: name, Lat: toFloat(a[2]), Lon: toFloat(a[3])}, penalty, 0)
	}
	// Helper points around airspaces in the way, at least ~3 NM apart.
	for _, z := range zones {
		if z.terminal != 0 {
			continue
		}
		cx, cy := (z.minX+z.maxX)/2, (z.minY+z.maxY)/2
		lastX, lastY := math.Inf(1), math.Inf(1)
		for _, p := range z.poly {
			if math.Hypot(p[0]-lastX, p[1]-lastY) < 3 {
				continue
			}
			lastX, lastY = p[0], p[1]
			d := math.Hypot(p[0]-cx, p[1]-cy)
			if d < 0.01 {
				continue
			}
			lat, lon := proj.latLon(p[0]+(p[0]-cx)/d*autoRouteClearNm, p[1]+(p[1]-cy)/d*autoRouteClearNm)
			if add(RouteWaypoint{Kind: "USR", Lat: lat, Lon: lon}, w.helper, 0) {
				nodes[len(nodes)-1].around = strings.Join(strings.Fields(z.a.Name), " ")
			}
		}
	}
	if len(nodes) > autoRouteMaxNodes {
		// Keep the departure/destination side and the points closest to
		// the direct line.
		lineDist := func(n routeNode) float64 { return pointSegDist(n.x, n.y, fx, fy, tx, ty) }
		rest := nodes[2:]
		sort.SliceStable(rest, func(i, j int) bool {
			if (rest[i].terminal != 0) != (rest[j].terminal != 0) {
				return rest[i].terminal != 0
			}
			return lineDist(rest[i]) < lineDist(rest[j])
		})
		nodes = nodes[:autoRouteMaxNodes]
	}

	// Into or out of an airport in a control zone only via its own
	// reporting points, if it has any.
	viaVrps := [3]bool{}
	for _, n := range nodes[2:] {
		if n.terminal != 0 && terminalZoned[n.terminal] {
			viaVrps[n.terminal] = true
		}
	}
	// legAlt returns the altitude to fly from node i to node j and the
	// extra cost of crossing danger areas, if that can't be avoided.
	legAltAbove := func(i, j int, floor float64) (float64, float64, bool) {
		a, b := &nodes[i], &nodes[j]
		for _, end := range []int{1, 2} {
			airport := end - 1
			if viaVrps[end] && ((i == airport && b.terminal != end) || (j == airport && a.terminal != end)) {
				return 0, 0, false
			}
		}
		// Short legs out of / into an airport may stay low (under a class C
		// shelf right above it); everything else keeps the en-route minimum.
		lo := enrouteMin
		if (i <= 1 || j <= 1) && math.Hypot(a.x-b.x, a.y-b.y) <= autoRouteLowLegNm {
			lo = 0
			for _, k := range []int{i, j} {
				if k <= 1 {
					lo = math.Max(lo, []float64{fromElev, toElev}[k]+autoRouteLowAglFt)
				}
			}
			lo = math.Min(lo, enrouteMin)
		}
		lo = math.Max(lo, floor)
		// Terrain from the installed scenery (unknown elsewhere - the check
		// after the search covers that).
		if ground, ok := legGround(xplaneRoot, a.wp, b.wp, i <= 1, j <= 1); ok {
			lo = math.Max(lo, ground+terrainClearance(i <= 1 || j <= 1, math.Hypot(a.x-b.x, a.y-b.y)))
		}
		hard, danger := [][2]float64{}, [][2]float64{}
		for _, z := range zones {
			if z.terminal != 0 && (a.terminal == z.terminal || b.terminal == z.terminal) {
				continue
			}
			if z.hitsSegment(a.x, a.y, b.x, b.y) {
				l, h := z.band()
				if z.a.Class == "Q" {
					danger = append(danger, [2]float64{l, h})
				} else {
					hard = append(hard, [2]float64{l, h})
				}
			}
		}
		if alt, ok := highestFree(lo, cruise, append(append([][2]float64{}, hard...), danger...)); ok {
			return alt, 0, true
		}
		alt, ok := highestFree(lo, cruise, hard)
		if !ok {
			return 0, 0, false
		}
		extra := 0.0
		for _, d := range danger {
			if d[0] < alt && alt < d[1] {
				extra += autoRouteDangerNm
			}
		}
		return alt, extra, true
	}
	legAlt := func(i, j int) (float64, float64, bool) { return legAltAbove(i, j, 0) }

	path, alts, ok := aStar(nodes, maxLegNm, cruise, legAlt)
	if !ok {
		return nil, nil, false
	}
	// raise finds the highest altitude for the leg to path[k] that is at
	// least floor (terrain clearance), or false if airspace is in the way.
	raise := func(k int, floor float64) (float64, bool) {
		alt, _, ok := legAltAbove(path[k-1], path[k], floor)
		return alt, ok
	}
	wps := make([]RouteWaypoint, len(path))
	for i, k := range path {
		wps[i] = nodes[k].wp
		if i > 0 && math.Round(alts[i]) < cruise {
			wps[i].AltFt = int(math.Floor(alts[i]/100) * 100)
		}
		if wps[i].Kind == "USR" {
			parts := []string{}
			if fix := vorFix(nav.points, wps[i].Lat, wps[i].Lon); fix != "" {
				parts = append(parts, fix)
			}
			if around := aroundWhat(nodes, zones, path, i, alts); around != "" {
				parts = append(parts, "around "+around)
			}
			wps[i].Name = strings.Join(parts, " · ")
		}
	}
	return wps, raise, true
}

// aroundWhat names the airspace a helper point at path[i] leads around: one
// the shortcut from the previous to the next point would cross (at the
// legs' altitude) but the two actual legs don't. Falls back to the airspace
// whose corner the point was made from.
func aroundWhat(nodes []routeNode, zones []*routeZone, path []int, i int, alts []float64) string {
	if i > 0 && i < len(path)-1 {
		p, c, n := &nodes[path[i-1]], &nodes[path[i]], &nodes[path[i+1]]
		alt := math.Min(alts[i], alts[i+1])
		for _, z := range zones {
			lo, hi := z.band()
			if z.terminal != 0 || alt <= lo || alt >= hi {
				continue
			}
			if z.hitsSegment(p.x, p.y, n.x, n.y) && !z.hitsSegment(p.x, p.y, c.x, c.y) && !z.hitsSegment(c.x, c.y, n.x, n.y) {
				return strings.Join(strings.Fields(z.a.Name), " ")
			}
		}
	}
	return nodes[path[i]].around
}

// terrainClearance: SERA's minimum heights - 1000 ft en route, 500 ft on
// the short legs out of and into an airport.
func terrainClearance(touchesAirport bool, lengthNm float64) float64 {
	if touchesAirport && lengthNm <= autoRouteLowLegNm {
		return 500
	}
	return terrainClearanceFt
}

// legGround is the highest ground within a mile of a leg from the X-Plane
// scenery (false if a tile isn't installed), leaving out the first/last
// 2 NM at an airport end (climb-out and approach).
func legGround(xplaneRoot string, a, b RouteWaypoint, skipA, skipB bool) (float64, bool) {
	if xplaneRoot == "" {
		return 0, false
	}
	d := greatCircleNm(a.Lat, a.Lon, b.Lat, b.Lon)
	n := max(1, int(math.Ceil(d/terrainStepNm)))
	proj := newProjection((a.Lat + b.Lat) / 2)
	ax, ay := proj.xy(a.Lat, a.Lon)
	bx, by := proj.xy(b.Lat, b.Lon)
	ux, uy := 0.0, 0.0
	if d > 0 {
		l := math.Hypot(bx-ax, by-ay)
		ux, uy = (bx-ax)/l, (by-ay)/l
	}
	hi := 0.0
	for k := 0; k <= n; k++ {
		along := d * float64(k) / float64(n)
		if (skipA && along < 2) || (skipB && d-along < 2) {
			continue
		}
		f := float64(k) / float64(n)
		x, y := ax+(bx-ax)*f, ay+(by-ay)*f
		for _, side := range []float64{0, -terrainSideNm, terrainSideNm} {
			lat, lon := proj.latLon(x-uy*side, y+ux*side)
			v, ok := xplaneElevation(xplaneRoot, lat, lon)
			if !ok {
				return 0, false
			}
			hi = math.Max(hi, v)
		}
	}
	return hi, true
}

// checkTerrain raises legs to 1000 ft above the highest ground within a
// mile of them (where airspace allows) and reports the ones it can't.
func checkTerrain(xplaneRoot string, wps []RouteWaypoint, cruiseFt int, raise func(int, float64) (float64, bool)) []string {
	terrain, err := routeTerrain(xplaneRoot, wps, 2)
	if err != nil {
		return []string{"Terrain NOT checked (" + err.Error() + ") - check the heights yourself"}
	}
	notes := []string{}
	raised, short := 0, []string{}
	highestNeed := 0
	for i := 1; i < len(wps); i++ {
		ground := terrain.LegMaxFt[i]
		if ground <= 0 {
			continue
		}
		touches := i == 1 || i == len(wps)-1
		need := math.Ceil((ground+terrainClearance(touches, greatCircleNm(wps[i-1].Lat, wps[i-1].Lon, wps[i].Lat, wps[i].Lon)))/100) * 100
		cur := float64(cruiseFt)
		if wps[i].AltFt > 0 {
			cur = float64(wps[i].AltFt)
		}
		if cur >= need {
			continue
		}
		if alt, ok := raise(i, need); ok {
			alt = math.Floor(alt/100) * 100
			wps[i].AltFt = int(alt)
			if alt >= float64(cruiseFt) {
				wps[i].AltFt = 0
			}
			raised++
			continue
		}
		name := wps[i].Ident
		if name == "" {
			name = fmt.Sprintf("WPT%d", i+1)
		}
		short = append(short, fmt.Sprintf("to %s (ground %.0f ft)", name, ground))
		highestNeed = max(highestNeed, int(need))
	}
	if raised > 0 {
		notes = append(notes, fmt.Sprintf("%d leg(s) raised to clear the terrain", raised))
	}
	if len(short) > 0 {
		hint := "airspace above doesn't leave room - fly around the high ground or plan differently"
		if highestNeed > cruiseFt {
			hint = fmt.Sprintf("raise the cruise altitude to at least %d ft or fly around the high ground", highestNeed)
		}
		notes = append(notes, "Too close to the terrain on the leg(s) "+strings.Join(short, ", ")+": "+hint)
	}
	notes = append(notes, "Terrain checked 1 NM either side, 1000 ft clearance en route / 500 ft near the airports ("+terrain.Source+"); obstacles like masts and wind turbines are not included")
	return notes
}

// isIcaoIdent: four letters, not an X-prefixed (non-ICAO) local ident.
func isIcaoIdent(id string) bool {
	if len(id) != 4 || id[0] == 'X' {
		return false
	}
	for _, c := range id {
		if c < 'A' || c > 'Z' {
			return false
		}
	}
	return true
}

// vorFix describes a lat/lon point as radial/distance from the nearest VOR
// ("FFM R245/18"), so it can be found without a GPS. Empty if none is near.
func vorFix(points []NavPoint, lat, lon float64) string {
	best, bestNm := -1, float64(autoRouteFixVorNm)
	for i, p := range points {
		if p.Kind != "VOR" || math.Abs(p.Lat-lat) > 1.5 {
			continue
		}
		if d := greatCircleNm(p.Lat, p.Lon, lat, lon); d < bestNm {
			best, bestNm = i, d
		}
	}
	if best < 0 {
		return ""
	}
	v := points[best]
	const rad = math.Pi / 180
	y := math.Sin((lon-v.Lon)*rad) * math.Cos(lat*rad)
	x := math.Cos(v.Lat*rad)*math.Sin(lat*rad) - math.Sin(v.Lat*rad)*math.Cos(lat*rad)*math.Cos((lon-v.Lon)*rad)
	trueBrg := math.Atan2(y, x) / rad
	radial := int(math.Round(math.Mod(trueBrg-v.MagVar+720, 360)))
	if radial == 0 {
		radial = 360
	}
	return fmt.Sprintf("%s R%03d/%.0f", v.Ident, radial, bestNm)
}

func pointSegDist(px, py, ax, ay, bx, by float64) float64 {
	dx, dy := bx-ax, by-ay
	l2 := dx*dx + dy*dy
	t := 0.0
	if l2 > 0 {
		t = math.Max(0, math.Min(1, ((px-ax)*dx+(py-ay)*dy)/l2))
	}
	return math.Hypot(px-(ax+t*dx), py-(ay+t*dy))
}

type pqItem struct {
	node int
	f    float64
}
type pq []pqItem

func (q pq) Len() int            { return len(q) }
func (q pq) Less(i, j int) bool  { return q[i].f < q[j].f }
func (q pq) Swap(i, j int)       { q[i], q[j] = q[j], q[i] }
func (q *pq) Push(x interface{}) { *q = append(*q, x.(pqItem)) }
func (q *pq) Pop() interface{} {
	old := *q
	it := old[len(old)-1]
	*q = old[:len(old)-1]
	return it
}

// aStar searches from node 0 to node 1; edges are evaluated lazily (only
// between nodes up to maxLegNm apart). Legs below the cruise altitude cost
// a little extra, so the route prefers staying high. Returns the path and
// the altitude of the leg to each of its nodes.
func aStar(nodes []routeNode, maxLegNm, cruise float64, legAlt func(i, j int) (float64, float64, bool)) ([]int, []float64, bool) {
	n := len(nodes)
	g := make([]float64, n)
	alt := make([]float64, n)
	prev := make([]int, n)
	closed := make([]bool, n)
	for i := range g {
		g[i] = math.Inf(1)
		prev[i] = -1
	}
	h := func(i int) float64 { return math.Hypot(nodes[i].x-nodes[1].x, nodes[i].y-nodes[1].y) }
	g[0] = 0
	open := &pq{{node: 0, f: h(0)}}
	for open.Len() > 0 {
		cur := heap.Pop(open).(pqItem).node
		if closed[cur] {
			continue
		}
		if cur == 1 {
			path, alts := []int{}, []float64{}
			for k := 1; k != -1; k = prev[k] {
				path = append([]int{k}, path...)
				alts = append([]float64{alt[k]}, alts...)
			}
			return path, alts, true
		}
		closed[cur] = true
		c := &nodes[cur]
		for j := 1; j < n; j++ {
			if closed[j] {
				continue
			}
			d := math.Hypot(nodes[j].x-c.x, nodes[j].y-c.y)
			if d > maxLegNm || d < 0.3 {
				continue
			}
			cost := g[cur] + d + nodes[j].penalty
			if cost >= g[j] {
				continue
			}
			a, extra, ok := legAlt(cur, j)
			if !ok {
				continue
			}
			cost += extra
			if j != 1 {
				cost += 0.25 * math.Max(0, cruise-a) / 500
			}
			if cost >= g[j] {
				continue
			}
			g[j] = cost
			alt[j] = a
			prev[j] = cur
			heap.Push(open, pqItem{node: j, f: cost + h(j)})
		}
	}
	return nil, nil, false
}

// routeNotes: which controlled airspace the route enters (needs a
// clearance), lower legs, and the altitude/terrain caveats.
func routeNotes(airports AirportData, nav *navCache, req AutoRouteRequest, wps []RouteWaypoint) []string {
	notes := []string{}
	proj := newProjection((wps[0].Lat + wps[len(wps)-1].Lat) / 2)
	xs, ys := make([]float64, len(wps)), make([]float64, len(wps))
	for i, w := range wps {
		xs[i], ys[i] = proj.xy(w.Lat, w.Lon)
	}
	legAltFt := func(k int) float64 {
		if wps[k].AltFt > 0 {
			return float64(wps[k].AltFt)
		}
		return float64(req.CruiseFt)
	}
	last := len(wps) - 1
	crossed := map[string]bool{}
	clearance, danger := []string{}, []string{}
	for i := range nav.airspaces {
		a := &nav.airspaces[i]
		if !isControlledClass(a.Class) && a.Class != "Q" {
			continue
		}
		z := proj.zone(a)
		for k := 1; k <= last; k++ {
			if !z.hitsSegment(xs[k-1], ys[k-1], xs[k], ys[k]) {
				continue
			}
			// Climbing out of / descending into the airports' own zones.
			terminal := a.LowerGnd && ((k == 1 && z.contains(xs[0], ys[0])) || (k == last && z.contains(xs[last], ys[last])))
			lo, hi := z.band()
			alt := legAltFt(k)
			if a.Class == "Q" && (terminal || (lo < alt && alt < hi)) {
				label := strings.Join(strings.Fields(a.Name), " ")
				if !crossed[label] {
					crossed[label] = true
					danger = append(danger, label)
				}
				break
			}
			if a.Class != "Q" && (terminal || a.containsAlt(alt)) {
				label := fmt.Sprintf("%s (%s)", a.Name, a.Class)
				if !crossed[label] {
					crossed[label] = true
					clearance = append(clearance, label)
				}
				break
			}
		}
	}
	shorten := func(list []string) string {
		if len(list) > 6 {
			list = append(list[:6], fmt.Sprintf("+%d more", len(list)-6))
		}
		return strings.Join(list, ", ")
	}
	if len(clearance) > 0 {
		notes = append(notes, "Clearance needed: "+shorten(clearance))
	}
	if len(danger) > 0 {
		notes = append(notes, "Crosses danger areas - check whether they're active (FIS/NOTAM): "+shorten(danger))
	}
	lowest := 0
	for _, w := range wps[1:] {
		if w.AltFt > 0 && (lowest == 0 || w.AltFt < lowest) {
			lowest = w.AltFt
		}
	}
	if lowest > 0 {
		notes = append(notes, fmt.Sprintf("Some legs are lower than %d ft (down to %d ft) to stay clear of airspace - see the altitude per leg", req.CruiseFt, lowest))
	}
	highest := math.Max(elevationOf(airports, wps[0].Ident), elevationOf(airports, wps[last].Ident))
	if float64(req.CruiseFt) < highest+autoRouteMinAglFt {
		notes = append(notes, fmt.Sprintf("Cruise altitude is less than 1000 ft above an airport on the route (%.0f ft)", highest))
	}
	notes = append(notes, "Prohibited and restricted areas are always avoided (activity times unknown).")
	return notes
}
