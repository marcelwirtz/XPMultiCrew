package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
)

// Airport details for the Airports page: everything X-Plane itself knows
// about one airport, read on demand from the user's own apt.dat (seeking
// to the offset remembered by parseAptDat) and earth_nav.dat - runways with
// surfaces and PAPIs, the paved areas, taxiways with their names, parking
// positions, windsocks, the tower, frequencies and the instrument
// approaches (ILS/LOC/LPV). The layout is drawn by the frontend as our own
// airport diagram - no third-party chart involved.

// AirportLayout is sent to the frontend. Coordinates are metres east (x)
// and north (y) of the airport reference point.
type AirportLayout struct {
	Ident       string           `json:"ident"`
	Name        string           `json:"name"`
	ElevationFt int              `json:"elevationFt"`
	Lat         float64          `json:"lat"`
	Lon         float64          `json:"lon"`
	MagVar      float64          `json:"magVar"` // east positive; magnetic = true - magVar
	Runways     []LayoutRunway   `json:"runways"`
	Pavement    [][][][2]float64 `json:"pavement"` // polygons, each: outer ring then holes
	Taxiways    []TaxiEdge       `json:"taxiways"`
	Parking     []ParkingSpot    `json:"parking"`
	Windsocks   [][2]float64     `json:"windsocks"`
	Tower       *[2]float64      `json:"tower,omitempty"`
	Frequencies []Frequency      `json:"frequencies"`
	Approaches  []Approach       `json:"approaches"`
}

// LayoutRunway is one land runway.
type LayoutRunway struct {
	WidthM  float64            `json:"widthM"`
	LengthM float64            `json:"lengthM"`
	Surface string             `json:"surface"`
	Ends    [2]LayoutRunwayEnd `json:"ends"`
}

// LayoutRunwayEnd is one end of a runway.
type LayoutRunwayEnd struct {
	Name        string  `json:"name"`
	X           float64 `json:"x"`
	Y           float64 `json:"y"`
	HeadingTrue float64 `json:"headingTrue"` // landing direction from this end
	DisplacedM  float64 `json:"displacedM"`
	PapiDeg     float64 `json:"papiDeg,omitempty"` // PAPI/VASI glide angle, 0 if none
}

// TaxiEdge is one segment of the taxi route network.
type TaxiEdge struct {
	Name string     `json:"name"`
	A    [2]float64 `json:"a"`
	B    [2]float64 `json:"b"`
}

// ParkingSpot is a startup location (gate, ramp, tie-down, hangar).
type ParkingSpot struct {
	Name    string  `json:"name"`
	Type    string  `json:"type"`
	X       float64 `json:"x"`
	Y       float64 `json:"y"`
	Heading float64 `json:"heading"`
}

// Approach is an instrument approach aid from earth_nav.dat.
type Approach struct {
	Runway    string  `json:"runway"`
	Kind      string  `json:"kind"` // "ILS-cat-II", "LOC", "LPV", ...
	Ident     string  `json:"ident"`
	Freq      string  `json:"freq,omitempty"`      // MHz, LOC/ILS only
	CourseMag float64 `json:"courseMag,omitempty"` // 0 if unknown
	Course    float64 `json:"course"`              // true
	GlideDeg  float64 `json:"glideDeg,omitempty"`
}

// --- apt.dat block -----------------------------------------------------------

var surfaceNames = map[int]string{1: "Asphalt", 2: "Concrete", 3: "Grass", 4: "Dirt", 5: "Gravel", 12: "Dry lakebed",
	13: "Water", 14: "Snow/ice", 15: "Transparent"}

func surfaceName(code int) string {
	switch {
	case code >= 20 && code <= 38:
		return "Asphalt"
	case code >= 50 && code <= 57:
		return "Concrete"
	}
	if n, ok := surfaceNames[code]; ok {
		return n
	}
	return "Unknown"
}

