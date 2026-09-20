package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/minio/selfupdate"
)

// githubRepo is where releases are published - see .github/workflows/release.yml
// (`git tag vX.Y.Z && git push origin vX.Y.Z` triggers it).
const githubRepo = "marcelwirtz/XPMultiCrew"

// githubAPIBaseURL is a var, not a const, so updater_test.go can point it
// at a local httptest server instead of the real GitHub API.
var githubAPIBaseURL = "https://api.github.com"

// platformAssetName is the release asset (a zip - see release.yml's
// Package steps) that contains this platform's companion executable.
// GOOS is the right switch here, not build tags: the running binary
// already knows what OS it's on, and this only affects which URL to
// fetch, not what compiles into this binary.
func platformAssetName() (zipName, exeName string, ok bool) {
	switch runtime.GOOS {
	case "linux":
		return "xpmulticrew-linux.zip", "xpmulticrew-companion", true
	case "windows":
		return "xpmulticrew-windows.zip", "xpmulticrew-companion.exe", true
	default:
		return "", "", false
	}
}

// UpdateInfo is what CheckForUpdate returns to the frontend when a newer
// release is available (nil otherwise).
type UpdateInfo struct {
	Version string `json:"version"` // the new release's tag, e.g. "v0.2.0"
}

type githubAsset struct {
	Name               string `json:"name"`
	BrowserDownloadURL string `json:"browser_download_url"`
}

type githubRelease struct {
	TagName string        `json:"tag_name"`
	Assets  []githubAsset `json:"assets"`
}

func fetchLatestRelease() (*githubRelease, error) {
	client := &http.Client{Timeout: 10 * time.Second}
	req, err := http.NewRequest(http.MethodGet,
		fmt.Sprintf("%s/repos/%s/releases/latest", githubAPIBaseURL, githubRepo), nil)
	if err != nil {
		return nil, err
	}
	// GitHub's API asks for this; some proxies/CDNs in front of it also
	// reject requests with no Accept header at all.
	req.Header.Set("Accept", "application/vnd.github+json")

	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("github returned %s fetching the latest release", resp.Status)
	}

	var release githubRelease
	if err := json.NewDecoder(resp.Body).Decode(&release); err != nil {
		return nil, err
	}
	return &release, nil
}

func findAsset(release *githubRelease, name string) (*githubAsset, bool) {
	for i := range release.Assets {
		if release.Assets[i].Name == name {
			return &release.Assets[i], true
		}
	}
	return nil, false
}

// CheckForUpdate compares the latest GitHub release's tag against this
// build's own companionVersion (plain string inequality, matching the
// existing Installed/Available plugin-version convention in app.go's
// refreshVersions() - not semver parsing). Returns (nil, nil) - not an
// error - both when already up to date and when companionVersion is
// "dev" (a local unreleased build should never offer to "update" itself
// over a real release).
func CheckForUpdate() (*UpdateInfo, error) {
	if companionVersion == "" || companionVersion == "dev" {
		return nil, nil
	}
	release, err := fetchLatestRelease()
	if err != nil {
		return nil, err
	}
	if release.TagName == "" || release.TagName == companionVersion {
		return nil, nil
	}
	return &UpdateInfo{Version: release.TagName}, nil
}

func downloadBytes(url string) ([]byte, error) {
	client := &http.Client{Timeout: 2 * time.Minute}
	resp, err := client.Get(url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("download failed: %s", resp.Status)
	}
	return io.ReadAll(resp.Body)
}

