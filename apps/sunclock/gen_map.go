// Map generator for World Sun Clock (apps/sunclock/sunclock.lgolf).
// Rasterizes hand-authored continent/biome polygons and metropolitan corridors onto a
// 160x80 grid (80N..80S, 180W..180E) using signed-distance feathering + multi-octave noise.
//
// Usage:
//   go run ./apps/sunclock         # Print 80-row ASCII WORLD_MAP
//   go run ./apps/sunclock -check  # Verify output matches sunclock.lgolf
package main

import (
	"flag"
	"fmt"
	"math"
	"os"
	"strings"
)

const (
	mapW       = 160
	mapH       = 80
	latMaxDeg  = 80.0
	latSpanDeg = 160.0
	lonMinDeg  = -180.0
	lonSpanDeg = 360.0

	// 4-bit cell indices matching cellSymbols below.
	cellDeepOcean    = 0
	cellMidOcean     = 1
	cellShallowOcean = 2
	cellDenseForest  = 3
	cellWoodland     = 4
	cellGrassland    = 5
	cellScrubSteppe  = 6
	cellAridDesert   = 7
	cellFoothills    = 8
	cellHighMountain = 9
	cellTundra       = 10
	cellIceCap       = 11
	cellCityBase     = 11 // + tier (1..3) => 12, 13, 14

	cellSymbols = " ~.FWgsDhMt*123"

	oceanShelfRadiusCells = 3
	oceanShallowMaxDist   = 2
	oceanMidMaxDist       = 4
	sentinelDist          = 999.0
)

type point struct {
	lon, lat float64
}

type metroCorridor struct {
	a, b               point
	rHalo, rMid, rCore float64
}

type rgb struct {
	r, g, b float64
}

type paletteEntry struct {
	cell  int
	color rgb
}

type biomeLayer struct {
	polys      [][]point
	noiseScale float64
	edge0      float64
	edge1      float64
	targetCell int
}