type pathNode struct {
	p, c [2]float64
	hasC bool
}

// bezierRing turns apt.dat nodes (111-116) into a polyline. A node's
// control point shapes the curve leaving it; mirrored, the one entering it.
func bezierRing(nodes []pathNode, closed bool) [][2]float64 {
	out := [][2]float64{}
	n := len(nodes)
	segments := n - 1
	if closed {
		segments = n
	}
	for i := 0; i < segments; i++ {
		a, b := nodes[i], nodes[(i+1)%n]
		out = append(out, a.p)
		if !a.hasC && !b.hasC {
			continue
		}
		p1 := a.p
		if a.hasC {
			p1 = a.c
		}
		p2 := b.p
		if b.hasC {
			p2 = [2]float64{2*b.p[0] - b.c[0], 2*b.p[1] - b.c[1]}
		}
		for k := 1; k < 8; k++ {
			t := float64(k) / 8
			u := 1 - t
			out = append(out, [2]float64{
				u*u*u*a.p[0] + 3*u*u*t*p1[0] + 3*u*t*t*p2[0] + t*t*t*b.p[0],
				u*u*u*a.p[1] + 3*u*u*t*p1[1] + 3*u*t*t*p2[1] + t*t*t*b.p[1],
			})
		}
	}
	if !closed && n > 0 {
		out = append(out, nodes[n-1].p)
	}
	return out
}

func round1(v float64) float64 { return math.Round(v*10) / 10 }

