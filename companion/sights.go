package main

import (
	"encoding/json"
	"fmt"
	"hash/fnv"
	"math"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Sights along a route: castles, palaces, lighthouses, mountains, lakes,
// dams, waterfalls, observation towers, cathedrals, bridges, islands,
// glaciers... from Wikidata (CC0), ranked by how many Wikipedia language
// editions have an article about them - a decent measure of "worth a look
// from the air". Also visual waypoints for flying without GPS.

// Sight is one thing worth looking at.
type Sight struct {
	ID      string  `json:"id"` // Wikidata Q-id
	Name    string  `json:"name"`
	Kind    string  `json:"kind"` // castle, palace, lighthouse, mountain, ...
	Lat     float64 `json:"lat"`
	Lon     float64 `json:"lon"`
	Links   int     `json:"links"` // Wikipedia editions
	AlongNm float64 `json:"alongNm"`
	OffNm   float64 `json:"offNm"`
	Wiki    string  `json:"wiki,omitempty"` // https://www.wikidata.org/wiki/Q...
}

const (
	wikidataSparql = "https://query.wikidata.org/sparql"
	sightsChunkNm  = 40  // route pieces per query
	sightsMaxOffNm = 8.0 // listed up to this far off the route
	sightsMinLinks = 3   // at least this many Wikipedia articles
	sightsPerChunk = 60  // best per query
	sightsMax      = 40  // in the answer
	sightsMinGapNm = 0.5 // duplicates (same thing twice in Wikidata)
)

// Wikidata classes (instance of, P31) and the kind shown for them.
var sightClasses = map[string]string{
	"Q23413": "castle", "Q751876": "castle", "Q17715832": "castle", "Q109607": "ruins",
	"Q16560": "palace", "Q39715": "lighthouse", "Q8502": "mountain", "Q8072": "volcano",
	"Q12323": "dam", "Q34038": "waterfall", "Q23397": "lake", "Q1440300": "tower",
	"Q2977": "cathedral", "Q12280": "bridge", "Q23442": "island", "Q35666": "glacier",
	"Q45776": "fjord", "Q131681": "reservoir", "Q4022": "river", "Q570116": "attraction",
}

func sightsQuery(minLat, minLon, maxLat, maxLon float64) string {
	ids := make([]string, 0, len(sightClasses))
	for id := range sightClasses {
		if id != "Q4022" { // rivers are long lines - their coordinate means little
			ids = append(ids, "wd:"+id)
		}
	}
	sort.Strings(ids)
	return fmt.Sprintf(`SELECT ?item ?itemLabel ?coord ?type ?links WHERE {
  SERVICE wikibase:box { ?item wdt:P625 ?coord .
    bd:serviceParam wikibase:cornerSouthWest "Point(%.4f %.4f)"^^geo:wktLiteral .
    bd:serviceParam wikibase:cornerNorthEast "Point(%.4f %.4f)"^^geo:wktLiteral . }
  ?item wdt:P31 ?type . VALUES ?type { %s }
  ?item wikibase:sitelinks ?links . FILTER(?links >= %d)
  SERVICE wikibase:label { bd:serviceParam wikibase:language "en,de,fr,es,it,nl". }
} ORDER BY DESC(?links) LIMIT %d`, minLon, minLat, maxLon, maxLat, strings.Join(ids, " "), sightsMinLinks, sightsPerChunk)
}

type sparqlValue struct {
	Value string `json:"value"`
}

func parseSights(body []byte) ([]Sight, error) {
	var res struct {
		Results struct {
			Bindings []map[string]sparqlValue `json:"bindings"`
		} `json:"results"`
	}
	if err := json.Unmarshal(body, &res); err != nil {
		return nil, fmt.Errorf("unexpected Wikidata answer")
	}
	out := []Sight{}
	for _, b := range res.Results.Bindings {
		var lon, lat float64
		if n, _ := fmt.Sscanf(b["coord"].Value, "Point(%g %g)", &lon, &lat); n != 2 {
			continue
		}
		id := b["item"].Value[strings.LastIndex(b["item"].Value, "/")+1:]
		typ := b["type"].Value[strings.LastIndex(b["type"].Value, "/")+1:]
		links, _ := strconv.Atoi(b["links"].Value)
		name := b["itemLabel"].Value
		if name == "" || name == id {
			continue // no label in any of our languages
		}
		out = append(out, Sight{ID: id, Name: name, Kind: sightClasses[typ], Lat: lat, Lon: lon, Links: links,
			Wiki: "https://www.wikidata.org/wiki/" + id})
	}
	return out, nil
}

// sightsDiskTTL: sights don't move - answers are kept on disk (user cache
// dir) for a month, so a restarted companion doesn't ask Wikidata again.
const sightsDiskTTL = 30 * 24 * time.Hour

func sightsCachePath(u string) string {
	dir, err := userCacheDir()
	if err != nil {
		return ""
	}
	h := fnv.New64a()
	h.Write([]byte(u))
	return filepath.Join(dir, "xpmulticrew-companion", "sights", fmt.Sprintf("%016x.json", h.Sum64()))
}

func fetchSightsCached(u string) ([]byte, error) {
	path := sightsCachePath(u)
	if path != "" {
		if st, err := os.Stat(path); err == nil && time.Since(st.ModTime()) < sightsDiskTTL {
			if body, err := os.ReadFile(path); err == nil {
				return body, nil
			}
		}
	}
	body, err := fetchCachedFor(u, 24*time.Hour)
	if err != nil {
		return nil, err
	}
	if _, perr := parseSights(body); perr == nil && path != "" {
		_ = os.MkdirAll(filepath.Dir(path), 0755)
		tmp := path + ".tmp"
		if os.WriteFile(tmp, body, 0644) == nil {
			_ = os.Rename(tmp, path)
		}
	}
	return body, nil
}

// routeChunks splits the route into boxes of at most sightsChunkNm, padded
// by sightsMaxOffNm.
func routeChunks(wps []RouteWaypoint) [][4]float64 {
	out := [][4]float64{}
	for i := 1; i < len(wps); i++ {
		a, b := wps[i-1], wps[i]
		d := greatCircleNm(a.Lat, a.Lon, b.Lat, b.Lon)
		n := max(1, int(math.Ceil(d/sightsChunkNm)))
		for k := 0; k < n; k++ {
			f0, f1 := float64(k)/float64(n), float64(k+1)/float64(n)
			lat0, lon0 := a.Lat+(b.Lat-a.Lat)*f0, a.Lon+(b.Lon-a.Lon)*f0
			lat1, lon1 := a.Lat+(b.Lat-a.Lat)*f1, a.Lon+(b.Lon-a.Lon)*f1
			padLat := sightsMaxOffNm / 60
			padLon := padLat / math.Max(0.2, math.Cos((lat0+lat1)/2*math.Pi/180))
			out = append(out, [4]float64{math.Min(lat0, lat1) - padLat, math.Min(lon0, lon1) - padLon, math.Max(lat0, lat1) + padLat, math.Max(lon0, lon1) + padLon})
		}
	}
	return out
}

// routeSights returns the best-known sights near the route, along it.
func routeSights(r PlannedRoute) ([]Sight, error) {
	if len(r.Waypoints) < 2 {
		return nil, fmt.Errorf("plan a route first")
	}
	chunks := routeChunks(r.Waypoints)
	if len(chunks) > 15 {
		return nil, fmt.Errorf("route too long for the sights search (%d pieces) - split it into legs", len(chunks))
	}
	// A few pieces at a time (the query service allows 5 parallel queries
	// per client); answers are kept for a day - sights don't move.
	results := make([][]Sight, len(chunks))
	errs := make([]error, len(chunks))
	sem := make(chan struct{}, 3)
	var wg sync.WaitGroup
	for i, c := range chunks {
		wg.Add(1)
		go func(i int, c [4]float64) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			body, err := fetchSightsCached(wikidataSparql + "?format=json&query=" + url.QueryEscape(sightsQuery(c[0], c[1], c[2], c[3])))
			if err == nil {
				results[i], err = parseSights(body)
			}
			errs[i] = err
		}(i, c)
	}
	wg.Wait()
	all := []Sight{}
	var lastErr error
	for i := range chunks {
		all = append(all, results[i]...)
		if errs[i] != nil {
			lastErr = errs[i]
		}
	}
	if len(all) == 0 && lastErr != nil {
		return nil, lastErr
	}
	return pickSights(r.Waypoints, all), nil
}