var landPolys = [][]point{
	// North & Central America
	{{-166, 60}, {-168, 66}, {-160, 71}, {-140, 70}, {-125, 70}, {-110, 68}, {-95, 69}, {-82, 66},
		{-92, 62}, {-94, 58}, {-82, 55}, {-78, 52}, {-79, 62}, {-70, 61}, {-64, 60}, {-55, 52},
		{-60, 46}, {-66, 44}, {-70, 42}, {-74, 40}, {-76, 35}, {-81, 31}, {-80, 25}, {-82, 25},
		{-83, 30}, {-89, 29}, {-97, 26}, {-97, 20}, {-90, 21}, {-87, 21}, {-88, 16}, {-83, 15},
		{-83, 10}, {-78, 8}, {-80, 7}, {-85, 10}, {-92, 14}, {-105, 19}, {-110, 24}, {-114, 31},
		{-117, 32}, {-121, 35}, {-124, 42}, {-124, 49}, {-131, 54}, {-140, 60}, {-151, 60}, {-161, 56}},
	{{-85, 73}, {-70, 73}, {-63, 66}, {-74, 64}, {-84, 68}},                                     // Baffin Island
	{{-118, 74}, {-96, 74}, {-96, 70}, {-115, 69}},                                              // Victoria / Banks Islands
	{{-84, 22}, {-74, 20}, {-75, 21}, {-83, 23}},                                                // Cuba
	{{-52, 60}, {-43, 60}, {-33, 68}, {-18, 76}, {-22, 82}, {-48, 82}, {-65, 78}, {-55, 70}}, // Greenland
	{{-24, 64}, {-14, 64}, {-14, 66}, {-24, 66}},                                                // Iceland

	// South America
	{{-78, 9}, {-72, 12}, {-62, 10}, {-50, 1}, {-35, -6}, {-38, -14}, {-41, -22}, {-48, -26},
		{-53, -34}, {-62, -40}, {-66, -45}, {-68, -54}, {-72, -54}, {-75, -46}, {-72, -35},
		{-70, -18}, {-81, -5}, {-80, 1}, {-78, 6}},

	// Europe
	{{-6, 50}, {2, 51}, {1, 55}, {-3, 58}, {-6, 57}, {-5, 52}}, // Great Britain
	{{-10, 51}, {-6, 52}, {-6, 55}, {-10, 54}},                 // Ireland
	{{-9, 37}, {-9, 43}, {-2, 43}, {-5, 48}, {2, 51}, {8, 54}, {10, 58}, {16, 56}, {21, 55},
		{24, 60}, {21, 65}, {26, 70}, {32, 70}, {40, 67}, {45, 68}, {60, 69}, {60, 50}, {47, 46},
		{40, 44}, {35, 46}, {29, 46}, {28, 41}, {23, 37}, {20, 40}, {14, 45}, {18, 40}, {16, 38},
		{12, 42}, {9, 44}, {4, 43}, {0, 39}, {-5, 36}}, // Continental Europe
	{{5, 60}, {10, 58}, {14, 56}, {19, 60}, {22, 66}, {28, 71}, {18, 69}, {11, 64}}, // Scandinavia

	// Africa & Madagascar
	{{-17, 15}, {-13, 28}, {-6, 36}, {10, 37}, {12, 33}, {25, 32}, {33, 31}, {35, 27}, {43, 12},
		{51, 11}, {42, 0}, {40, -11}, {35, -24}, {28, -33}, {18, -34}, {14, -22}, {12, -6}, {9, 4},
		{2, 6}, {-8, 5}, {-16, 11}},
	{{44, -25}, {47, -25}, {50, -14}, {48, -12}, {44, -18}},

	// Asia & Maritime Southeast Asia
	{{27, 41}, {36, 42}, {42, 42}, {48, 46}, {60, 50}, {60, 69}, {75, 73}, {105, 76}, {135, 72},
		{165, 69}, {178, 66}, {170, 60}, {162, 55}, {156, 51}, {142, 54}, {135, 44}, {129, 35},
		{126, 38}, {122, 40}, {118, 39}, {122, 31}, {120, 23}, {109, 20}, {109, 12}, {104, 10},
		{101, 13}, {104, 2}, {101, 2}, {98, 10}, {97, 16}, {90, 22}, {80, 14}, {77, 8}, {72, 20},
		{67, 25}, {57, 26}, {50, 30}, {48, 29}, {59, 22}, {52, 15}, {44, 13}, {39, 22}, {35, 29},
		{35, 36}, {27, 37}},
	{{80, 6}, {82, 7}, {81, 10}, {79, 9}},                                                   // Sri Lanka
	{{130, 31}, {136, 34}, {141, 36}, {142, 41}, {145, 44}, {141, 45}, {139, 38}, {131, 34}}, // Japan
	{{120, 22}, {122, 25}, {121, 25}, {120, 23}},                                            // Taiwan
	{{120, 14}, {122, 18}, {124, 14}, {126, 7}, {122, 7}, {121, 12}},                        // Philippines
	{{95, 5}, {100, 2}, {106, -6}, {104, -6}, {98, 0}},                                      // Sumatra
	{{106, -6}, {114, -8}, {114, -7}, {106, -7}},                                            // Java
	{{109, 1}, {115, 6}, {119, 5}, {117, -4}, {111, -3}},                                    // Borneo
	{{119, -5}, {121, -5}, {125, 1}, {120, 0}},                                              // Sulawesi
	{{131, -1}, {141, -2}, {150, -10}, {146, -10}, {136, -5}},                               // New Guinea

	// Australia, Tasmania & New Zealand
	{{114, -22}, {122, -18}, {131, -12}, {136, -12}, {137, -16}, {142, -11}, {146, -18},
		{153, -26}, {151, -34}, {147, -38}, {140, -38}, {134, -33}, {128, -32}, {115, -34}, {113, -26}},
	{{145, -41}, {148, -41}, {147, -43}, {145, -43}},
	{{167, -46}, {171, -44}, {174, -41}, {178, -37}, {174, -35}, {173, -39}, {170, -42}, {166, -45}},

	// Antarctica coastline (clipped at 80S)
	{{-180, -70}, {-140, -74}, {-100, -72}, {-62, -64}, {-55, -65}, {-40, -74}, {0, -70},
		{45, -68}, {90, -67}, {135, -67}, {170, -72}, {180, -72}, {180, -81}, {-180, -81}},
}

