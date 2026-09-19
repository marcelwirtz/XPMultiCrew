// Command xpmulticrew-companion is the cross-platform desktop companion
// app for XPMultiCrew's X-Plane plugin. It handles all session-management
// UI (Formation/rendezvous connect, Shared Cockpit setup) as a native
// window built with Wails (Go + native webview), talking to the plugin's
// control listener (plugin/include/control/control_listener.h) over UDP -
// see plugin_client.go.
//
// This replaced an earlier version that ran a local HTTP server and opened
// a browser tab/app-mode window: that architecture had no reliable way to
// tell "window closed" from "browser did something weird" (a pagehide
// event fired unexpectedly and killed the backend mid-session). A real
// native window has a real, unambiguous close event, so that whole class
// of bug is gone by construction.
package main

import (
	"embed"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
)

//go:embed all:frontend/dist
var assets embed.FS

func main() {
	app := NewApp()

	err := wails.Run(&options.App{
		Title:            "XPMultiCrew",
		Width:            760,
		Height:           520,
		MinWidth:         600,
		MinHeight:        420,
		BackgroundColour: &options.RGBA{R: 20, G: 23, B: 28, A: 1},
		AssetServer: &assetserver.Options{
			Assets: assets,
		},
		OnStartup: app.startup,
		Bind: []interface{}{
			app,
		},
	})
	if err != nil {
		println("Error:", err.Error())
	}
}
