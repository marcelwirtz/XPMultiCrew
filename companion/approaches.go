package main

import (
	"math"
	"strconv"
	"strings"
)

// Approach Coach results (the plugin's sync/approach_coach.h): one entry
// per approach that passed the 500 ft gate, attached to the Butter-Board
// landing it ended in.

// ApproachRating is one rated approach.
type ApproachRating struct {
	Time          int64    `json:"time"` // unix seconds, when the approach ended (touchdown / go-around)
	StableAt500   bool     `json:"stableAt500"`
	AtGate        []string `json:"atGate"`        // what was off at 500 ft
	WarningsBelow []string `json:"warningsBelow"` // warnings called out below 500 ft
	MaxSinkFpm    float64  `json:"maxSinkFpm"`    // below 500 ft
	GoAround      bool     `json:"goAround"`
}

// Same order as the plugin's ApproachDeviation bits.
var approachDeviationNames = []string{
	"speed high", "speed low", "sink rate", "gear up", "bank angle", "glideslope", "localizer",
}

func approachDeviations(bits uint64) []string {
	out := []string{}
	for i, name := range approachDeviationNames {
		if bits&(1<<uint(i)) != 0 {
			out = append(out, name)
		}
	}
	return out
}

// parseApproachRatings decodes APPROACHES "<entry>;..." (see control_listener.h).
// Malformed entries are skipped.
func parseApproachRatings(value string) []ApproachRating {
	out := []ApproachRating{}
	if strings.TrimSpace(value) == "" {
		return out
	}
	for _, entry := range strings.Split(strings.TrimSpace(value), ";") {
		f := strings.Split(entry, ":")
		if len(f) != 6 {
			continue
		}
		id, err1 := strconv.ParseInt(f[0], 10, 64)
		gate, err2 := strconv.ParseUint(f[2], 10, 32)
		warn, err3 := strconv.ParseUint(f[3], 10, 32)
		sink, err4 := strconv.ParseFloat(f[4], 64)
		if err1 != nil || err2 != nil || err3 != nil || err4 != nil || math.IsNaN(sink) || math.IsInf(sink, 0) {
			continue
		}
		out = append(out, ApproachRating{
			Time: id, StableAt500: f[1] == "1", AtGate: approachDeviations(gate),
			WarningsBelow: approachDeviations(warn), MaxSinkFpm: sink, GoAround: f[5] == "1",
		})
	}
	return out
}

// approachForLanding finds the approach that ended in this landing: the
// plugin closes it at touchdown, the landing itself is reported a few
// seconds later once the bounces are over.
func approachForLanding(l Landing, approaches []ApproachRating) *ApproachRating {
	for i := len(approaches) - 1; i >= 0; i-- {
		a := approaches[i]
		if !a.GoAround && a.Time <= l.Time+5 && a.Time >= l.Time-60 {
			return &a
		}
	}
	return nil
}