var desertPolys = [][]point{
	{{-17, 15}, {-15, 28}, {-5, 33}, {10, 33}, {25, 31}, {35, 31}, {45, 35}, {62, 38}, {70, 42},
		{72, 27}, {60, 24}, {58, 20}, {45, 13}, {38, 18}, {35, 14}, {20, 14}, {0, 14}}, // Sahara + Arabian
	{{12, -28}, {24, -28}, {25, -19}, {14, -18}},     // Kalahari / Namib
	{{115, -30}, {142, -30}, {142, -20}, {120, -20}}, // Australian Outback
	{{-118, 36}, {-104, 36}, {-102, 24}, {-113, 26}}, // Sonoran / Mojave
	{{76, 37}, {108, 38}, {112, 44}, {82, 45}},       // Taklamakan / Gobi
	{{-72, -28}, {-67, -28}, {-68, -16}, {-73, -18}}, // Atacama
	{{-71, -48}, {-65, -48}, {-63, -37}, {-69, -38}}, // Patagonia
}

var mountainPolys = [][]point{
	{{70, 38}, {103, 38}, {103, 27}, {73, 30}},                                      // Himalayas / Tibetan Plateau
	{{-78, 8}, {-72, 8}, {-66, -22}, {-69, -48}, {-74, -48}, {-73, -20}, {-79, -4}}, // Andes
	{{-155, 64}, {-122, 62}, {-105, 45}, {-105, 33}, {-113, 34}, {-122, 49}, {-145, 61}}, // Rockies / Alaska
	{{5, 44}, {16, 44}, {16, 48}, {6, 48}},   // Alps
	{{38, 41}, {50, 41}, {50, 44}, {38, 44}}, // Caucasus
}

var tundraPolys = [][]point{
	{{-165, 65}, {-60, 65}, {-60, 76}, {-165, 76}}, // Arctic North America
	{{65, 63}, {178, 63}, {178, 76}, {65, 76}},     // Siberia
}

var forestPolys = [][]point{
	{{-78, 6}, {-50, 4}, {-48, -12}, {-72, -14}},             // Amazon
	{{-12, 10}, {32, 6}, {30, -10}, {10, -8}},                // Congo / West Africa
	{{92, 24}, {112, 24}, {150, -10}, {94, -8}},              // SE Asia
	{{-135, 62}, {-55, 62}, {-65, 43}, {-92, 30}, {-96, 50}}, // North American Boreal / Eastern
	{{-6, 44}, {140, 48}, {140, 63}, {0, 63}},                // Eurasian Taiga
}

var icePolys = [][]point{
	{{-180, -63}, {180, -63}, {180, -81}, {-180, -81}}, // Antarctic Ice Sheet
	{{-66, 60}, {-17, 60}, {-17, 82}, {-66, 82}},       // Greenland Ice Sheet
	{{-120, 71}, {-62, 71}, {-62, 80}, {-120, 80}},     // High Canadian Arctic
}

