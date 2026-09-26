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

