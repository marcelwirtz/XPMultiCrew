package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// fakeGitHubReleaseServer stands in for api.github.com/repos/.../releases/latest
// (same "real loopback server instead of a mock" approach as the rest of
// this package's tests, e.g. plugin_client_test.go's real UDP sockets) -
// points githubAPIBaseURL at it for the duration of the test.
func fakeGitHubReleaseServer(t *testing.T, tagName string, assets map[string][]byte) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()

	var assetsJSON strings.Builder
	assetsJSON.WriteString("[")
	first := true
	mux.HandleFunc(fmt.Sprintf("/repos/%s/releases/latest", githubRepo), func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"tag_name":"` + tagName + `","assets":` + assetsJSON.String() + `]}`))
	})

	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)

	for name, data := range assets {
		if !first {
			assetsJSON.WriteString(",")
		}
		first = false
		url := srv.URL + "/download/" + name
		assetsJSON.WriteString(`{"name":"` + name + `","browser_download_url":"` + url + `"}`)
		data := data
		mux.HandleFunc("/download/"+name, func(w http.ResponseWriter, r *http.Request) {
			_, _ = w.Write(data)
		})
	}

	old := githubAPIBaseURL
	githubAPIBaseURL = srv.URL
	t.Cleanup(func() { githubAPIBaseURL = old })

	return srv
}

func TestCheckForUpdateSkipsDevBuild(t *testing.T) {
	old := companionVersion
	companionVersion = "dev"
	defer func() { companionVersion = old }()

	info, err := CheckForUpdate()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if info != nil {
		t.Fatalf("expected nil UpdateInfo for a dev build, got %+v", info)
	}
}

func TestCheckForUpdateReturnsNewerVersion(t *testing.T) {
	fakeGitHubReleaseServer(t, "v0.2.0", nil)

	oldVersion := companionVersion
	companionVersion = "v0.1.0"
	defer func() { companionVersion = oldVersion }()

	info, err := CheckForUpdate()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if info == nil {
		t.Fatal("expected an UpdateInfo for a newer release, got nil")
	}
	if info.Version != "v0.2.0" {
		t.Fatalf("unexpected version: %q", info.Version)
	}
}

func TestCheckForUpdateReturnsNilWhenAlreadyLatest(t *testing.T) {
	fakeGitHubReleaseServer(t, "v0.1.0", nil)

	oldVersion := companionVersion
	companionVersion = "v0.1.0"
	defer func() { companionVersion = oldVersion }()

	info, err := CheckForUpdate()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if info != nil {
		t.Fatalf("expected nil UpdateInfo when already on the latest release, got %+v", info)
	}
}

func TestVerifyChecksumAcceptsMatchingSum(t *testing.T) {
	data := []byte("hello world")
	sum := sha256.Sum256(data)
	sums := hex.EncodeToString(sum[:]) + "  xpmulticrew-linux.zip\n" +
		"deadbeef  some-other-file.zip\n"

	if err := verifyChecksum(sums, "xpmulticrew-linux.zip", data); err != nil {
		t.Fatalf("expected matching checksum to pass, got: %v", err)
	}
}

func TestVerifyChecksumRejectsMismatch(t *testing.T) {
	data := []byte("hello world")
	sums := "0000000000000000000000000000000000000000000000000000000000000000  xpmulticrew-linux.zip\n"

	if err := verifyChecksum(sums, "xpmulticrew-linux.zip", data); err == nil {
		t.Fatal("expected a checksum mismatch to be rejected")
	}
}

func TestVerifyChecksumRejectsMissingEntry(t *testing.T) {
	data := []byte("hello world")
	sums := "deadbeef  some-other-file.zip\n"

	if err := verifyChecksum(sums, "xpmulticrew-linux.zip", data); err == nil {
		t.Fatal("expected a missing checksum entry to be rejected")
	}
}

func TestExtractExecutableFindsNamedEntryIgnoresOthers(t *testing.T) {
	var buf bytes.Buffer
	w := zip.NewWriter(&buf)
	writeEntry := func(name string, content string) {
		f, err := w.Create(name)
		if err != nil {
			t.Fatalf("creating zip entry %q: %v", name, err)
		}
		if _, err := f.Write([]byte(content)); err != nil {
			t.Fatalf("writing zip entry %q: %v", name, err)
		}
	}
	writeEntry("xpmulticrew-companion.desktop", "[Desktop Entry]\n")
	writeEntry("xpmulticrew-companion", "binary-content-here")
	if err := w.Close(); err != nil {
		t.Fatalf("closing zip writer: %v", err)
	}

	got, err := extractExecutable(buf.Bytes(), "xpmulticrew-companion")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if string(got) != "binary-content-here" {
		t.Fatalf("unexpected extracted content: %q", got)
	}
}

func TestExtractExecutableErrorsWhenEntryMissing(t *testing.T) {
	var buf bytes.Buffer
	w := zip.NewWriter(&buf)
	f, _ := w.Create("something-else")
	_, _ = f.Write([]byte("x"))
	_ = w.Close()

	if _, err := extractExecutable(buf.Bytes(), "xpmulticrew-companion.exe"); err == nil {
		t.Fatal("expected an error when the named entry isn't present")
	}
}

func TestPrepareUpdateEndToEnd(t *testing.T) {
	zipName, exeName, ok := platformAssetName()
	if !ok {
		t.Skip("unsupported platform for this build - nothing to check")
	}

	var zipBuf bytes.Buffer
	w := zip.NewWriter(&zipBuf)
	f, err := w.Create(exeName)
	if err != nil {
		t.Fatalf("creating zip entry: %v", err)
	}
	const wantContent = "fake-updated-binary-content"
	if _, err := f.Write([]byte(wantContent)); err != nil {
		t.Fatalf("writing zip entry: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("closing zip writer: %v", err)
	}

	sum := sha256.Sum256(zipBuf.Bytes())
	sums := hex.EncodeToString(sum[:]) + "  " + zipName + "\n"

	fakeGitHubReleaseServer(t, "v9.9.9", map[string][]byte{
		zipName:      zipBuf.Bytes(),
		"SHA256SUMS": []byte(sums),
	})

	got, err := prepareUpdate()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if string(got) != wantContent {
		t.Fatalf("unexpected prepared executable content: %q", got)
	}
}

func TestPrepareUpdateRejectsTamperedZip(t *testing.T) {
	zipName, exeName, ok := platformAssetName()
	if !ok {
		t.Skip("unsupported platform for this build - nothing to check")
	}

	var zipBuf bytes.Buffer
	w := zip.NewWriter(&zipBuf)
	f, _ := w.Create(exeName)
	_, _ = f.Write([]byte("original content"))
	_ = w.Close()

	// Checksum recorded for entirely different bytes than what's served -
	// simulates a corrupted download or a tampered asset.
	sums := "0000000000000000000000000000000000000000000000000000000000000000  " + zipName + "\n"

	fakeGitHubReleaseServer(t, "v9.9.9", map[string][]byte{
		zipName:      zipBuf.Bytes(),
		"SHA256SUMS": []byte(sums),
	})

	if _, err := prepareUpdate(); err == nil {
		t.Fatal("expected a checksum-mismatched zip to be rejected")
	}
}

func TestPlatformAssetNameReturnsKnownPair(t *testing.T) {
	zipName, exeName, ok := platformAssetName()
	// This test runs on whatever OS built it, so just check internal
	// consistency rather than a specific platform.
	if !ok {
		t.Skip("unsupported platform for this build - nothing to check")
	}
	if zipName == "" || exeName == "" {
		t.Fatalf("expected non-empty names when ok=true, got zip=%q exe=%q", zipName, exeName)
	}
}