// Major urban night-light corridors: segment a -> b with Halo/Mid/Core radii in degrees.
var metros = []metroCorridor{
	// North America
	{point{-77.0, 38.9}, point{-71.0, 42.4}, 3.0, 1.8, 0.9},   // US Northeast (DC - NYC - Boston)
	{point{-87.7, 41.9}, point{-79.4, 43.7}, 2.7, 1.6, 0.8},   // Great Lakes (Chicago - Detroit - Toronto)
	{point{-84.4, 33.7}, point{-80.8, 35.2}, 2.3, 1.3, 0.6},   // Piedmont (Atlanta - Charlotte)
	{point{-82.5, 27.9}, point{-80.2, 25.8}, 2.1, 1.2, 0.6},   // Florida (Tampa - Miami)
	{point{-97.7, 30.3}, point{-95.4, 29.8}, 2.2, 1.3, 0.6},   // Texas Triangle South (Austin - Houston)
	{point{-96.8, 32.8}, point{-97.3, 32.7}, 2.1, 1.2, 0.6},   // Dallas - Fort Worth
	{point{-122.4, 37.8}, point{-121.9, 37.3}, 2.1, 1.2, 0.6}, // SF Bay Area
	{point{-118.4, 34.1}, point{-117.1, 32.7}, 2.5, 1.5, 0.8}, // SoCal (LA - San Diego)
	{point{-122.7, 45.5}, point{-122.3, 49.3}, 1.9, 1.1, 0.5}, // Cascadia (Portland - Seattle - Vancouver)
	{point{-105.0, 39.7}, point{-104.8, 38.8}, 1.8, 1.0, 0.5}, // Front Range (Denver - Colorado Springs)
	{point{-112.1, 33.4}, point{-112.0, 33.5}, 1.8, 1.0, 0.5}, // Phoenix
	{point{-93.3, 45.0}, point{-93.2, 44.9}, 1.8, 1.0, 0.5},   // Minneapolis - St. Paul
	{point{-75.7, 45.4}, point{-73.6, 45.5}, 2.0, 1.1, 0.5},   // Ottawa - Montreal
	{point{-99.2, 19.4}, point{-98.2, 19.0}, 2.5, 1.5, 0.8},   // Mexico City - Puebla
	{point{-103.3, 20.7}, point{-100.3, 25.7}, 1.9, 1.1, 0.5}, // Guadalajara - Monterrey

	// South America
	{point{-74.1, 4.7}, point{-75.6, 6.2}, 2.0, 1.1, 0.5},     // Bogota - Medellin
	{point{-66.9, 10.5}, point{-67.0, 10.4}, 1.8, 1.0, 0.5},   // Caracas
	{point{-77.0, -12.0}, point{-77.1, -12.1}, 1.9, 1.1, 0.5}, // Lima
	{point{-70.7, -33.4}, point{-71.6, -33.0}, 1.9, 1.1, 0.5}, // Santiago - Valparaiso
	{point{-46.7, -23.6}, point{-43.2, -22.9}, 2.8, 1.7, 0.9}, // Sao Paulo - Rio de Janeiro
	{point{-58.4, -34.6}, point{-56.2, -34.9}, 2.4, 1.4, 0.7}, // Buenos Aires - Montevideo

	// Europe
	{point{-3.0, 53.4}, point{-0.1, 51.5}, 2.8, 1.7, 0.9}, // England (Liverpool - Manchester - London)
	{point{2.3, 48.9}, point{7.0, 51.4}, 3.1, 1.9, 1.0},   // Paris - Benelux - Ruhr
	{point{6.8, 51.2}, point{9.2, 48.8}, 2.4, 1.4, 0.7},   // Rhine (Cologne - Frankfurt - Stuttgart)
	{point{7.7, 45.1}, point{12.3, 45.4}, 2.4, 1.4, 0.7},  // Po Valley (Turin - Milan - Venice)
	{point{11.3, 44.5}, point{14.3, 40.9}, 2.2, 1.3, 0.6}, // Italy Spine (Bologna - Rome - Naples)
	{point{-3.7, 40.4}, point{2.2, 41.4}, 2.2, 1.3, 0.6},  // Madrid - Barcelona
	{point{-9.1, 38.7}, point{-8.6, 41.1}, 1.8, 1.0, 0.5}, // Lisbon - Porto
	{point{13.4, 52.5}, point{14.4, 50.1}, 2.2, 1.3, 0.6}, // Berlin - Prague
	{point{16.4, 48.2}, point{19.0, 47.5}, 2.1, 1.2, 0.6}, // Vienna - Budapest
	{point{21.0, 52.2}, point{19.9, 50.1}, 2.1, 1.2, 0.6}, // Warsaw - Krakow
	{point{37.6, 55.7}, point{30.3, 59.9}, 2.5, 1.5, 0.7}, // Moscow - St. Petersburg
	{point{30.5, 50.4}, point{30.6, 50.5}, 1.9, 1.1, 0.5}, // Kyiv
	{point{29.0, 41.0}, point{32.9, 39.9}, 2.3, 1.4, 0.7}, // Istanbul - Ankara
	{point{23.7, 38.0}, point{23.8, 37.9}, 1.8, 1.0, 0.5}, // Athens

	// Africa & Middle East
	{point{31.2, 30.0}, point{31.3, 31.2}, 2.6, 1.5, 0.8},   // Nile Delta (Cairo - Alexandria)
	{point{31.2, 30.0}, point{32.6, 25.7}, 1.8, 1.0, 0.5},   // Upper Nile (Cairo - Luxor)
	{point{3.0, 36.7}, point{-7.6, 33.6}, 1.9, 1.1, 0.5},    // Maghreb (Algiers - Casablanca)
	{point{3.4, 6.5}, point{7.5, 9.1}, 2.4, 1.4, 0.7},       // Nigeria (Lagos - Abuja)
	{point{36.8, -1.3}, point{32.6, 0.3}, 1.9, 1.1, 0.5},    // East Africa (Nairobi - Kampala)
	{point{28.0, -26.2}, point{28.2, -25.7}, 2.2, 1.3, 0.6}, // Gauteng (Johannesburg - Pretoria)
	{point{18.4, -33.9}, point{18.5, -33.9}, 1.8, 1.0, 0.5}, // Cape Town
	{point{34.8, 32.1}, point{35.5, 33.9}, 2.0, 1.2, 0.6},   // Levant (Tel Aviv - Beirut)
	{point{46.7, 24.7}, point{50.1, 26.3}, 2.1, 1.2, 0.6},   // Riyadh - Dammam
	{point{55.3, 25.2}, point{51.5, 25.3}, 2.4, 1.4, 0.7},   // Persian Gulf (Dubai - Doha)
	{point{51.4, 35.7}, point{51.7, 32.7}, 2.3, 1.4, 0.7},   // Tehran - Isfahan

	// South & East Asia, Oceania
	{point{74.3, 31.5}, point{67.0, 24.9}, 2.1, 1.2, 0.6},     // Indus (Lahore - Karachi)
	{point{77.2, 28.6}, point{88.4, 22.6}, 3.2, 2.0, 1.0},     // Indo-Gangetic Plain (Delhi - Kolkata)
	{point{72.9, 19.1}, point{73.9, 18.5}, 2.4, 1.4, 0.7},     // Mumbai - Pune
	{point{77.6, 13.0}, point{80.3, 13.1}, 2.3, 1.3, 0.7},     // Bengaluru - Chennai
	{point{90.4, 23.8}, point{91.8, 22.3}, 2.1, 1.2, 0.6},     // Dhaka - Chittagong
	{point{116.4, 39.9}, point{117.2, 39.1}, 2.9, 1.7, 0.9},   // Beijing - Tianjin
	{point{113.7, 34.7}, point{120.2, 36.1}, 2.8, 1.7, 0.9},   // North China Plain (Zhengzhou - Qingdao)
	{point{118.8, 32.1}, point{121.5, 31.2}, 3.0, 1.8, 0.9},   // Yangtze Delta (Nanjing - Shanghai)
	{point{114.3, 30.6}, point{113.0, 28.2}, 2.3, 1.3, 0.6},   // Wuhan - Changsha
	{point{104.1, 30.7}, point{106.5, 29.6}, 2.4, 1.4, 0.7},   // Sichuan Basin (Chengdu - Chongqing)
	{point{113.3, 23.1}, point{114.2, 22.3}, 2.8, 1.7, 0.9},   // Pearl River Delta (Guangzhou - Hong Kong)
	{point{120.2, 22.6}, point{121.5, 25.0}, 2.0, 1.1, 0.6},   // Taiwan West Coast
	{point{127.0, 37.6}, point{129.1, 35.2}, 2.4, 1.4, 0.7},   // South Korea (Seoul - Busan)
	{point{139.7, 35.7}, point{135.5, 34.7}, 2.9, 1.7, 0.9},   // Tokaido Corridor (Tokyo - Osaka)
	{point{100.5, 13.7}, point{100.6, 13.8}, 2.2, 1.3, 0.6},   // Bangkok
	{point{101.7, 3.1}, point{103.8, 1.3}, 2.1, 1.2, 0.6},     // Kuala Lumpur - Singapore
	{point{106.8, -6.2}, point{112.7, -7.3}, 2.4, 1.4, 0.7},   // Java (Jakarta - Surabaya)
	{point{121.0, 14.6}, point{121.1, 14.5}, 2.0, 1.2, 0.6},   // Manila
	{point{151.2, -33.9}, point{153.0, -27.5}, 2.0, 1.2, 0.6}, // Sydney - Brisbane
	{point{145.0, -37.8}, point{138.6, -34.9}, 1.9, 1.1, 0.5}, // Melbourne - Adelaide
}

