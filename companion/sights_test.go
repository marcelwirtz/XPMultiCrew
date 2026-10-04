package main

import (
	"strings"
	"testing"
)

func TestRouteSights(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	answer := `{"results":{"bindings":[
	 {"item":{"value":"http://www.wikidata.org/entity/Q1"},"itemLabel":{"value":"Big Castle"},"coord":{"value":"Point(8.5 50.21)"},"type":{"value":"http://www.wikidata.org/entity/Q23413"},"links":{"value":"12"}},
	 {"item":{"value":"http://www.wikidata.org/entity/Q2"},"itemLabel":{"value":"Same castle again"},"coord":{"value":"Point(8.501 50.211)"},"type":{"value":"http://www.wikidata.org/entity/Q23413"},"links":{"value":"4"}},
	 {"item":{"value":"http://www.wikidata.org/entity/Q3"},"itemLabel":{"value":"Lake"},"coord":{"value":"Point(8.2 50.25)"},"type":{"value":"http://www.wikidata.org/entity/Q23397"},"links":{"value":"5"}},
	 {"item":{"value":"http://www.wikidata.org/entity/Q4"},"itemLabel":{"value":"Far peak"},"coord":{"value":"Point(8.5 51.5)"},"type":{"value":"http://www.wikidata.org/entity/Q8502"},"links":{"value":"30"}},
	 {"item":{"value":"http://www.wikidata.org/entity/Q5"},"itemLabel":{"value":"Q5"},"coord":{"value":"Point(8.6 50.2)"},"type":{"value":"http://www.wikidata.org/entity/Q8502"},"links":{"value":"9"}}
	]}}`
	calls := 0
	stubFetch(t, map[string]string{wikidataSparql: answer})
	old := fetchURL
	fetchURL = func(u string) ([]byte, error) { calls++; return old(u) }
	r := PlannedRoute{Waypoints: []RouteWaypoint{{Kind: "APT", Ident: "EAAA", Lat: 50.2, Lon: 8.0}, {Kind: "APT", Ident: "EBBB", Lat: 50.2, Lon: 9.0}}}
	sights, err := routeSights(r)
	if err != nil {
		t.Fatal(err)
	}
	names := []string{}
	for _, s := range sights {
		names = append(names, s.Name)
	}
	if strings.Join(names, ",") != "Lake,Big Castle" || sights[1].Kind != "castle" || sights[1].Links != 12 {
		t.Fatalf("expected the lake, then the castle (along the route), no far peak, no duplicate, no unlabelled: %+v", sights)
	}
	if calls != 1 {
		t.Fatalf("a 38 NM route is one query, got %d", calls)
	}
	// After a restart (memory cache gone) the answer comes from disk.
	wxCacheMu.Lock()
	wxCache = map[string]wxCacheEntry{}
	wxCacheMu.Unlock()
	if again, err := routeSights(r); err != nil || len(again) != 2 || calls != 1 {
		t.Fatalf("expected the disk cache: %v %d sights, %d calls", err, len(again), calls)
	}
	if !strings.Contains(sightsQuery(50, 8, 51, 9), `"Point(8.0000 50.0000)"`) {
		t.Fatal("box corners are lon lat")
	}
}

func TestRouteChunks(t *testing.T) {
	wps := []RouteWaypoint{{Lat: 50, Lon: 8}, {Lat: 51.5, Lon: 8}} // 90 NM -> 3 pieces
	if n := len(routeChunks(wps)); n != 3 {
		t.Fatalf("expected 3 chunks, got %d", n)
	}
}

func TestTourLegSights(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	answer := `{"results":{"bindings":[
	 {"item":{"value":"http://www.wikidata.org/entity/Q1"},"itemLabel":{"value":"A"},"coord":{"value":"Point(8.3 50.2)"},"type":{"value":"http://www.wikidata.org/entity/Q23413"},"links":{"value":"5"}},
	 {"item":{"value":"http://www.wikidata.org/entity/Q2"},"itemLabel":{"value":"B"},"coord":{"value":"Point(8.6 50.2)"},"type":{"value":"http://www.wikidata.org/entity/Q23413"},"links":{"value":"9"}},
	 {"item":{"value":"http://www.wikidata.org/entity/Q3"},"itemLabel":{"value":"C"},"coord":{"value":"Point(8.4 50.21)"},"type":{"value":"http://www.wikidata.org/entity/Q8502"},"links":{"value":"7"}},
	 {"item":{"value":"http://www.wikidata.org/entity/Q4"},"itemLabel":{"value":"D"},"coord":{"value":"Point(8.5 50.2)"},"type":{"value":"http://www.wikidata.org/entity/Q8502"},"links":{"value":"3"}},
	 {"item":{"value":"http://www.wikidata.org/entity/Q5"},"itemLabel":{"value":"Off"},"coord":{"value":"Point(8.5 50.3)"},"type":{"value":"http://www.wikidata.org/entity/Q8502"},"links":{"value":"50"}}
	]}}`
	stubFetch(t, map[string]string{wikidataSparql: answer})
	tour := Tour{FromLat: 50.2, FromLon: 8.0, Legs: []TourLeg{{From: "EAAA", To: "EBBB", Lat: 50.2, Lon: 9.0}}}
	got, err := tourLegSights(tour)
	if err != nil {
		t.Fatal(err)
	}
	names := []string{}
	for _, s := range got[0] {
		names = append(names, s.Name)
	}
	// Top 3 by fame within 5 NM (B 9, C 7, A 5 - not D, not the one 6 NM off), in flying order.
	if strings.Join(names, ",") != "A,C,B" {
		t.Fatalf("unexpected leg sights %v", names)
	}
}
