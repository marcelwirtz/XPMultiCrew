package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Butter-Board: every landing the plugin reports (ours and the session's,
// control_listener.h's LANDINGS line - see the plugin's
// sync/touchdown_detector.h) is placed on a runway from apt.dat, scored,
// and shown on the Landings page. Our own landings are also kept on disk
// as a personal landing log.

// Landing is one rated landing.
type Landing struct {
	Key        string  `json:"key"` // "<sender_id>:<id>", unique
	Own        bool    `json:"own"`
	SenderID   uint32  `json:"senderId"`
	Time       int64   `json:"time"` // unix seconds, from the lander's clock
	Lat        float64 `json:"lat"`
	Lon        float64 `json:"lon"`
	Heading    float64 `json:"heading"`
	VsFpm      float64 `json:"vsFpm"`
	PeakG      float64 `json:"peakG"`
	GsKt       float64 `json:"gsKt"`
	DriftDeg   float64 `json:"driftDeg"`
	Bounces    int     `json:"bounces"`
	FlareM     float64 `json:"flareM"` // 50 ft -> touchdown, -1 unknown
	TouchAndGo bool    `json:"touchAndGo"`
	Icao       string  `json:"icao"`
	Callsign   string  `json:"callsign"`

	// Filled in by rateLanding.
	Airport       string   `json:"airport,omitempty"`
	Runway        string   `json:"runway,omitempty"`
	RunwayLengthM float64  `json:"runwayLengthM,omitempty"`
	RunwayWidthM  float64  `json:"runwayWidthM,omitempty"`
	PastThrM      *float64 `json:"pastThrM,omitempty"`    // touchdown distance past the landing threshold
	CenterlineM   *float64 `json:"centerlineM,omitempty"` // offset from the centerline, positive = right
	Score         int      `json:"score"`
	Verdict       string   `json:"verdict"`
	Notes         []string `json:"notes"`

	// Our own landings only: the Approach Coach's verdict on the approach
	// that ended here (nil if the coach was off or the approach started
	// below its arm height).
	Approach *ApproachRating `json:"approach,omitempty"`
}

// parseLandings decodes LANDINGS "<entry>;..." (see control_listener.h).
// Malformed entries are skipped.
func parseLandings(value string) []Landing {
	out := []Landing{}
	if strings.TrimSpace(value) == "" {
		return out
	}
	for _, entry := range strings.Split(strings.TrimSpace(value), ";") {
		f := strings.Split(entry, ":")
		if len(f) != 14 {
			continue
		}
		sender, err1 := strconv.ParseUint(f[0], 10, 32)
		id, err2 := strconv.ParseInt(f[1], 10, 64)
		if err1 != nil || err2 != nil {
			continue
		}
		var v [9]float64
		ok := true
		for i := 0; i < 9; i++ {
			x, err := strconv.ParseFloat(f[i+2], 64)
			if err != nil || math.IsNaN(x) || math.IsInf(x, 0) {
				ok = false
				break
			}
			v[i] = x
		}
		if !ok {
			continue
		}
		clean := func(s string) string {
			if s == "-" {
				return ""
			}
			return s
		}
		out = append(out, Landing{
			Key: f[0] + ":" + f[1], Own: sender == 0, SenderID: uint32(sender), Time: id,
			Lat: v[0], Lon: v[1], Heading: v[2], VsFpm: v[3], PeakG: v[4], GsKt: v[5], DriftDeg: v[6],
			Bounces: int(v[7]), FlareM: v[8], TouchAndGo: f[11] == "1", Icao: clean(f[12]), Callsign: clean(f[13]),
		})
	}
	return out
}

// --- Geometry ---------------------------------------------------------------

const earthRadiusM = 6371008.8

func toRad(d float64) float64 { return d * math.Pi / 180 }

// localXY projects (lat, lon) to metres east/north of (lat0, lon0) - plenty
// accurate over a runway's few kilometres.
func localXY(lat0, lon0, lat, lon float64) (x, y float64) {
	x = toRad(lon-lon0) * math.Cos(toRad(lat0)) * earthRadiusM
	y = toRad(lat-lat0) * earthRadiusM
	return
}

func angleDiff(a, b float64) float64 {
	d := math.Mod(a-b+540, 360) - 180
	return math.Abs(d)
}

// placeOnRunway finds the runway the aircraft touched down on: the
// touchdown point inside (or just short of / beside) the pavement,
// aligned with the landing heading. Returns false if none fits.
func placeOnRunway(l *Landing, runways []RunwayGeometry) bool {
	bestScore := math.Inf(1)
	found := false
	for _, r := range runways {
		// Cheap reject first: more than ~6 km from the first end.
		if math.Abs(r.Ends[0].Lat-l.Lat) > 0.06 || math.Abs(r.Ends[0].Lon-l.Lon) > 0.1 {
			continue
		}
		for i := 0; i < 2; i++ {
			from, to := r.Ends[i], r.Ends[1-i]
			ex, ey := localXY(from.Lat, from.Lon, to.Lat, to.Lon)
			length := math.Hypot(ex, ey)
			if length < 50 {
				continue
			}
			course := math.Mod(math.Atan2(ex, ey)*180/math.Pi+360, 360)
			if angleDiff(course, l.Heading) > 30 {
				continue
			}
			px, py := localXY(from.Lat, from.Lon, l.Lat, l.Lon)
			ux, uy := ex/length, ey/length
			along := px*ux + py*uy
			cross := px*uy - py*ux // positive = right of the centerline, looking along the landing direction
			halfWidth := math.Max(r.WidthM/2, 10)
			if along < -300 || along > length+100 || math.Abs(cross) > halfWidth+40 {
				continue
			}
			score := math.Abs(cross) + angleDiff(course, l.Heading)*5
			if score >= bestScore {
				continue
			}
			bestScore = score
			found = true
			past := along - from.DisplacedM
			cl := cross
			l.Airport = r.Airport
			l.Runway = from.Name
			l.RunwayLengthM = math.Round(length)
			l.RunwayWidthM = r.WidthM
			l.PastThrM = &past
			l.CenterlineM = &cl
		}
	}
	return found
}

// --- Scoring ----------------------------------------------------------------

func clamp(v, lo, hi float64) float64 { return math.Max(lo, math.Min(hi, v)) }

// verdictFor is the one-word rating, by sink rate (matches the plugin's own
// overlay fallback in plugin_main.cpp's LandingVerdict).
func verdictFor(fpm float64) string {
	fpm = math.Abs(fpm)
	switch {
	case fpm < 60:
		return "Butter!"
	case fpm < 180:
		return "Smooth"
	case fpm < 300:
		return "Good"
	case fpm < 500:
		return "Firm"
	case fpm < 700:
		return "Hard"
	}
	return "Ouch"
}

// rateLanding places the landing on a runway and scores it 0-100: sink
// rate is what counts most, then G, bounces, where on the runway and how
// straight. Notes explain every deduction.
func rateLanding(l *Landing, runways []RunwayGeometry) {
	placeOnRunway(l, runways)
	score := 100.0
	l.Notes = []string{}
	deduct := func(points float64, note string) {
		if points >= 0.5 {
			score -= points
			l.Notes = append(l.Notes, fmt.Sprintf("-%.0f %s", points, note))
		}
	}
	fpm := math.Abs(l.VsFpm)
	deduct(clamp((fpm-60)/6, 0, 60), "sink rate")
	deduct(clamp((l.PeakG-1.25)*40, 0, 20), "G load")
	deduct(clamp(float64(l.Bounces)*10, 0, 20), "bounced")
	if l.CenterlineM != nil {
		deduct(clamp((math.Abs(*l.CenterlineM)-2)*1.5, 0, 15), "off the centerline")
	}
	if l.PastThrM != nil {
		past := *l.PastThrM
		switch {
		case past < 0:
			deduct(25, "touched down before the threshold")
		case past > 600:
			deduct(clamp((past-600)/40, 0, 15), "long landing")
		}
	}
	deduct(clamp((math.Abs(l.DriftDeg)-3)*2, 0, 10), "drift / crab at touchdown")
	l.Score = int(math.Round(clamp(score, 0, 100)))
	l.Verdict = verdictFor(l.VsFpm)
	if l.Bounces > 0 && l.Verdict != "Hard" && l.Verdict != "Ouch" {
		l.Verdict = "Bounced"
	}
	if l.PastThrM != nil && *l.PastThrM < 0 {
		l.Verdict = "Short!"
	}
}

// overlayText is the one line shown in the sim after our own landing
// (SHOW_OVERLAY) - ASCII only, X-Plane's font has nothing else.
func overlayText(l Landing) string {
	parts := []string{fmt.Sprintf("%s %.0f fpm", map[bool]string{true: "TOUCH & GO", false: "TOUCHDOWN"}[l.TouchAndGo], l.VsFpm),
		fmt.Sprintf("%.2f G", l.PeakG)}
	if l.PastThrM != nil {
		parts = append(parts, fmt.Sprintf("%.0f m past thr %s", *l.PastThrM, l.Runway))
	}
	if l.CenterlineM != nil {
		side := "R"
		if *l.CenterlineM < 0 {
			side = "L"
		}
		parts = append(parts, fmt.Sprintf("%.1f m %s of CL", math.Abs(*l.CenterlineM), side))
	}
	if l.Bounces > 0 {
		parts = append(parts, fmt.Sprintf("%d bounce(s)", l.Bounces))
	}
	parts = append(parts, fmt.Sprintf("Score %d - %s", l.Score, l.Verdict))
	return strings.Join(parts, "  |  ")
}

// --- Landing book: session board + persisted own log ------------------------

const maxLandingLog = 500

// LandingBoard is what the Landings page shows.
type LandingBoard struct {
	Session []Landing `json:"session"` // this app run, everyone's, newest first
	Log     []Landing `json:"log"`     // our own, persisted, newest first
}

type landingBook struct {
	mu      sync.Mutex
	seen    map[string]bool
	session []Landing
	log     []Landing
	loaded  bool
	version int // bumped on every change, for the frontend to refetch
}

func landingLogPath() (string, error) {
	dir, err := userConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "xpmulticrew-companion", "landings.json"), nil
}