// parseAirportBlock reads one airport's rows, starting at its header row,
// until the next airport. (lat0, lon0) is the reference point for the
// local coordinates.
func parseAirportBlock(r io.Reader, lat0, lon0 float64) (*AirportLayout, error) {
	lay := &AirportLayout{Lat: lat0, Lon: lon0, Runways: []LayoutRunway{}, Pavement: [][][][2]float64{},
		Taxiways: []TaxiEdge{}, Parking: []ParkingSpot{}, Windsocks: [][2]float64{}, Frequencies: []Frequency{},
		Approaches: []Approach{}}
	xy := func(lat, lon float64) [2]float64 {
		x, y := localXY(lat0, lon0, lat, lon)
		return [2]float64{round1(x), round1(y)}
	}
	num := func(s string) float64 {
		v, _ := strconv.ParseFloat(s, 64)
		return v
	}
	type papi struct {
		rwy         string // often missing - then matched by orientation
		pos         [2]float64
		orientation float64
		angle       float64
	}
	papis := []papi{}
	taxiNodes := map[string][2]float64{}
	type edge struct{ a, b, name string }
	edges := []edge{}

	// Pavement state: which feature the 111-116 nodes belong to.
	inPavement := false
	var ring []pathNode
	var polygon [][][2]float64
	flushPolygon := func() {
		if len(polygon) > 0 {
			lay.Pavement = append(lay.Pavement, polygon)
		}
		polygon = nil
		ring = nil
	}

	s := bufio.NewScanner(r)
	s.Buffer(make([]byte, 64*1024), 1024*1024)
	header := true
	for s.Scan() {
		f := strings.Fields(s.Text())
		if len(f) == 0 {
			continue
		}
		code, err := strconv.Atoi(f[0])
		if err != nil {
			continue
		}
		if header {
			header = false
			if (code != 1 && code != 16 && code != 17) || len(f) < 5 {
				return nil, errors.New("not an airport header")
			}
			lay.ElevationFt, _ = strconv.Atoi(f[1])
			lay.Ident = f[4]
			lay.Name = strings.Join(f[5:], " ")
			continue
		}
		if code == 1 || code == 16 || code == 17 || code == 99 {
			break // next airport
		}
		if code >= 111 && code <= 116 {
			if !inPavement || len(f) < 3 {
				continue
			}
			n := pathNode{p: xy(num(f[1]), num(f[2]))}
			if (code == 112 || code == 114 || code == 116) && len(f) >= 5 {
				n.c, n.hasC = xy(num(f[3]), num(f[4])), true
			}
			ring = append(ring, n)
			if code == 113 || code == 114 {
				if len(ring) >= 3 {
					polygon = append(polygon, bezierRing(ring, true))
				}
				ring = nil
			}
			continue
		}
		if inPavement {
			flushPolygon()
			inPavement = false
		}
		switch code {
		case 110:
			inPavement = true
		case 100:
			if len(f) < 20 {
				continue
			}
			var rw LayoutRunway
			rw.WidthM = num(f[1])
			sc, _ := strconv.Atoi(f[2])
			rw.Surface = surfaceName(sc)
			a := xy(num(f[9]), num(f[10]))
			b := xy(num(f[18]), num(f[19]))
			dx, dy := b[0]-a[0], b[1]-a[1]
			rw.LengthM = math.Round(math.Hypot(dx, dy))
			hdg := math.Mod(math.Atan2(dx, dy)*180/math.Pi+360, 360)
			disp2 := 0.0
			if len(f) > 20 {
				disp2 = num(f[20])
			}
			rw.Ends[0] = LayoutRunwayEnd{Name: f[8], X: a[0], Y: a[1], HeadingTrue: round1(hdg), DisplacedM: num(f[11])}
			rw.Ends[1] = LayoutRunwayEnd{Name: f[17], X: b[0], Y: b[1], HeadingTrue: round1(math.Mod(hdg+180, 360)), DisplacedM: disp2}
			lay.Runways = append(lay.Runways, rw)
		case 1201:
			if len(f) >= 5 {
				taxiNodes[f[4]] = xy(num(f[1]), num(f[2]))
			}
		case 1202:
			if len(f) >= 5 && strings.HasPrefix(f[4], "taxiway") {
				name := ""
				if len(f) >= 6 {
					name = strings.Join(f[5:], " ")
				}
				edges = append(edges, edge{f[1], f[2], name})
			}
		case 1300:
			if len(f) >= 6 {
				p := xy(num(f[1]), num(f[2]))
				lay.Parking = append(lay.Parking, ParkingSpot{Name: strings.Join(f[6:], " "), Type: f[4], X: p[0], Y: p[1], Heading: num(f[3])})
			}
		case 19:
			if len(f) >= 3 {
				lay.Windsocks = append(lay.Windsocks, xy(num(f[1]), num(f[2])))
			}
		case 14:
			if len(f) >= 3 {
				p := xy(num(f[1]), num(f[2]))
				lay.Tower = &p
			}
		case 21:
			// 21 lat lon type orientation angle runway description; types
			// 1 VASI, 2/3 PAPI left/right, 5 tri-colour VASI.
			if len(f) >= 6 && (f[3] == "1" || f[3] == "2" || f[3] == "3" || f[3] == "5") {
				p := papi{pos: xy(num(f[1]), num(f[2])), orientation: num(f[4]), angle: num(f[5])}
				if len(f) >= 7 {
					p.rwy = f[6]
				}
				papis = append(papis, p)
			}
		default:
			if (code >= 50 && code <= 56) || (code >= 1050 && code <= 1056) {
				if len(f) < 2 {
					continue
				}
				freq, err := strconv.Atoi(f[1])
				if err != nil {
					continue
				}
				c := code
				mhz := fmt.Sprintf("%.2f", float64(freq)/100)
				if code >= 1050 {
					c -= 1000
					mhz = fmt.Sprintf("%.3f", float64(freq)/1000)
				}
				lay.Frequencies = append(lay.Frequencies, Frequency{Type: map[int]string{50: "ATIS", 51: "UNICOM", 52: "DEL", 53: "GND",
					54: "TWR", 55: "APP", 56: "DEP"}[c], MHz: mhz, Name: strings.Join(f[2:], " ")})
			}
		}
	}
	if inPavement {
		flushPolygon()
	}
	for _, e := range edges {
		a, ok1 := taxiNodes[e.a]
		b, ok2 := taxiNodes[e.b]
		if ok1 && ok2 {
			lay.Taxiways = append(lay.Taxiways, TaxiEdge{Name: e.name, A: a, B: b})
		}
	}
	for _, p := range papis {
		var best *LayoutRunwayEnd
		bestDist := 2000.0
		for i := range lay.Runways {
			for j := range lay.Runways[i].Ends {
				e := &lay.Runways[i].Ends[j]
				if p.rwy != "" {
					if e.Name == p.rwy {
						best = e
					}
					continue
				}
				d := math.Hypot(p.pos[0]-e.X, p.pos[1]-e.Y)
				if angleDiff(p.orientation, e.HeadingTrue) < 30 && d < bestDist {
					best, bestDist = e, d
				}
			}
		}
		if best != nil && p.angle > 0 {
			best.PapiDeg = p.angle
		}
	}
	if lay.Ident == "" {
		return nil, errors.New("airport not found")
	}
	return lay, s.Err()
}

