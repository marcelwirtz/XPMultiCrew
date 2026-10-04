package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"github.com/bodgit/sevenzip"
)

// Ground elevation straight from the user's X-Plane scenery: every DSF
// mesh tile carries an "elevation" raster (1201 x 1201 posts per 1° tile
// in X-Plane 12's global scenery, metres). The tile X-Plane itself would
// use wins - Custom Scenery packs in scenery_packs.ini order (overlays
// without a mesh have no raster and are skipped), then the global scenery.
// DSFs are 7z-compressed; the raster is cached decompressed in the user
// cache dir, so each tile is unpacked once.

type demTile struct {
	w, h        int
	postCentric bool
	data        []int16 // metres, row 0 = south edge, west to east
}

func (t *demTile) at(fracLat, fracLon float64) float64 {
	fx, fy := fracLon*float64(t.w), fracLat*float64(t.h)
	if t.postCentric {
		fx, fy = fracLon*float64(t.w-1), fracLat*float64(t.h-1)
	} else {
		fx, fy = fx-0.5, fy-0.5
	}
	x := min(t.w-1, max(0, int(math.Round(fx))))
	y := min(t.h-1, max(0, int(math.Round(fy))))
	return float64(t.data[y*t.w+x])
}

var (
	demMu    sync.Mutex
	demTiles = map[string]*demTile{} // "lat,lon" -> tile (nil = none installed)
	demOrder []string
)

const demMemTiles = 24

func tileDir(lat, lon int) string {
	fl := func(v int) int { return int(math.Floor(float64(v)/10)) * 10 }
	return fmt.Sprintf("%+03d%+04d", fl(lat), fl(lon))
}

func tileName(lat, lon int) string {
	return fmt.Sprintf("%+03d%+04d.dsf", lat, lon)
}

// sceneryPackDirs lists the folders X-Plane looks in for mesh, highest
// priority first.
func sceneryPackDirs(xplaneRoot string) []string {
	dirs := []string{}
	if f, err := os.Open(filepath.Join(xplaneRoot, "Custom Scenery", "scenery_packs.ini")); err == nil {
		s := bufio.NewScanner(f)
		for s.Scan() {
			rest, ok := strings.CutPrefix(strings.TrimSpace(s.Text()), "SCENERY_PACK ")
			if !ok || strings.HasPrefix(rest, "*GLOBAL_AIRPORTS*") {
				continue
			}
			p := filepath.FromSlash(strings.TrimRight(rest, "/"))
			if !filepath.IsAbs(p) {
				p = filepath.Join(xplaneRoot, p)
			}
			dirs = append(dirs, p)
		}
		f.Close()
	}
	globals, _ := filepath.Glob(filepath.Join(xplaneRoot, "Global Scenery", "*"))
	return append(dirs, globals...)
}

// xplaneElevation returns the ground elevation (ft) from X-Plane's scenery,
// or false if no mesh tile is installed there.
func xplaneElevation(xplaneRoot string, lat, lon float64) (float64, bool) {
	la, lo := int(math.Floor(lat)), int(math.Floor(lon))
	t := loadDemTile(xplaneRoot, la, lo)
	if t == nil {
		return 0, false
	}
	return math.Max(0, t.at(lat-float64(la), lon-float64(lo))) * 3.28084, true
}

func loadDemTile(xplaneRoot string, lat, lon int) *demTile {
	key := fmt.Sprintf("%d,%d", lat, lon)
	demMu.Lock()
	defer demMu.Unlock()
	if t, ok := demTiles[key]; ok {
		return t
	}
	var tile *demTile
	for _, dir := range sceneryPackDirs(xplaneRoot) {
		path := filepath.Join(dir, "Earth nav data", tileDir(lat, lon), tileName(lat, lon))
		st, err := os.Stat(path)
		if err != nil {
			continue
		}
		cache := demCachePath(path, st)
		if t, err := readDemCache(cache); err == nil {
			tile = t
		} else if t, err := readDSFElevation(path); err == nil {
			tile = t
			writeDemCache(cache, t)
		} else if errors.Is(err, errNoElevation) {
			continue // an overlay: no mesh in this pack
		}
		if tile != nil {
			break
		}
	}
	demTiles[key] = tile
	demOrder = append(demOrder, key)
	if len(demOrder) > demMemTiles {
		delete(demTiles, demOrder[0])
		demOrder = demOrder[1:]
	}
	return tile
}