func (b *landingBook) loadLocked() {
	if b.loaded {
		return
	}
	b.loaded = true
	if b.seen == nil {
		b.seen = map[string]bool{}
	}
	path, err := landingLogPath()
	if err != nil {
		return
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return
	}
	var log []Landing
	if json.Unmarshal(raw, &log) == nil {
		b.log = log
		for _, l := range log {
			b.seen[l.Key] = true
		}
	}
}

func (b *landingBook) saveLocked() {
	path, err := landingLogPath()
	if err != nil {
		return
	}
	raw, err := json.MarshalIndent(b.log, "", " ")
	if err != nil {
		return
	}
	_ = os.MkdirAll(filepath.Dir(path), 0755)
	tmp := path + ".tmp"
	if os.WriteFile(tmp, raw, 0644) == nil {
		_ = os.Rename(tmp, path)
	}
}

// newLandings returns the reported landings not seen before and marks
// them seen. Landings older than a day (still in the plugin's list after
// a restart of this app) that aren't in the log are ignored for the board.
func (b *landingBook) newLandings(reported []Landing) []Landing {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.loadLocked()
	out := []Landing{}
	for _, l := range reported {
		if b.seen[l.Key] {
			continue
		}
		b.seen[l.Key] = true
		out = append(out, l)
	}
	return out
}

// add records a rated landing: on the session board, and in the log if
// it's ours.
func (b *landingBook) add(l Landing) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.loadLocked()
	b.session = append([]Landing{l}, b.session...)
	if l.Own {
		b.log = append([]Landing{l}, b.log...)
		sort.SliceStable(b.log, func(i, j int) bool { return b.log[i].Time > b.log[j].Time })
		if len(b.log) > maxLandingLog {
			b.log = b.log[:maxLandingLog]
		}
		b.saveLocked()
	}
	b.version++
}

func (b *landingBook) board() LandingBoard {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.loadLocked()
	return LandingBoard{Session: append([]Landing{}, b.session...), Log: append([]Landing{}, b.log...)}
}

func (b *landingBook) currentVersion() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.version
}

func (b *landingBook) deleteFromLog(key string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.loadLocked()
	out := b.log[:0]
	for _, l := range b.log {
		if l.Key != key {
			out = append(out, l)
		}
	}
	b.log = out
	b.saveLocked()
	b.version++
}

// isRecent says whether a landing just happened (worth an overlay in the
// sim), as opposed to one replayed from the plugin's list after this app
// was restarted.
func isRecent(l Landing, now time.Time) bool {
	return math.Abs(float64(now.Unix()-l.Time)) < 120
}