// --- earth_nav.dat approaches ------------------------------------------------

// parseApproaches reads ILS/LOC (rows 4/5), glideslopes (6) and SBAS/GBAS
// final approach paths (14, with the glide angle from their 16 row) per
// airport. Row: code lat lon elev freq range bearing ident airport region
// runway name. LOC bearing: magnetic*360 + true when both are known;
// glideslope/LTP bearing: angle*100*1000 + true course.
func parseApproaches(r io.Reader) map[string][]Approach {
	out := map[string][]Approach{}
	type key struct{ apt, rwy, ident string }
	glide := map[key]float64{}
	s := newScanner(r)
	for s.Scan() {
		f := strings.Fields(s.Text())
		if len(f) < 11 {
			continue
		}
		switch f[0] {
		case "4", "5", "6", "14", "16":
		default:
			continue
		}
		bearing, err := strconv.ParseFloat(f[6], 64)
		if err != nil {
			continue
		}
		apt, rwy, ident := f[8], f[10], f[7]
		switch f[0] {
		case "6", "16":
			glide[key{apt, rwy, ident}] = math.Floor(bearing/1000) / 100
		case "4", "5":
			freq, _ := strconv.Atoi(f[4])
			a := Approach{Runway: rwy, Ident: ident, Freq: fmt.Sprintf("%.2f", float64(freq)/100), Kind: "LOC"}
			if len(f) > 11 {
				a.Kind = strings.Join(f[11:], " ")
			}
			if bearing >= 360 {
				mag := math.Floor(bearing / 360)
				a.CourseMag, a.Course = mag, round1(bearing-mag*360)
			} else {
				a.Course = round1(bearing)
			}
			out[apt] = append(out[apt], a)
		case "14":
			a := Approach{Runway: rwy, Ident: ident, Course: round1(math.Mod(bearing, 360)), Kind: "RNAV"}
			if len(f) > 11 {
				a.Kind = f[11]
			}
			out[apt] = append(out[apt], a)
		}
	}
	for apt, list := range out {
		for i := range list {
			if g, ok := glide[key{apt, list[i].Runway, list[i].Ident}]; ok {
				list[i].GlideDeg = g
			}
		}
		sort.SliceStable(list, func(i, j int) bool { return list[i].Runway < list[j].Runway })
	}
	return out
}

var (
	approachMu    sync.Mutex
	approachKey   string
	approachCache map[string][]Approach
)

func approachesFor(xplaneRoot, ident string) []Approach {
	_, nav, _ := navFiles(xplaneRoot)
	key := fileKey(nav)
	approachMu.Lock()
	defer approachMu.Unlock()
	if approachCache == nil || approachKey != key {
		f, err := os.Open(nav)
		if err != nil {
			return []Approach{}
		}
		approachCache = parseApproaches(f)
		approachKey = key
		f.Close()
	}
	if list := approachCache[ident]; list != nil {
		return append([]Approach{}, list...)
	}
	return []Approach{}
}

