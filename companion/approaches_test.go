package main

import "testing"

func TestParseApproachRatings(t *testing.T) {
	got := parseApproachRatings("1790000000:1:0:0:640:0;bad;1790000100:0:9:4:1350:0;1790000200:1:0:0:500:1")
	if len(got) != 3 {
		t.Fatalf("want 3 approaches, got %d", len(got))
	}
	if !got[0].StableAt500 || len(got[0].AtGate) != 0 || got[0].MaxSinkFpm != 640 {
		t.Errorf("first approach wrong: %+v", got[0])
	}
	// 9 = speed high + gear up, 4 = sink rate
	if got[1].StableAt500 || len(got[1].AtGate) != 2 || got[1].AtGate[0] != "speed high" || got[1].AtGate[1] != "gear up" {
		t.Errorf("gate deviations wrong: %+v", got[1].AtGate)
	}
	if len(got[1].WarningsBelow) != 1 || got[1].WarningsBelow[0] != "sink rate" {
		t.Errorf("warnings wrong: %+v", got[1].WarningsBelow)
	}
	if !got[2].GoAround {
		t.Errorf("go-around flag lost")
	}
	if len(parseApproachRatings("")) != 0 {
		t.Errorf("empty line should give no approaches")
	}
}

func TestApproachForLanding(t *testing.T) {
	approaches := []ApproachRating{
		{Time: 1000, StableAt500: true},
		{Time: 1500, GoAround: true},
		{Time: 1600, StableAt500: false},
	}
	if a := approachForLanding(Landing{Time: 1603}, approaches); a == nil || a.Time != 1600 {
		t.Errorf("want the approach ending 3 s before the landing, got %+v", a)
	}
	if a := approachForLanding(Landing{Time: 1502}, approaches); a != nil {
		t.Errorf("a go-around must not be attached to a landing, got %+v", a)
	}
	if a := approachForLanding(Landing{Time: 5000}, approaches); a != nil {
		t.Errorf("no approach near this landing, got %+v", a)
	}
}