// 9 feathered land biome palette entries in deterministic order (cell indices 3..11).
var landPalette = []paletteEntry{
	{cellDenseForest, rgb{34, 110, 48}},
	{cellWoodland, rgb{64, 128, 58}},
	{cellGrassland, rgb{102, 144, 68}},
	{cellScrubSteppe, rgb{146, 152, 86}},
	{cellAridDesert, rgb{196, 166, 110}},
	{cellFoothills, rgb{144, 126, 98}},
	{cellHighMountain, rgb{118, 114, 108}},
	{cellTundra, rgb{152, 158, 156}},
	{cellIceCap, rgb{224, 236, 246}},
}

var biomeLayers = []biomeLayer{
	{forestPolys, 1.0, -6.0, 7.0, cellDenseForest},
	{desertPolys, 1.0, -7.0, 7.0, cellAridDesert},
	{mountainPolys, 0.7, -4.0, 5.0, cellHighMountain},
	{tundraPolys, 1.0, -5.0, 5.0, cellTundra},
	{icePolys, 0.5, -3.0, 4.0, cellIceCap},
}

func biomeColor(cell int) rgb {
	for _, entry := range landPalette {
		if entry.cell == cell {
			return entry.color
		}
	}
	return rgb{}
}

func cellCenterPoint(px, py int) point {
	return point{
		lon: lonMinDeg + (float64(px)+0.5)*(lonSpanDeg/mapW),
		lat: latMaxDeg - (float64(py)+0.5)*(latSpanDeg/mapH),
	}
}