// --- Loading -------------------------------------------------------------------

// magVarAt estimates the local magnetic variation: from the airport's own
// localizers when they carry both courses, else the nearest VOR's station
// declination.
func magVarAt(lay *AirportLayout, vors []NavPoint) float64 {
	for _, a := range lay.Approaches {
		if a.CourseMag > 0 {
			return round1(math.Remainder(a.Course-a.CourseMag, 360))
		}
	}
	best, bestNm := 0.0, 150.0
	for _, p := range vors {
		if p.Kind != "VOR" || math.Abs(p.Lat-lay.Lat) > 3 {
			continue
		}
		if d := nmBetween(lay.Lat, lay.Lon, p.Lat, p.Lon); d < bestNm {
			best, bestNm = p.MagVar, d
		}
	}
	return best
}

// customSceneryAirport finds the airport in an add-on scenery's apt.dat -
// X-Plane shows the highest one in scenery_packs.ini, so that's the layout
// to draw. Returns "" if no custom scenery has it.
func customSceneryAirport(xplaneRoot, ident string) (path string, offset int64) {
	raw, err := os.ReadFile(filepath.Join(xplaneRoot, "Custom Scenery", "scenery_packs.ini"))
	if err != nil {
		return "", 0
	}
	for _, line := range strings.Split(string(raw), "\n") {
		rest, ok := strings.CutPrefix(strings.TrimSpace(line), "SCENERY_PACK ")
		if !ok {
			continue // SCENERY_PACK_DISABLED and the header
		}
		dir := strings.TrimSpace(rest)
		if !filepath.IsAbs(dir) {
			dir = filepath.Join(xplaneRoot, dir)
		}
		apt := filepath.Join(dir, "Earth nav data", "apt.dat")
		if strings.Contains(dir, "Global Airports") {
			continue // the default data, read elsewhere
		}
		f, err := os.Open(apt)
		if err != nil {
			continue
		}
		lines := newOffsetLineReader(f)
		for lines.next() {
			l := lines.line
			if len(l) < 2 || !(strings.HasPrefix(l, "1 ") || strings.HasPrefix(l, "16 ") || strings.HasPrefix(l, "17 ")) {
				continue
			}
			if fs := strings.Fields(l); len(fs) >= 5 && fs[4] == ident {
				f.Close()
				return apt, lines.offset
			}
		}
		f.Close()
	}
	return "", 0
}

func loadAirportLayout(xplaneRoot, ident string) (*AirportLayout, error) {
	data, err := loadAirports(xplaneRoot)
	if err != nil {
		return nil, err
	}
	path := aptDatPath(xplaneRoot)
	offset, ok := data.Offsets[ident]
	if custom, customOffset := customSceneryAirport(xplaneRoot, ident); custom != "" {
		path, offset, ok = custom, customOffset, true
	}
	if !ok {
		return nil, fmt.Errorf("airport %s not found in X-Plane's airport data", ident)
	}
	lat0, lon0 := 0.0, 0.0
	for _, a := range data.Airports {
		if a[0] == ident {
			lat0, _ = a[2].(float64)
			lon0, _ = a[3].(float64)
			break
		}
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	if _, err := f.Seek(offset, io.SeekStart); err != nil {
		return nil, err
	}
	lay, err := parseAirportBlock(f, lat0, lon0)
	if err != nil {
		return nil, err
	}
	if lay.Ident != ident {
		return nil, errors.New("airport data changed on disk - reopen the Map page to reload it")
	}
	lay.Approaches = approachesFor(xplaneRoot, ident)
	vors := []NavPoint{}
	if c, err := loadNav(xplaneRoot); err == nil {
		vors = c.points
	}
	lay.MagVar = magVarAt(lay, vors)
	return lay, nil
}
