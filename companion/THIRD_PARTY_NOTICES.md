# Third-party notices (companion app)

## MapLibre GL JS

Bundled into the companion's map page (`frontend/src/map.js`).

```
Copyright (c) 2023, MapLibre contributors

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of MapLibre GL JS nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

(MapLibre GL JS is a fork of Mapbox GL JS v1.13, which was BSD-3-Clause as
well; the full license text including that notice ships in
`frontend/node_modules/maplibre-gl/LICENSE.txt`.)

## Map data and tiles (not bundled)

The map page loads its base map at runtime from
[OpenFreeMap](https://openfreemap.org) (free, no API key, commercial use
allowed). The map shows the required attribution "OpenFreeMap © OpenMapTiles
Data from OpenStreetMap" in its corner; the map data is © OpenStreetMap
contributors, available under the Open Database License.

Airports and runways are read at runtime from the user's own X-Plane
installation (`Global Scenery/Global Airports/Earth nav data/apt.dat`) and
are never bundled or redistributed with this app.