func demCachePath(dsf string, st os.FileInfo) string {
	dir, err := os.UserCacheDir()
	if err != nil {
		return ""
	}
	h := uint32(2166136261)
	for _, c := range fmt.Sprintf("%s|%d|%d", dsf, st.Size(), st.ModTime().UnixNano()) {
		h = (h ^ uint32(c)) * 16777619
	}
	return filepath.Join(dir, "xpmulticrew-companion", "terrain", fmt.Sprintf("%s-%08x.dem", strings.TrimSuffix(filepath.Base(dsf), ".dsf"), h))
}

// Cache file: "XPMCDEM1", width, height, post-centric flag (int32 LE each),
// then the int16 posts.
func writeDemCache(path string, t *demTile) {
	if path == "" {
		return
	}
	_ = os.MkdirAll(filepath.Dir(path), 0755)
	var buf bytes.Buffer
	buf.WriteString("XPMCDEM1")
	pc := int32(0)
	if t.postCentric {
		pc = 1
	}
	_ = binary.Write(&buf, binary.LittleEndian, []int32{int32(t.w), int32(t.h), pc})
	_ = binary.Write(&buf, binary.LittleEndian, t.data)
	tmp := path + ".tmp"
	if os.WriteFile(tmp, buf.Bytes(), 0644) == nil {
		_ = os.Rename(tmp, path)
	}
}

func readDemCache(path string) (*demTile, error) {
	if path == "" {
		return nil, errors.New("no cache")
	}
	raw, err := os.ReadFile(path)
	if err != nil || len(raw) < 20 || string(raw[:8]) != "XPMCDEM1" {
		return nil, errors.New("no cache")
	}
	w := int(int32(binary.LittleEndian.Uint32(raw[8:])))
	h := int(int32(binary.LittleEndian.Uint32(raw[12:])))
	if w <= 0 || h <= 0 || w > 10000 || h > 10000 || len(raw) != 20+w*h*2 {
		return nil, errors.New("bad cache")
	}
	t := &demTile{w: w, h: h, postCentric: binary.LittleEndian.Uint32(raw[16:]) == 1, data: make([]int16, w*h)}
	if err := binary.Read(bytes.NewReader(raw[20:]), binary.LittleEndian, t.data); err != nil {
		return nil, err
	}
	return t, nil
}

var errNoElevation = errors.New("no elevation raster in this DSF")

// readDSFElevation unpacks a DSF (7z or plain) and returns its elevation
// raster.
func readDSFElevation(path string) (*demTile, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	magic := make([]byte, 6)
	if _, err := io.ReadFull(f, magic); err != nil {
		return nil, err
	}
	var raw []byte
	if bytes.Equal(magic, []byte{'7', 'z', 0xBC, 0xAF, 0x27, 0x1C}) {
		st, _ := f.Stat()
		zr, err := sevenzip.NewReader(f, st.Size())
		if err != nil {
			return nil, err
		}
		if len(zr.File) == 0 {
			return nil, errors.New("empty archive")
		}
		rc, err := zr.File[0].Open()
		if err != nil {
			return nil, err
		}
		raw, err = io.ReadAll(io.LimitReader(rc, 512<<20))
		rc.Close()
		if err != nil {
			return nil, err
		}
	} else {
		if _, err := f.Seek(0, io.SeekStart); err != nil {
			return nil, err
		}
		if raw, err = io.ReadAll(io.LimitReader(f, 512<<20)); err != nil {
			return nil, err
		}
	}
	return parseDSFElevation(raw)
}