func pointInPoly(pt point, poly []point) bool {
	inside := false
	j := len(poly) - 1
	for i := 0; i < len(poly); i++ {
		pi, pj := poly[i], poly[j]
		if ((pi.lat > pt.lat) != (pj.lat > pt.lat)) &&
			(pt.lon < (pj.lon-pi.lon)*(pt.lat-pi.lat)/(pj.lat-pi.lat+1e-12)+pi.lon) {
			inside = !inside
		}
		j = i
	}
	return inside
}

func distToSegment(pt, a, b point) float64 {
	dx, dy := b.lon-a.lon, b.lat-a.lat
	l2 := dx*dx + dy*dy
	if l2 < 1e-6 {
		return math.Hypot(pt.lon-a.lon, pt.lat-a.lat)
	}
	t := math.Max(0.0, math.Min(1.0, ((pt.lon-a.lon)*dx+(pt.lat-a.lat)*dy)/l2))
	return math.Hypot(pt.lon-(a.lon+t*dx), pt.lat-(a.lat+t*dy))
}

func signedDistPolys(pt point, polys [][]point) float64 {
	best := -sentinelDist
	for _, poly := range polys {
		inside := pointInPoly(pt, poly)
		dEdge := sentinelDist
		n := len(poly)
		for i := 0; i < n; i++ {
			if d := distToSegment(pt, poly[i], poly[(i+1)%n]); d < dEdge {
				dEdge = d
			}
		}
		sd := -dEdge
		if inside {
			sd = dEdge
		}
		if sd > best {
			best = sd
		}
	}
	return best
}

func smoothstep(edge0, edge1, x float64) float64 {
	t := math.Max(0.0, math.Min(1.0, (x-edge0)/(edge1-edge0)))
	return t * t * (3.0 - 2.0*t)
}

// Three-octave spatial wave perturbation (in degrees) to break up straight polygon edges.
func biomeEdgeNoiseDeg(pt point) float64 {
	return 2.6*math.Sin(pt.lon*0.14+pt.lat*0.19) +
		1.5*math.Cos(pt.lon*0.31-pt.lat*0.27) +
		0.9*math.Sin(pt.lon*0.53+pt.lat*0.43)
}

