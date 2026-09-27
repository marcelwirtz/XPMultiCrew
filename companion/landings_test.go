package main

import (
	"math"
	"strings"
	"testing"
	"time"
)

var testRunways = []RunwayGeometry{{
	Airport: "EDDF", WidthM: 60,
	Ends: [2]RunwayEnd{
		{Name: "07C", Lat: 50.03253798, Lon: 8.53459410},
		{Name: "25C", Lat: 50.04499797, Lon: 8.58696597, DisplacedM: 200},
	},
}}

// pointFrom returns the position `along` metres down the runway from end
// `from` and `right` metres right of the centerline, plus the runway course.
func pointFrom(r RunwayGeometry, from int, along, right float64) (lat, lon, course float64) {
	a, b := r.Ends[from], r.Ends[1-from]
	ex, ey := localXY(a.Lat, a.Lon, b.Lat, b.Lon)
	l := math.Hypot(ex, ey)
	ux, uy := ex/l, ey/l
	x := ux*along + uy*right
	y := uy*along - ux*right
	lat = a.Lat + y/earthRadiusM*180/math.Pi
	lon = a.Lon + x/(earthRadiusM*math.Cos(toRad(a.Lat)))*180/math.Pi
	course = math.Mod(math.Atan2(ex, ey)*180/math.Pi+360, 360)
	return
}

func TestParseLandings(t *testing.T) {
	got := parseLandings("0:1790000000:50.1:8.5:70.2:-142:1.18:62:2.5:0:380:0:C172:DEABC;" +
		"12345:1790000100:50.2:8.6:250:-300:1.4:65:-1:2:-1:1:-:-;garbage;1:2:3")
	if len(got) != 2 {
		t.Fatalf("expected 2 landings, got %+v", got)
	}
	a := got[0]
	if !a.Own || a.Key != "0:1790000000" || a.VsFpm != -142 || a.PeakG != 1.18 || a.FlareM != 380 ||
		a.Icao != "C172" || a.Callsign != "DEABC" || a.TouchAndGo {
		t.Fatalf("own landing parsed wrong: %+v", a)
	}
	b := got[1]
	if b.Own || b.SenderID != 12345 || b.Bounces != 2 || !b.TouchAndGo || b.Icao != "" || b.Callsign != "" {
		t.Fatalf("peer landing parsed wrong: %+v", b)
	}
	if len(parseLandings("")) != 0 {
		t.Fatal("empty value should give no landings")
	}
}

func TestPlaceOnRunway(t *testing.T) {
	lat, lon, course := pointFrom(testRunways[0], 0, 400, 3)
	l := Landing{Lat: lat, Lon: lon, Heading: course + 2}
	if !placeOnRunway(&l, testRunways) {
		t.Fatal("touchdown on 07C not found")
	}
	if l.Airport != "EDDF" || l.Runway != "07C" || math.Abs(*l.PastThrM-400) > 1 || math.Abs(*l.CenterlineM-3) > 0.5 {
		t.Fatalf("07C placement wrong: %s %s past %.1f cl %.1f", l.Airport, l.Runway, *l.PastThrM, *l.CenterlineM)
	}
	if l.RunwayLengthM < 3900 || l.RunwayLengthM > 4100 {
		t.Fatalf("runway length wrong: %v", l.RunwayLengthM)
	}

	// The other direction, with its displaced threshold, left of the centerline.
	lat, lon, course = pointFrom(testRunways[0], 1, 500, -4)
	l = Landing{Lat: lat, Lon: lon, Heading: course}
	if !placeOnRunway(&l, testRunways) || l.Runway != "25C" || math.Abs(*l.PastThrM-300) > 1 ||
		math.Abs(*l.CenterlineM+4) > 0.5 {
		t.Fatalf("25C placement wrong: %+v", l)
	}

	// Crossing the runway at 90 degrees, or on a taxiway 150 m beside it: no runway.
	lat, lon, course = pointFrom(testRunways[0], 0, 400, 0)
	l = Landing{Lat: lat, Lon: lon, Heading: course + 90}
	if placeOnRunway(&l, testRunways) {
		t.Fatal("perpendicular touchdown should not match")
	}
	lat, lon, course = pointFrom(testRunways[0], 0, 400, 150)
	l = Landing{Lat: lat, Lon: lon, Heading: course}
	if placeOnRunway(&l, testRunways) {
		t.Fatal("touchdown beside the runway should not match")
	}
}