// pickSights keeps the ones near the route (no duplicates), best known
// first, then orders them along the route.
func pickSights(wps []RouteWaypoint, all []Sight) []Sight {
	sort.SliceStable(all, func(i, j int) bool { return all[i].Links > all[j].Links })
	out := []Sight{}
	seen := map[string]bool{}
	for _, s := range all {
		if seen[s.ID] {
			continue
		}
		seen[s.ID] = true
		s.AlongNm, s.OffNm = alongRoute(wps, s.Lat, s.Lon)
		if s.OffNm > sightsMaxOffNm {
			continue
		}
		dup := false
		for _, o := range out {
			if greatCircleNm(o.Lat, o.Lon, s.Lat, s.Lon) < sightsMinGapNm {
				dup = true
				break
			}
		}
		if dup {
			continue
		}
		s.AlongNm, s.OffNm = math.Round(s.AlongNm*10)/10, math.Round(s.OffNm*10)/10
		out = append(out, s)
		if len(out) >= sightsMax {
			break
		}
	}
	sort.SliceStable(out, func(i, j int) bool { return out[i].AlongNm < out[j].AlongNm })
	return out
}

// tourLegSights: the top sights along each leg of a tour (straight line
// from stop to stop - the real route is planned on the evening).
func tourLegSights(t Tour) ([][]Sight, error) {
	out := make([][]Sight, len(t.Legs))
	lat, lon := t.FromLat, t.FromLon
	var lastErr error
	found := 0
	for i, l := range t.Legs {
		wps := []RouteWaypoint{{Kind: "APT", Ident: l.From, Lat: lat, Lon: lon}, {Kind: "APT", Ident: l.To, Lat: l.Lat, Lon: l.Lon}}
		lat, lon = l.Lat, l.Lon
		all, err := routeSights(PlannedRoute{Waypoints: wps})
		if err != nil {
			lastErr = err
			out[i] = []Sight{}
			continue
		}
		near := []Sight{}
		for _, s := range all {
			if s.OffNm <= 5 {
				near = append(near, s)
			}
		}
		sort.SliceStable(near, func(a, b int) bool { return near[a].Links > near[b].Links })
		if len(near) > 3 {
			near = near[:3]
		}
		sort.SliceStable(near, func(a, b int) bool { return near[a].AlongNm < near[b].AlongNm })
		out[i] = near
		found += len(near)
	}
	if found == 0 && lastErr != nil {
		return nil, lastErr
	}
	return out, nil
}