// parseDSFElevation walks the DSF atoms: the raster names (DEFN/DEMN) and
// the rasters (DEMS: DEMI header + DEMD data, in the same order).
func parseDSFElevation(raw []byte) (*demTile, error) {
	if len(raw) < 12+16 || string(raw[:8]) != "XPLNEDSF" {
		return nil, errors.New("not a DSF file")
	}
	end := len(raw) - 16 // MD5 footer
	type atom struct {
		id         string
		start, end int
	}
	children := func(from, to int) []atom {
		out := []atom{}
		for off := from; off+8 <= to; {
			id := string([]byte{raw[off+3], raw[off+2], raw[off+1], raw[off]})
			n := int(binary.LittleEndian.Uint32(raw[off+4:]))
			if n < 8 || off+n > to {
				break
			}
			out = append(out, atom{id, off + 8, off + n})
			off += n
		}
		return out
	}
	names := []string{}
	var dems []atom
	for _, a := range children(12, end) {
		switch a.id {
		case "DEFN":
			for _, c := range children(a.start, a.end) {
				if c.id == "DEMN" {
					for _, s := range strings.Split(string(raw[c.start:c.end]), "\x00") {
						if s != "" {
							names = append(names, s)
						}
					}
				}
			}
		case "DEMS":
			dems = children(a.start, a.end)
		}
	}
	idx := -1
	for i, n := range names {
		if n == "elevation" {
			idx = i
		}
	}
	if idx < 0 {
		return nil, errNoElevation
	}
	k := -1
	for i := 0; i+1 < len(dems); i++ {
		if dems[i].id != "DEMI" || dems[i+1].id != "DEMD" {
			continue
		}
		k++
		if k != idx {
			i++
			continue
		}
		info := raw[dems[i].start:dems[i].end]
		if len(info) < 20 {
			return nil, errors.New("short DEMI")
		}
		bpp, flags := int(info[1]), binary.LittleEndian.Uint16(info[2:])
		w, h := int(binary.LittleEndian.Uint32(info[4:])), int(binary.LittleEndian.Uint32(info[8:]))
		scale := math.Float32frombits(binary.LittleEndian.Uint32(info[12:]))
		offset := math.Float32frombits(binary.LittleEndian.Uint32(info[16:]))
		data := raw[dems[i+1].start:dems[i+1].end]
		if w <= 0 || h <= 0 || w > 10000 || h > 10000 || len(data) < w*h*bpp {
			return nil, errors.New("bad DEM size")
		}
		t := &demTile{w: w, h: h, postCentric: flags&4 != 0, data: make([]int16, w*h)}
		kind := flags & 3 // 0 float, 1 signed int, 2 unsigned int
		for p := 0; p < w*h; p++ {
			var v float64
			b := data[p*bpp:]
			switch {
			case kind == 0 && bpp == 4:
				v = float64(math.Float32frombits(binary.LittleEndian.Uint32(b)))
			case kind == 1 && bpp == 2:
				v = float64(int16(binary.LittleEndian.Uint16(b)))
			case kind == 2 && bpp == 2:
				v = float64(binary.LittleEndian.Uint16(b))
			case kind == 1 && bpp == 1:
				v = float64(int8(b[0]))
			case kind == 2 && bpp == 1:
				v = float64(b[0])
			case kind == 1 && bpp == 4:
				v = float64(int32(binary.LittleEndian.Uint32(b)))
			default:
				return nil, fmt.Errorf("unsupported DEM format (%d bytes, flags %d)", bpp, flags)
			}
			v = v*float64(scale) + float64(offset)
			t.data[p] = int16(math.Max(-32768, math.Min(32767, math.Round(v))))
		}
		return t, nil
	}
	return nil, errNoElevation
}