func TestRateLanding(t *testing.T) {
	lat, lon, course := pointFrom(testRunways[0], 0, 350, 1)
	butter := Landing{Lat: lat, Lon: lon, Heading: course, VsFpm: -55, PeakG: 1.05, DriftDeg: 1}
	rateLanding(&butter, testRunways)
	if butter.Score != 100 || butter.Verdict != "Butter!" || len(butter.Notes) != 0 {
		t.Fatalf("perfect landing rated wrong: %+v", butter)
	}

	hard := Landing{Lat: lat, Lon: lon, Heading: course, VsFpm: -650, PeakG: 2.1, Bounces: 1, DriftDeg: 8}
	rateLanding(&hard, testRunways)
	if hard.Score > 20 || hard.Verdict != "Hard" || len(hard.Notes) < 3 {
		t.Fatalf("hard landing rated wrong: %+v", hard)
	}

	lat, lon, course = pointFrom(testRunways[0], 0, -20, 0)
	short := Landing{Lat: lat, Lon: lon, Heading: course, VsFpm: -120, PeakG: 1.1}
	rateLanding(&short, testRunways)
	if short.Verdict != "Short!" || short.Score > 80 {
		t.Fatalf("short landing rated wrong: %+v", short)
	}

	// No runway data: still rated by the numbers.
	grass := Landing{Lat: 10, Lon: 10, VsFpm: -150, PeakG: 1.2}
	rateLanding(&grass, nil)
	if grass.Runway != "" || grass.Score != 85 || grass.Verdict != "Smooth" {
		t.Fatalf("landing without runway rated wrong: %+v", grass)
	}

	text := overlayText(butter)
	if !strings.Contains(text, "-55 fpm") || !strings.Contains(text, "past thr 07C") || !strings.Contains(text, "Score 100") {
		t.Fatalf("overlay text: %q", text)
	}
	for _, c := range text {
		if c < 0x20 || c > 0x7e {
			t.Fatalf("overlay text must be plain ASCII: %q", text)
		}
	}
}

func TestLandingBook(t *testing.T) {
	useTempConfigDir(t)
	var book landingBook
	reported := parseLandings("0:1790000000:50.1:8.5:70:-142:1.18:62:2.5:0:380:0:C172:DEABC;" +
		"77:1790000050:50.1:8.5:70:-90:1.1:60:0:0:300:0:PA28:DEFGH")
	fresh := book.newLandings(reported)
	if len(fresh) != 2 || len(book.newLandings(reported)) != 0 {
		t.Fatal("each landing must be new exactly once")
	}
	v0 := book.currentVersion()
	for _, l := range fresh {
		rateLanding(&l, nil)
		book.add(l)
	}
	if book.currentVersion() == v0 {
		t.Fatal("version must change")
	}
	board := book.board()
	if len(board.Session) != 2 || len(board.Log) != 1 || board.Log[0].Key != "0:1790000000" {
		t.Fatalf("board wrong: %+v", board)
	}

	// The log survives a restart, and the plugin re-reporting the same
	// landing afterwards doesn't add it twice.
	var again landingBook
	if len(again.board().Log) != 1 {
		t.Fatal("log not persisted")
	}
	if n := len(again.newLandings(reported)); n != 1 {
		t.Fatalf("only the peer's landing is new after a restart, got %d", n)
	}
	again.deleteFromLog("0:1790000000")
	var third landingBook
	if len(third.board().Log) != 0 {
		t.Fatal("delete not persisted")
	}
}

func TestIsRecent(t *testing.T) {
	now := time.Unix(1790000100, 0)
	if !isRecent(Landing{Time: 1790000090}, now) || isRecent(Landing{Time: 1789990000}, now) {
		t.Fatal("isRecent wrong")
	}
}