func oceanCellType(land *[mapH][mapW]bool, px, py int) int {
	dmin := int(sentinelDist)
	for dy := -oceanShelfRadiusCells; dy <= oceanShelfRadiusCells; dy++ {
		ny := py + dy
		if ny < 0 || ny >= mapH {
			continue
		}
		for dx := -oceanShelfRadiusCells; dx <= oceanShelfRadiusCells; dx++ {
			nx := (px + dx + mapW) % mapW
			if land[ny][nx] {
				d := int(math.Abs(float64(dx)) + math.Abs(float64(dy)))
				if d < dmin {
					dmin = d
				}
			}
		}
	}
	if dmin <= oceanShallowMaxDist {
		return cellShallowOcean
	}
	if dmin <= oceanMidMaxDist {
		return cellMidOcean
	}
	return cellDeepOcean
}

func metroTier(pt point) int {
	tier := 0
	for _, m := range metros {
		d := distToSegment(pt, m.a, m.b)
		if d <= m.rCore {
			return 3
		}
		if d <= m.rMid && tier < 2 {
			tier = 2
		} else if d <= m.rHalo && tier < 1 {
			tier = 1
		}
	}
	return tier
}

func nearestBiomeCell(c rgb) int {
	bestCell := cellGrassland
	bestDist2 := math.MaxFloat64
	for _, entry := range landPalette {
		dr, dg, db := c.r-entry.color.r, c.g-entry.color.g, c.b-entry.color.b
		if d2 := dr*dr + dg*dg + db*db; d2 < bestDist2 {
			bestDist2 = d2
			bestCell = entry.cell
		}
	}
	return bestCell
}

func landBiomeCell(pt point) int {
	if tier := metroTier(pt); tier > 0 {
		return cellCityBase + tier
	}

	noise := biomeEdgeNoiseDeg(pt)
	c := biomeColor(cellGrassland)
	for _, layer := range biomeLayers {
		sd := signedDistPolys(pt, layer.polys) + noise*layer.noiseScale
		w := smoothstep(layer.edge0, layer.edge1, sd)
		target := biomeColor(layer.targetCell)
		c.r = c.r*(1-w) + target.r*w
		c.g = c.g*(1-w) + target.g*w
		c.b = c.b*(1-w) + target.b*w
	}
	return nearestBiomeCell(c)
}

func rasterizeLandMask() [mapH][mapW]bool {
	var land [mapH][mapW]bool
	for py := 0; py < mapH; py++ {
		for px := 0; px < mapW; px++ {
			pt := cellCenterPoint(px, py)
			for _, poly := range landPolys {
				if pointInPoly(pt, poly) {
					land[py][px] = true
					break
				}
			}
		}
	}
	return land
}

func generateWorldMap() []string {
	land := rasterizeLandMask()
	rows := make([]string, mapH)
	for py := 0; py < mapH; py++ {
		var row strings.Builder
		row.Grow(mapW)
		for px := 0; px < mapW; px++ {
			cell := oceanCellType(&land, px, py)
			if land[py][px] {
				cell = landBiomeCell(cellCenterPoint(px, py))
			}
			row.WriteByte(cellSymbols[cell])
		}
		rows[py] = row.String()
	}
	return rows
}

func extractExistingWorldMap(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	src := string(data)
	marker := "const WORLD_MAP = \"\n"
	start := strings.Index(src, marker)
	if start < 0 {
		return "", fmt.Errorf("WORLD_MAP not found in %s", path)
	}
	start += len(marker)
	end := strings.Index(src[start:], "\n\"")
	if end < 0 {
		return "", fmt.Errorf("WORLD_MAP terminator not found in %s", path)
	}
	return src[start : start+end], nil
}

func main() {
	check := flag.Bool("check", false, "verify generated map matches apps/sunclock/sunclock.lgolf")
	flag.Parse()

	generated := strings.Join(generateWorldMap(), "\n")
	if *check {
		existing, err := extractExistingWorldMap("apps/sunclock/sunclock.lgolf")
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(1)
		}
		if generated != existing {
			fmt.Fprintln(os.Stderr, "MISMATCH: generated map differs from apps/sunclock/sunclock.lgolf")
			os.Exit(1)
		}
		fmt.Println("MATCH_OK: generated 160x80 WORLD_MAP matches apps/sunclock/sunclock.lgolf byte-for-byte")
		return
	}
	fmt.Println(generated)
}
