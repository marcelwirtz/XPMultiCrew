export namespace main {
	
	export class AirportData {
	    airports: any[][];
	    runways: number[][];
	
	    static createFrom(source: any = {}) {
	        return new AirportData(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.airports = source["airports"];
	        this.runways = source["runways"];
	    }
	}
	export class Frequency {
	    type: string;
	    mhz: string;
	    name: string;
	
	    static createFrom(source: any = {}) {
	        return new Frequency(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.type = source["type"];
	        this.mhz = source["mhz"];
	        this.name = source["name"];
	    }
	}
	export class AirportDetail {
	    elevationFt: number;
	    runways: string[];
	    frequencies: Frequency[];
	
	    static createFrom(source: any = {}) {
	        return new AirportDetail(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.elevationFt = source["elevationFt"];
	        this.runways = source["runways"];
	        this.frequencies = this.convertValues(source["frequencies"], Frequency);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class NavPoint {
	    kind: string;
	    ident: string;
	    name: string;
	    airport?: string;
	    lat: number;
	    lon: number;
	    freq?: string;
	    magVar?: number;
	
	    static createFrom(source: any = {}) {
	        return new NavPoint(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.kind = source["kind"];
	        this.ident = source["ident"];
	        this.name = source["name"];
	        this.airport = source["airport"];
	        this.lat = source["lat"];
	        this.lon = source["lon"];
	        this.freq = source["freq"];
	        this.magVar = source["magVar"];
	    }
	}
	export class AirportInfo {
	    ident: string;
	    detail: AirportDetail;
	    vrps: NavPoint[];
	
	    static createFrom(source: any = {}) {
	        return new AirportInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.ident = source["ident"];
	        this.detail = this.convertValues(source["detail"], AirportDetail);
	        this.vrps = this.convertValues(source["vrps"], NavPoint);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class Airspace {
	    name: string;
	    class: string;
	    lowerFt: number;
	    lowerGnd: boolean;
	    upperFt: number;
	    poly: number[][];
	
	    static createFrom(source: any = {}) {
	        return new Airspace(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.class = source["class"];
	        this.lowerFt = source["lowerFt"];
	        this.lowerGnd = source["lowerGnd"];
	        this.upperFt = source["upperFt"];
	        this.poly = source["poly"];
	    }
	}
	export class ChooseXPlaneResult {
	    path: string;
	    warning?: string;
	
	    static createFrom(source: any = {}) {
	        return new ChooseXPlaneResult(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.path = source["path"];
	        this.warning = source["warning"];
	    }
	}
	export class DatarefInfo {
	    name: string;
	    type: string;
	    writable: boolean;
	    units?: string;
	    description?: string;
	    category: string;
	
	    static createFrom(source: any = {}) {
	        return new DatarefInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.type = source["type"];
	        this.writable = source["writable"];
	        this.units = source["units"];
	        this.description = source["description"];
	        this.category = source["category"];
	    }
	}
	
	export class LogLinesResult {
	    lines: string[];
	    offset: number;
	
	    static createFrom(source: any = {}) {
	        return new LogLinesResult(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.lines = source["lines"];
	        this.offset = source["offset"];
	    }
	}
	export class NavData {
	    points: NavPoint[];
	
	    static createFrom(source: any = {}) {
	        return new NavData(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.points = this.convertValues(source["points"], NavPoint);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	
	export class RouteWaypoint {
	    kind: string;
	    ident: string;
	    name: string;
	    lat: number;
	    lon: number;
	
	    static createFrom(source: any = {}) {
	        return new RouteWaypoint(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.kind = source["kind"];
	        this.ident = source["ident"];
	        this.name = source["name"];
	        this.lat = source["lat"];
	        this.lon = source["lon"];
	    }
	}
	export class PlannedRoute {
	    name: string;
	    cruiseFt: number;
	    tasKt: number;
	    waypoints: RouteWaypoint[];
	
	    static createFrom(source: any = {}) {
	        return new PlannedRoute(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.cruiseFt = source["cruiseFt"];
	        this.tasKt = source["tasKt"];
	        this.waypoints = this.convertValues(source["waypoints"], RouteWaypoint);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class PluginPrefs {
	    callsign: string;
	    showLabels: boolean;
	    envSync: boolean;
	
	    static createFrom(source: any = {}) {
	        return new PluginPrefs(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.callsign = source["callsign"];
	        this.showLabels = source["showLabels"];
	        this.envSync = source["envSync"];
	    }
	}
	export class ProfileEntry {
	    name: string;
	    stream: boolean;
	    category: string;
	    warning?: string;
	
	    static createFrom(source: any = {}) {
	        return new ProfileEntry(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.stream = source["stream"];
	        this.category = source["category"];
	        this.warning = source["warning"];
	    }
	}
	export class ProfileData {
	    icao: string;
	    source: string;
	    entries: ProfileEntry[];
	    validated: boolean;
	
	    static createFrom(source: any = {}) {
	        return new ProfileData(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.icao = source["icao"];
	        this.source = source["source"];
	        this.entries = this.convertValues(source["entries"], ProfileEntry);
	        this.validated = source["validated"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	
	export class ProfileInfo {
	    icao: string;
	    hasUser: boolean;
	    hasBundled: boolean;
	
	    static createFrom(source: any = {}) {
	        return new ProfileInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.icao = source["icao"];
	        this.hasUser = source["hasUser"];
	        this.hasBundled = source["hasBundled"];
	    }
	}
	
	export class SavedServer {
	    label: string;
	    hostPort: string;
	
	    static createFrom(source: any = {}) {
	        return new SavedServer(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.label = source["label"];
	        this.hostPort = source["hostPort"];
	    }
	}
	export class UpdateInfo {
	    version: string;
	
	    static createFrom(source: any = {}) {
	        return new UpdateInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.version = source["version"];
	    }
	}

}

