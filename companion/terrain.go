package main

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"
	"sync"
)

// Terrain along a route: from the user's own X-Plane scenery (xpterrain.go
// - the ground X-Plane actually draws), and where no scenery is installed
// from open-meteo.com's elevation API (Copernicus DEM, ~90 m grid).
// Sampled every nautical mile on the route line and a mile to either
// side, so a leg's highest ground is known. Obstacles (masts, wind
// turbines) are not in either.

const (
	elevationBase      = "https://api.open-meteo.com/v1/elevation"
	elevationBatch     = 100 // points per request (the API's limit)
	terrainStepNm      = 1.0
	terrainSideNm      = 1.0
	terrainClearanceFt = 1000
	terrainMaxSamples  = 1500 // ~500 NM of route
)

var (
	elevMu    sync.Mutex
	elevCache = map[string]float64{} // "lat,lon" rounded to ~100 m -> feet
)

func elevKey(lat, lon float64) string {
	return fmt.Sprintf("%.3f,%.3f", lat, lon)
}

// fetchElevations returns the ground elevation in feet for each point and
// whether any of it had to come from the internet.
func fetchElevations(xplaneRoot string, points [][2]float64) ([]float64, bool, error) {
	out := make([]float64, len(points))
	missing := []int{}
	elevMu.Lock()
	for i, p := range points {
		if v, ok := elevCache[elevKey(p[0], p[1])]; ok {
			out[i] = v
		} else {
			missing = append(missing, i)
		}
	}
	elevMu.Unlock()
	if xplaneRoot != "" {
		still := []int{}
		for _, i := range missing {
			if v, ok := xplaneElevation(xplaneRoot, points[i][0], points[i][1]); ok {
				out[i] = v
			} else {
				still = append(still, i)
			}
		}
		missing = still
	}
	online := len(missing) > 0
	for start := 0; start < len(missing); start += elevationBatch {
		batch := missing[start:min(start+elevationBatch, len(missing))]
		lats, lons := make([]string, len(batch)), make([]string, len(batch))
		for k, i := range batch {
			lats[k] = fmt.Sprintf("%.3f", points[i][0])
			lons[k] = fmt.Sprintf("%.3f", points[i][1])
		}
		body, err := fetchURL(fmt.Sprintf("%s?latitude=%s&longitude=%s", elevationBase, strings.Join(lats, ","), strings.Join(lons, ",")))
		if err != nil {
			return nil, online, err
		}
		var res struct {
			Elevation []float64 `json:"elevation"`
		}
		if err := json.Unmarshal(body, &res); err != nil || len(res.Elevation) != len(batch) {
			return nil, online, fmt.Errorf("unexpected elevation answer")
		}
		elevMu.Lock()
		for k, i := range batch {
			ft := math.Max(0, res.Elevation[k]) * 3.28084
			out[i] = ft
			elevCache[elevKey(points[i][0], points[i][1])] = ft
		}
		elevMu.Unlock()
	}
	return out, online, nil
}

// RouteTerrain is the ground along a route.
type RouteTerrain struct {
	LegMaxFt []float64    // index = waypoint ending the leg, [0] unused
	Profile  [][2]float64 // [along NM, highest ground ft] every mile
	Source   string       // "X-Plane scenery", "open-meteo.com" or both
}

// routeTerrain samples the ground along the route. Within skipNm of the
// first and last waypoint the ground isn't counted for the legs' maximum
// (that's the climb-out and approach, flown lower anyway).
func routeTerrain(xplaneRoot string, wps []RouteWaypoint, skipNm float64) (RouteTerrain, error) {
	type sample struct {
		leg   int
		along float64
		idx   []int // indices into points: centre and both sides
	}
	points := [][2]float64{}
	samples := []sample{}
	total := 0.0
	for i := 1; i < len(wps); i++ {
		total += greatCircleNm(wps[i-1].Lat, wps[i-1].Lon, wps[i].Lat, wps[i].Lon)
	}
	along := 0.0
	for i := 1; i < len(wps); i++ {
		a, b := wps[i-1], wps[i]
		d := greatCircleNm(a.Lat, a.Lon, b.Lat, b.Lon)
		n := max(1, int(math.Ceil(d/terrainStepNm)))
		proj := newProjection((a.Lat + b.Lat) / 2)
		ax, ay := proj.xy(a.Lat, a.Lon)
		bx, by := proj.xy(b.Lat, b.Lon)
		ux, uy := 0.0, 0.0
		if l := math.Hypot(bx-ax, by-ay); l > 0 {
			ux, uy = (bx-ax)/l, (by-ay)/l
		}
		for k := 0; k <= n; k++ {
			if k == 0 && i > 1 {
				continue // shared with the previous leg's last sample
			}
			f := float64(k) / float64(n)
			x, y := ax+(bx-ax)*f, ay+(by-ay)*f
			s := sample{leg: i, along: along + d*f}
			for _, side := range []float64{0, -terrainSideNm, terrainSideNm} {
				lat, lon := proj.latLon(x-uy*side, y+ux*side)
				s.idx = append(s.idx, len(points))
				points = append(points, [2]float64{lat, lon})
			}
			samples = append(samples, s)
		}
		along += d
	}
	if len(points) > terrainMaxSamples*3 {
		return RouteTerrain{}, fmt.Errorf("route too long for the terrain check")
	}
	elev, online, err := fetchElevations(xplaneRoot, points)
	if err != nil {
		return RouteTerrain{}, err
	}
	rt := RouteTerrain{LegMaxFt: make([]float64, len(wps)), Profile: [][2]float64{}, Source: "X-Plane scenery"}
	if online {
		rt.Source = "X-Plane scenery + open-meteo.com where none is installed"
		if xplaneRoot == "" {
			rt.Source = "open-meteo.com"
		}
	}
	for _, s := range samples {
		hi := 0.0
		for _, i := range s.idx {
			hi = math.Max(hi, elev[i])
		}
		rt.Profile = append(rt.Profile, [2]float64{math.Round(s.along*10) / 10, math.Round(hi)})
		if s.along >= skipNm && s.along <= total-skipNm {
			rt.LegMaxFt[s.leg] = math.Max(rt.LegMaxFt[s.leg], hi)
		}
	}
	return rt, nil
}
