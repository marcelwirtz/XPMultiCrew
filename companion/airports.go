package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
)

// Airports and runways for the map page, read from the user's own X-Plane
// installation (Global Scenery/Global Airports/Earth nav data/apt.dat) at
// runtime - nothing from it is bundled or redistributed with this app.
// Parsing the ~400 MB file takes a few seconds, so the result is cached in
// the user cache dir, keyed by the file's size and modification time.

// AirportData is sent to the frontend in a compact array form (tens of
// thousands of entries - field names would multiply the payload).
type AirportData struct {
	// [ident, name, lat, lon, kind] - kind 1 = land, 16 = seaplane base, 17 = heliport
	Airports [][]interface{} `json:"airports"`
	// [lat1, lon1, lat2, lon2] per land runway
	Runways [][4]float64 `json:"runways"`
	// Per-airport details for the map's info popup - kept on the Go side
	// (GetAirportInfo), not sent with the bulk list.
	Details map[string]AirportDetail `json:"-"`
}

// AirportDetail is what the map shows when an airport is clicked.
type AirportDetail struct {
	ElevationFt int         `json:"elevationFt"`
	Runways     []string    `json:"runways"`
	Frequencies []Frequency `json:"frequencies"`
}

// Frequency is one apt.dat 50-56 / 1050-1056 row.
type Frequency struct {
	Type string `json:"type"` // ATIS, UNICOM, DEL, GND, TWR, APP, DEP
	MHz  string `json:"mhz"`
	Name string `json:"name"`
}

const airportCacheVersion = 2

type airportCacheFile struct {
	Version int                      `json:"version"`
	Key     string                   `json:"key"`
	Data    AirportData              `json:"data"`
	Details map[string]AirportDetail `json:"details"`
}

func aptDatPath(xplaneRoot string) string {
	return filepath.Join(xplaneRoot, "Global Scenery", "Global Airports", "Earth nav data", "apt.dat")
}

// userCacheDir is a var so tests can redirect it (same pattern as config.go's
// userConfigDir).
var userCacheDir = os.UserCacheDir

var airportLoadMu sync.Mutex // one parse at a time, a second caller waits and then hits the cache

func loadAirports(xplaneRoot string) (AirportData, error) {
	path := aptDatPath(xplaneRoot)
	st, err := os.Stat(path)
	if err != nil {
		return AirportData{}, fmt.Errorf("no airport data found in this X-Plane installation (%s)", path)
	}
	key := fmt.Sprintf("%s|%d|%d", path, st.Size(), st.ModTime().UnixNano())

	airportLoadMu.Lock()
	defer airportLoadMu.Unlock()

	cachePath := ""
	if dir, err := userCacheDir(); err == nil {
		cachePath = filepath.Join(dir, "xpmulticrew-companion", "airports.json")
		if raw, err := os.ReadFile(cachePath); err == nil {
			var cached airportCacheFile
			if json.Unmarshal(raw, &cached) == nil && cached.Version == airportCacheVersion && cached.Key == key {
				cached.Data.Details = cached.Details
				return cached.Data, nil
			}
		}
	}

	f, err := os.Open(path)
	if err != nil {
		return AirportData{}, err
	}
	defer f.Close()
	data, err := parseAptDat(f)
	if err != nil {
		return AirportData{}, err
	}

	if cachePath != "" {
		if raw, err := json.Marshal(airportCacheFile{Version: airportCacheVersion, Key: key, Data: data, Details: data.Details}); err == nil {
			_ = os.MkdirAll(filepath.Dir(cachePath), 0755)
			tmp := cachePath + ".tmp"
			if os.WriteFile(tmp, raw, 0644) == nil {
				_ = os.Rename(tmp, cachePath)
			}
		}
	}
	return data, nil
}

