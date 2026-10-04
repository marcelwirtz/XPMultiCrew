package main

import (
	"errors"
	"fmt"
	"log"
	"math"
	"strings"
	"time"
)

// --- Butter-Board and Debrief (landings.go, flights.go) ----------------------

// runwayGeometry returns the runways from apt.dat, nil if unavailable.
func runwayGeometry() []RunwayGeometry {
	root := loadConfig().XPlanePath
	if root == "" {
		return nil
	}
	data, err := loadAirports(root)
	if err != nil {
		return nil
	}
	return data.RunwayGeometry
}

// nearestAirport returns the ident of the closest airport within 5 km, ""
// if none (or no airport data).
func nearestAirport(lat, lon float64) string {
	root := loadConfig().XPlanePath
	if root == "" {
		return ""
	}
	data, err := loadAirports(root)
	if err != nil {
		return ""
	}
	best, bestNm := "", 2.7
	for _, a := range data.Airports {
		if len(a) < 4 {
			continue
		}
		alat, ok1 := a[2].(float64)
		alon, ok2 := a[3].(float64)
		ident, ok3 := a[0].(string)
		if !ok1 || !ok2 || !ok3 || math.Abs(alat-lat) > 0.1 {
			continue
		}
		if d := nmBetween(lat, lon, alat, alon); d < bestNm {
			best, bestNm = ident, d
		}
	}
	return best
}

// processLandings rates the landings the plugin reported since the last
// tick. Rating needs the airport data, which can take seconds to load the
// first time - so it runs in the background.
func (a *App) processLandings() {
	fresh := a.landings.newLandings(a.plugin.Landings())
	if len(fresh) == 0 {
		return
	}
	approaches := a.plugin.Approaches()
	go func() {
		runways := runwayGeometry()
		for _, l := range fresh {
			l := l
			if !l.Own {
				// The callsign they sent may be empty (tail number not set);
				// the peer list knows what everyone sees.
				for _, p := range a.plugin.FormationPeers() {
					if p.ID == l.SenderID && l.Callsign == "" {
						l.Callsign = p.Callsign
					}
				}
			}
			rateLanding(&l, runways)
			if l.Own {
				l.Approach = approachForLanding(l, approaches)
			}
			if l.Own && isRecent(l, time.Now()) {
				_ = a.plugin.Send("SHOW_OVERLAY 12 " + overlayText(l))
			}
			if l.Own {
				a.recMu.Lock()
				a.recorder.addLanding(l)
				a.recMu.Unlock()
			}
			a.landings.add(l)
		}
	}()
}

// recordFlight feeds the flight recorder and saves a finished flight.
func (a *App) recordFlight(self *MapPosition, peers []MapPosition) *RecordingInfo {
	a.recMu.Lock()
	done := a.recorder.feed(time.Now(), self, peers, a.plugin.FormationPeers(), a.plugin.OwnIcao(),
		loadConfig().pluginPrefs().Callsign)
	info := a.recorder.recording()
	a.recMu.Unlock()
	if done != nil {
		go a.storeFlight(done)
	}
	return info
}

func (a *App) storeFlight(f *Flight) {
	if len(f.Track) > 0 {
		first, last := f.Track[0], f.Track[len(f.Track)-1]
		f.Departure = nearestAirport(first[1], first[2])
		f.Arrival = nearestAirport(last[1], last[2])
	}
	if err := saveFlight(f); err != nil {
		log.Printf("could not save flight %s: %v", f.ID, err)
		return
	}
	a.flights.forget(f.ID)
	a.recMu.Lock()
	a.flightsVersion++
	a.recMu.Unlock()
}

// shutdown saves a flight still being recorded when the app is closed.
func (a *App) shutdown() {
	a.recMu.Lock()
	f := a.recorder.finish()
	a.recMu.Unlock()
	if f != nil {
		a.storeFlight(f)
	}
}

// SetApproachCoach switches the plugin's Approach Coach on or off. The
// plugin saves the setting itself (it can also be switched from X-Plane's
// Plugins menu), so there's nothing to store here.
func (a *App) SetApproachCoach(enabled bool) error {
	if enabled {
		return a.plugin.Send("SET_APPROACH_COACH 1")
	}
	return a.plugin.Send("SET_APPROACH_COACH 0")
}

// GetLandingBoard returns the session's landings and our own landing log.
func (a *App) GetLandingBoard() LandingBoard {
	return a.landings.board()
}

// DeleteLanding removes one landing from our own log.
func (a *App) DeleteLanding(key string) {
	a.landings.deleteFromLog(key)
}

// GetLogbook returns the logbook built from the recorded flights - see
// logbook.go.
func (a *App) GetLogbook() (Logbook, error) {
	flights, err := a.flights.list()
	if err != nil {
		return Logbook{}, err
	}
	airports := AirportData{}
	if root := loadConfig().XPlanePath; root != "" {
		airports, _ = loadAirports(root)
	}
	return buildLogbook(loadLogbookEntries(flights), airports), nil
}

// ListFlights returns the recorded flights, newest first.
func (a *App) ListFlights() ([]FlightSummary, error) {
	return a.flights.list()
}

// LoadFlight returns one recorded flight; "current" is the one being
// recorded right now.
func (a *App) LoadFlight(id string) (*Flight, error) {
	if id == "current" {
		a.recMu.Lock()
		defer a.recMu.Unlock()
		if f := a.recorder.snapshot(); f != nil {
			return f, nil
		}
		return nil, errors.New("no flight is being recorded")
	}
	return loadFlight(id)
}

// DeleteFlight removes a recorded flight.
func (a *App) DeleteFlight(id string) error {
	a.flights.forget(id)
	if err := deleteFlight(id); err != nil {
		return err
	}
	a.recMu.Lock()
	a.flightsVersion++
	a.recMu.Unlock()
	return nil
}

// AddFlightEvent marks something on the current flight's timeline - the
// frontend calls it when a checklist is completed or a route waypoint
// is reached.
func (a *App) AddFlightEvent(kind, label string) error {
	switch kind {
	case "checklist", "waypoint":
	default:
		return fmt.Errorf("unknown event kind %q", kind)
	}
	label = strings.TrimSpace(label)
	if len(label) > 80 {
		label = label[:80]
	}
	a.recMu.Lock()
	a.recorder.addEvent(time.Now(), kind, label)
	a.recMu.Unlock()
	return nil
}
