package main

import (
	"math"
	"os"
	"strings"
	"testing"
)

func findSunclockLgolf(t *testing.T) string {
	t.Helper()
	for _, candidate := range []string{"sunclock.lgolf", "apps/sunclock/sunclock.lgolf"} {
		if _, err := os.Stat(candidate); err == nil {
			return candidate
		}
	}
	t.Fatal("unable to locate sunclock.lgolf")
	return ""
}

func TestGeneratedWorldMapMatchesSunclockLgolf(t *testing.T) {
	rows := generateWorldMap()
	if len(rows) != mapH {
		t.Fatalf("expected %d rows, got %d", mapH, len(rows))
	}
	for r, row := range rows {
		if len(row) != mapW {
			t.Fatalf("row %d: expected width %d, got %d", r, mapW, len(row))
		}
		for c := 0; c < len(row); c++ {
			if strings.IndexByte(cellSymbols, row[c]) < 0 {
				t.Fatalf("row %d col %d: unexpected symbol %q", r, c, row[c])
			}
		}
	}

	existing, err := extractExistingWorldMap(findSunclockLgolf(t))
	if err != nil {
		t.Fatalf("extractExistingWorldMap: %v", err)
	}
	generated := strings.Join(rows, "\n")
	if generated != existing {
		t.Fatal("generated 160x80 WORLD_MAP does not match sunclock.lgolf")
	}
}

func TestGeometryAndBiomeHelpers(t *testing.T) {
	square := []point{{-10, -10}, {10, -10}, {10, 10}, {-10, 10}}
	if !pointInPoly(point{0, 0}, square) {
		t.Fatal("expected (0,0) to be inside square")
	}
	if pointInPoly(point{25, 0}, square) {
		t.Fatal("expected (25,0) to be outside square")
	}

	if d := distToSegment(point{0, 3}, point{-5, 0}, point{5, 0}); math.Abs(d-3.0) > 1e-6 {
		t.Fatalf("expected perpendicular distance 3.0, got %v", d)
	}
	if d := distToSegment(point{7, 4}, point{3, 1}, point{3, 1}); math.Abs(d-5.0) > 1e-6 {
		t.Fatalf("expected degenerate segment distance 5.0, got %v", d)
	}

	if sdIn := signedDistPolys(point{0, 0}, [][]point{square}); sdIn <= 0 {
		t.Fatalf("expected positive interior signed distance, got %v", sdIn)
	}
	if sdOut := signedDistPolys(point{20, 0}, [][]point{square}); sdOut >= 0 {
		t.Fatalf("expected negative exterior signed distance, got %v", sdOut)
	}

	if smoothstep(0, 10, -5) != 0 || smoothstep(0, 10, 15) != 1 || math.Abs(smoothstep(0, 10, 5)-0.5) > 1e-9 {
		t.Fatal("smoothstep boundary/midpoint check failed")
	}

	// Tokyo (139.7E, 35.7N) is an endpoint of the Tokaido metro corridor -> tier 3 core.
	if tier := metroTier(point{139.7, 35.7}); tier != 3 {
		t.Fatalf("expected Tokyo metro tier 3, got %d", tier)
	}
	// Mid-Pacific (150W, 0N) has no metro corridor -> tier 0.
	if tier := metroTier(point{-150.0, 0.0}); tier != 0 {
		t.Fatalf("expected mid-Pacific metro tier 0, got %d", tier)
	}

	var land [mapH][mapW]bool
	land[40][80] = true
	if got := oceanCellType(&land, 81, 40); got != cellShallowOcean {
		t.Fatalf("d=1 from land: expected cellShallowOcean (%d), got %d", cellShallowOcean, got)
	}
	if got := oceanCellType(&land, 83, 40); got != cellMidOcean {
		t.Fatalf("d=3 from land: expected cellMidOcean (%d), got %d", cellMidOcean, got)
	}
	if got := oceanCellType(&land, 90, 40); got != cellDeepOcean {
		t.Fatalf("d=10 from land: expected cellDeepOcean (%d), got %d", cellDeepOcean, got)
	}
}