// verifyChecksum checks `data` against the SHA256 recorded for `name` in
// `sha256sums` - the plain `sha256sum`-format text file
// (.github/workflows/release.yml's "Compute checksums" step publishes it
// as the SHA256SUMS release asset). Verifying the downloaded zip itself,
// before it's even opened, catches a corrupted or tampered download at
// the one point this app fetches and later executes something from the
// network - selfupdate.Apply's own optional checksum is for the final
// extracted binary, which isn't needed on top of this since the zip is
// already verified end-to-end before being extracted.
func verifyChecksum(sha256sums string, name string, data []byte) error {
	expected := ""
	for _, line := range strings.Split(sha256sums, "\n") {
		fields := strings.Fields(line)
		if len(fields) == 2 && fields[1] == name {
			expected = fields[0]
			break
		}
	}
	if expected == "" {
		return fmt.Errorf("no checksum found for %q in SHA256SUMS", name)
	}

	sum := sha256.Sum256(data)
	got := hex.EncodeToString(sum[:])
	if !strings.EqualFold(got, expected) {
		return fmt.Errorf("checksum mismatch for %q: expected %s, got %s", name, expected, got)
	}
	return nil
}

// extractExecutable pulls exactly one named entry's bytes out of an
// in-memory zip (the release zips also carry a `.desktop` file on Linux -
// see release.yml's Package step - which is deliberately not what gets
// applied here).
func extractExecutable(zipBytes []byte, exeName string) ([]byte, error) {
	r, err := zip.NewReader(bytes.NewReader(zipBytes), int64(len(zipBytes)))
	if err != nil {
		return nil, err
	}
	for _, f := range r.File {
		if filepath.Base(f.Name) != exeName {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			return nil, err
		}
		defer rc.Close()
		return io.ReadAll(rc)
	}
	return nil, fmt.Errorf("no %q entry found in the downloaded zip", exeName)
}

// prepareUpdate downloads the latest release's platform-appropriate zip,
// verifies it against the published SHA256SUMS, and extracts the
// executable's bytes - everything up to, but not including, actually
// committing them over the running binary (applyUpdate, below). Split out
// so this whole pipeline (asset selection, checksum matching, extraction)
// is unit-testable via a fake release server (updater_test.go) without
// ever invoking selfupdate.Apply, which would replace whatever binary is
// currently running - fine in the real app, not something a test process
// should ever do to itself.
func prepareUpdate() (exeBytes []byte, err error) {
	zipName, exeName, ok := platformAssetName()
	if !ok {
		return nil, fmt.Errorf("no companion release build exists for this platform (%s)", runtime.GOOS)
	}

	release, err := fetchLatestRelease()
	if err != nil {
		return nil, err
	}
	zipAsset, ok := findAsset(release, zipName)
	if !ok {
		return nil, fmt.Errorf("release %s has no %q asset", release.TagName, zipName)
	}
	sumsAsset, ok := findAsset(release, "SHA256SUMS")
	if !ok {
		return nil, fmt.Errorf("release %s has no SHA256SUMS asset", release.TagName)
	}

	sumsBytes, err := downloadBytes(sumsAsset.BrowserDownloadURL)
	if err != nil {
		return nil, fmt.Errorf("downloading SHA256SUMS: %w", err)
	}
	zipBytes, err := downloadBytes(zipAsset.BrowserDownloadURL)
	if err != nil {
		return nil, fmt.Errorf("downloading %s: %w", zipName, err)
	}
	if err := verifyChecksum(string(sumsBytes), zipName, zipBytes); err != nil {
		return nil, fmt.Errorf("%s failed verification, not applying: %w", zipName, err)
	}

	return extractExecutable(zipBytes, exeName)
}

// applyUpdate is prepareUpdate plus actually committing the result over
// the currently-running binary (selfupdate.Apply - see updater.go's
// import - handles the OS-specific rename-while-running swap; TargetPath
// defaults to os.Executable()). Does not relaunch or exit - that's
// App.ApplyUpdate's job (companion/app.go), which is also what needs the
// Wails runtime this file deliberately doesn't depend on.
func applyUpdate() error {
	exeBytes, err := prepareUpdate()
	if err != nil {
		return err
	}

	if err := selfupdate.Apply(bytes.NewReader(exeBytes), selfupdate.Options{}); err != nil {
		if rerr := selfupdate.RollbackError(err); rerr != nil {
			return fmt.Errorf("update failed AND rollback failed - reinstall manually: %w", rerr)
		}
		return fmt.Errorf("update failed (rolled back, still running the previous version): %w", err)
	}
	return nil
}