// parseAptDat extracts one position per airport and the land runways.
// Row codes (apt.dat 1200+ spec): 1/16/17 airport/seaplane base/heliport
// header, 1302 datum_lat/datum_lon metadata (the airport reference point,
// when present), 100 land runway, 101 water runway, 102 helipad. Airports
// without a datum get the midpoint of their first runway/helipad.
func parseAptDat(r io.Reader) (AirportData, error) {
	data := AirportData{Airports: [][]interface{}{}, Runways: [][4]float64{}, Details: map[string]AirportDetail{}}
	freqTypes := map[int]string{50: "ATIS", 51: "UNICOM", 52: "DEL", 53: "GND", 54: "TWR", 55: "APP", 56: "DEP"}

	type current struct {
		ident, name        string
		kind               int
		detail             AirportDetail
		datumLat, datumLon float64
		hasDatumLat        bool
		hasDatumLon        bool
		fallbackLat        float64
		fallbackLon        float64
		hasFallback        bool
	}
	var cur *current
	flush := func() {
		if cur == nil {
			return
		}
		lat, lon, ok := cur.fallbackLat, cur.fallbackLon, cur.hasFallback
		if cur.hasDatumLat && cur.hasDatumLon {
			lat, lon, ok = cur.datumLat, cur.datumLon, true
		}
		if ok {
			data.Airports = append(data.Airports, []interface{}{cur.ident, cur.name, round6(lat), round6(lon), cur.kind})
			data.Details[cur.ident] = cur.detail
		}
		cur = nil
	}
	num := func(s string) (float64, bool) {
		v, err := strconv.ParseFloat(s, 64)
		return v, err == nil
	}

	scanner := bufio.NewScanner(r)
	scanner.Buffer(make([]byte, 64*1024), 1024*1024)
	for scanner.Scan() {
		line := scanner.Text()
		if len(line) < 2 {
			continue
		}
		// Cheap prefix filter first - the vast majority of lines are
		// taxiway/pavement/lighting rows this doesn't care about.
		switch {
		case strings.HasPrefix(line, "1 "), strings.HasPrefix(line, "16 "), strings.HasPrefix(line, "17 "):
			f := strings.Fields(line)
			if len(f) < 5 {
				continue
			}
			flush()
			kind, _ := strconv.Atoi(f[0])
			elev, _ := strconv.Atoi(f[1])
			cur = &current{ident: f[4], name: strings.Join(f[5:], " "), kind: kind,
				detail: AirportDetail{ElevationFt: elev, Runways: []string{}, Frequencies: []Frequency{}}}
		case cur == nil:
			continue
		case strings.HasPrefix(line, "1302 datum_lat "):
			if v, ok := num(strings.TrimSpace(line[len("1302 datum_lat "):])); ok {
				cur.datumLat, cur.hasDatumLat = v, true
			}
		case strings.HasPrefix(line, "1302 datum_lon "):
			if v, ok := num(strings.TrimSpace(line[len("1302 datum_lon "):])); ok {
				cur.datumLon, cur.hasDatumLon = v, true
			}
		case strings.HasPrefix(line, "100 "):
			f := strings.Fields(line)
			if len(f) < 20 {
				continue
			}
			lat1, ok1 := num(f[9])
			lon1, ok2 := num(f[10])
			lat2, ok3 := num(f[18])
			lon2, ok4 := num(f[19])
			if !(ok1 && ok2 && ok3 && ok4) {
				continue
			}
			data.Runways = append(data.Runways, [4]float64{round6(lat1), round6(lon1), round6(lat2), round6(lon2)})
			cur.detail.Runways = append(cur.detail.Runways, f[8]+"/"+f[17])
			if !cur.hasFallback {
				cur.fallbackLat, cur.fallbackLon, cur.hasFallback = (lat1+lat2)/2, (lon1+lon2)/2, true
			}
		case strings.HasPrefix(line, "101 "):
			f := strings.Fields(line)
			if len(f) < 9 || cur.hasFallback {
				continue
			}
			lat1, ok1 := num(f[4])
			lon1, ok2 := num(f[5])
			lat2, ok3 := num(f[7])
			lon2, ok4 := num(f[8])
			if ok1 && ok2 && ok3 && ok4 {
				cur.fallbackLat, cur.fallbackLon, cur.hasFallback = (lat1+lat2)/2, (lon1+lon2)/2, true
			}
		case strings.HasPrefix(line, "102 "):
			f := strings.Fields(line)
			if len(f) < 4 || cur.hasFallback {
				continue
			}
			lat, ok1 := num(f[2])
			lon, ok2 := num(f[3])
			if ok1 && ok2 {
				cur.fallbackLat, cur.fallbackLon, cur.hasFallback = lat, lon, true
			}
		case strings.HasPrefix(line, "99"):
			flush()
		case line[0] == '5' || strings.HasPrefix(line, "105"):
			// 50-56: frequency in 10 kHz; 1050-1056: in kHz (8.33 spacing).
			f := strings.Fields(line)
			if len(f) < 2 {
				continue
			}
			code, err1 := strconv.Atoi(f[0])
			freq, err2 := strconv.Atoi(f[1])
			if err1 != nil || err2 != nil {
				continue
			}
			mhz := ""
			switch {
			case code >= 50 && code <= 56:
				mhz = fmt.Sprintf("%.2f", float64(freq)/100)
			case code >= 1050 && code <= 1056:
				code -= 1000
				mhz = fmt.Sprintf("%.3f", float64(freq)/1000)
			default:
				continue
			}
			cur.detail.Frequencies = append(cur.detail.Frequencies,
				Frequency{Type: freqTypes[code], MHz: mhz, Name: strings.Join(f[2:], " ")})
		}
	}
	flush()
	return data, scanner.Err()
}

func round6(v float64) float64 {
	return float64(int64(v*1e6+copysignHalf(v))) / 1e6
}

func copysignHalf(v float64) float64 {
	if v < 0 {
		return -0.5
	}
	return 0.5
}
