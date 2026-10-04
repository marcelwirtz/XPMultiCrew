package main

import (
	"bytes"
	"encoding/binary"
	"math"
	"os"
	"path/filepath"
	"testing"
)

func dsfAtom(id string, payload []byte) []byte {
	var b bytes.Buffer
	b.Write([]byte{id[3], id[2], id[1], id[0]})
	_ = binary.Write(&b, binary.LittleEndian, uint32(8+len(payload)))
	b.Write(payload)
	return b.Bytes()
}

// A 3x3 post-centric int16 elevation raster, south row first.
func testDSF(names string, withRaster bool) []byte {
	var b bytes.Buffer
	b.WriteString("XPLNEDSF")
	_ = binary.Write(&b, binary.LittleEndian, int32(1))
	b.Write(dsfAtom("DEFN", dsfAtom("DEMN", []byte(names))))
	if withRaster {
		var info bytes.Buffer
		info.Write([]byte{1, 2})
		_ = binary.Write(&info, binary.LittleEndian, uint16(5)) // signed + post-centric
		_ = binary.Write(&info, binary.LittleEndian, []uint32{3, 3})
		_ = binary.Write(&info, binary.LittleEndian, []float32{1, 0})
		var data bytes.Buffer
		_ = binary.Write(&data, binary.LittleEndian, []int16{10, 20, 30, 40, 50, 60, 70, 80, 900})
		dems := append(dsfAtom("DEMI", info.Bytes()), dsfAtom("DEMD", data.Bytes())...)
		b.Write(dsfAtom("DEMS", dems))
	}
	b.Write(make([]byte, 16))
	return b.Bytes()
}

func TestParseDSFElevation(t *testing.T) {
	tile, err := parseDSFElevation(testDSF("elevation\x00sea_level\x00", true))
	if err != nil {
		t.Fatal(err)
	}
	if tile.w != 3 || !tile.postCentric || tile.at(0, 0) != 10 || tile.at(1, 1) != 900 || tile.at(0.5, 0.5) != 50 {
		t.Fatalf("unexpected tile %+v", tile)
	}
	if _, err := parseDSFElevation(testDSF("", false)); err != errNoElevation {
		t.Fatalf("an overlay has no elevation, got %v", err)
	}
}

func TestXPlaneElevationPackOrder(t *testing.T) {
	root := t.TempDir()
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	write := func(rel string, data []byte) {
		p := filepath.Join(root, rel)
		_ = os.MkdirAll(filepath.Dir(p), 0755)
		_ = os.WriteFile(p, data, 0644)
	}
	// An overlay pack first (no mesh), then a mesh pack, then the global one.
	write("Custom Scenery/Overlay/Earth nav data/+50+000/+51+007.dsf", testDSF("", false))
	write("Custom Scenery/Mesh/Earth nav data/+50+000/+51+007.dsf", testDSF("elevation\x00", true))
	write("Global Scenery/X-Plane 12 Global Scenery/Earth nav data/+50+000/+52+007.dsf", testDSF("elevation\x00", true))
	write("Custom Scenery/scenery_packs.ini", []byte("I\n1000 Version\nSCENERY\n\nSCENERY_PACK Custom Scenery/Overlay/\nSCENERY_PACK_DISABLED Custom Scenery/Nope/\nSCENERY_PACK Custom Scenery/Mesh/\n"))
	demMu.Lock()
	demTiles, demOrder = map[string]*demTile{}, nil
	demMu.Unlock()
	if ft, ok := xplaneElevation(root, 51.999, 7.999); !ok || math.Abs(ft-900*3.28084) > 1 {
		t.Fatalf("expected the mesh pack's 900 m, got %v %v", ft, ok)
	}
	if _, ok := xplaneElevation(root, 52.5, 7.5); !ok {
		t.Fatal("expected the global scenery tile")
	}
	if _, ok := xplaneElevation(root, 10.5, 7.5); ok {
		t.Fatal("no tile installed there")
	}
	if tileDir(-34, -59) != "-40-060" || tileName(-34, -59) != "-34-059.dsf" {
		t.Fatalf("tile naming: %s %s", tileDir(-34, -59), tileName(-34, -59))
	}
}
