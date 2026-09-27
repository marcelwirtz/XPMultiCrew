package main

import (
	"errors"
	"math"
	"sort"
	"strings"
)

// --- Airports page (airport_detail.go) -----------------------------------------

// AirportHit is one search result / nearby airport.
type AirportHit struct {
	Ident  string  `json:"ident"`
	Name   string  `json:"name"`
	Lat    float64 `json:"lat"`
	Lon    float64 `json:"lon"`
	DistNm float64 `json:"distNm,omitempty"`
}

func hitFrom(a []interface{}) (AirportHit, bool) {
	if len(a) < 4 {
		return AirportHit{}, false
	}
	ident, ok1 := a[0].(string)
	name, ok2 := a[1].(string)
	lat, ok3 := a[2].(float64)
	lon, ok4 := a[3].(float64)
	return AirportHit{Ident: ident, Name: name, Lat: lat, Lon: lon}, ok1 && ok2 && ok3 && ok4
}

// searchAirports matches the ident first (exact, then prefix), then the name.
func searchAirports(airports [][]interface{}, query string, limit int) []AirportHit {
	q := strings.ToUpper(strings.TrimSpace(query))
	out := []AirportHit{}
	if q == "" {
		return out
	}
	var exact, prefix, byName []AirportHit
	for _, a := range airports {
		h, ok := hitFrom(a)
		if !ok {
			continue
		}
		switch {
		case h.Ident == q:
			exact = append(exact, h)
		case strings.HasPrefix(h.Ident, q):
			prefix = append(prefix, h)
		case len(q) >= 3 && strings.Contains(strings.ToUpper(h.Name), q):
			byName = append(byName, h)
		}
	}
	sort.Slice(prefix, func(i, j int) bool { return prefix[i].Ident < prefix[j].Ident })
	for _, list := range [][]AirportHit{exact, prefix, byName} {
		for _, h := range list {
			if len(out) >= limit {
				return out
			}
			out = append(out, h)
		}
	}
	return out
}

// nearestAirports returns up to `limit` land airports within maxNm, closest first.
func nearestAirports(airports [][]interface{}, lat, lon, maxNm float64, limit int) []AirportHit {
	out := []AirportHit{}
	dLat := maxNm / 60
	for _, a := range airports {
		if len(a) >= 5 {
			// int when freshly parsed, float64 when read back from the cache
			if kind, ok := a[4].(int); ok && kind != 1 {
				continue // seaplane bases and heliports
			}
			if kind, ok := a[4].(float64); ok && kind != 1 {
				continue
			}
		}
		h, ok := hitFrom(a)
		if !ok || math.Abs(h.Lat-lat) > dLat {
			continue
		}
		if d := nmBetween(lat, lon, h.Lat, h.Lon); d <= maxNm {
			h.DistNm = math.Round(d*10) / 10
			out = append(out, h)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].DistNm < out[j].DistNm })
	if len(out) > limit {
		out = out[:limit]
	}
	return out
}

func (a *App) airportList() ([][]interface{}, error) {
	root := loadConfig().XPlanePath
	if root == "" {
		return nil, errors.New("choose your X-Plane folder first (Setup)")
	}
	data, err := loadAirports(root)
	if err != nil {
		return nil, err
	}
	return data.Airports, nil
}

// SearchAirports finds airports by ICAO code or name.
func (a *App) SearchAirports(query string) ([]AirportHit, error) {
	list, err := a.airportList()
	if err != nil {
		return nil, err
	}
	return searchAirports(list, query, 20), nil
}

// NearestAirports lists the airports around a position (the own aircraft).
func (a *App) NearestAirports(lat, lon float64) ([]AirportHit, error) {
	list, err := a.airportList()
	if err != nil {
		return nil, err
	}
	return nearestAirports(list, lat, lon, 60, 16), nil
}

// GetAirportLayout returns everything X-Plane knows about one airport.
func (a *App) GetAirportLayout(ident string) (*AirportLayout, error) {
	root := loadConfig().XPlanePath
	if root == "" {
		return nil, errors.New("choose your X-Plane folder first (Setup)")
	}
	return loadAirportLayout(root, strings.ToUpper(strings.TrimSpace(ident)))
}
