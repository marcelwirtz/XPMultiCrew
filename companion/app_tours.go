package main

import (
	"errors"
)

// --- Tours (tours.go) ---

// tourInputs loads what tour generation and progress need.
func (a *App) tourInputs() (string, AirportData, []LogbookEntry, error) {
	root := loadConfig().XPlanePath
	if root == "" {
		return "", AirportData{}, nil, errors.New("choose your X-Plane folder first (Setup)")
	}
	airports, err := loadAirports(root)
	if err != nil {
		return "", AirportData{}, nil, err
	}
	flights, _ := a.flights.list()
	return root, airports, loadLogbookEntries(flights), nil
}

func visitedFrom(entries []LogbookEntry) map[string]bool {
	visited := map[string]bool{}
	for _, e := range entries {
		for _, id := range []string{e.Departure, e.Arrival} {
			if id != "" {
				visited[id] = true
			}
		}
	}
	return visited
}

// GenerateTour makes a tour from the form (not saved yet).
func (a *App) GenerateTour(req TourRequest) (Tour, error) {
	root, airports, entries, err := a.tourInputs()
	if err != nil {
		return Tour{}, err
	}
	t, err := generateTour(airports, customSceneryAirports(root), visitedFrom(entries), req, nowUTC())
	if err != nil {
		return Tour{}, err
	}
	applyProgress(&t, nil)
	return t, nil
}

// SaveTour stores a tour (new or changed: notes, ticks, re-rolled legs).
func (a *App) SaveTour(t Tour) (Tour, error) {
	if len(t.Legs) == 0 {
		return Tour{}, errors.New("a tour needs at least one leg")
	}
	if t.Created == 0 {
		t.Created = nowUTC().Unix()
	}
	if err := saveTour(&t); err != nil {
		return Tour{}, err
	}
	flights, _ := a.flights.list()
	applyProgress(&t, loadLogbookEntries(flights))
	return t, nil
}

// ListTours returns the saved tours with their progress from the logbook.
func (a *App) ListTours() ([]Tour, error) {
	tours, err := listTours()
	if err != nil {
		return nil, err
	}
	flights, _ := a.flights.list()
	entries := loadLogbookEntries(flights)
	for i := range tours {
		applyProgress(&tours[i], entries)
	}
	return tours, nil
}

// DeleteTour removes a saved tour.
func (a *App) DeleteTour(id string) error {
	return deleteTour(id)
}

// TourShareCode returns a code a buddy can paste into ImportTourCode.
func (a *App) TourShareCode(t Tour) (string, error) {
	return tourCode(t)
}

// ImportTourCode saves a tour from a buddy's code.
func (a *App) ImportTourCode(code string) (Tour, error) {
	t, err := parseTourCode(code, nowUTC())
	if err != nil {
		return Tour{}, err
	}
	return a.SaveTour(t)
}

// TourLegSights returns the best-known sights near each leg's straight
// line (up to 3 per leg, within 5 NM) for the Tours page.
func (a *App) TourLegSights(t Tour) ([][]Sight, error) {
	return tourLegSights(t)
}
