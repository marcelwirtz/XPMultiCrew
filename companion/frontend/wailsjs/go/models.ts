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
	export class AirportHit {
	    ident: string;
	    name: string;
	    lat: number;
	    lon: number;
	    distNm?: number;
	
	    static createFrom(source: any = {}) {
	        return new AirportHit(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.ident = source["ident"];
	        this.name = source["name"];
	        this.lat = source["lat"];
	        this.lon = source["lon"];
	        this.distNm = source["distNm"];
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
	export class Approach {
	    runway: string;
	    kind: string;
	    ident: string;
	    freq?: string;
	    courseMag?: number;
	    course: number;
	    glideDeg?: number;
	
	    static createFrom(source: any = {}) {
	        return new Approach(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.runway = source["runway"];
	        this.kind = source["kind"];
	        this.ident = source["ident"];
	        this.freq = source["freq"];
	        this.courseMag = source["courseMag"];
	        this.course = source["course"];
	        this.glideDeg = source["glideDeg"];
	    }
	}
	export class ParkingSpot {
	    name: string;
	    type: string;
	    x: number;
	    y: number;
	    heading: number;
	
	    static createFrom(source: any = {}) {
	        return new ParkingSpot(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.type = source["type"];
	        this.x = source["x"];
	        this.y = source["y"];
	        this.heading = source["heading"];
	    }
	}
	export class TaxiEdge {
	    name: string;
	    a: number[];
	    b: number[];
	
	    static createFrom(source: any = {}) {
	        return new TaxiEdge(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.a = source["a"];
	        this.b = source["b"];
	    }
	}
	export class LayoutRunwayEnd {
	    name: string;
	    x: number;
	    y: number;
	    headingTrue: number;
	    displacedM: number;
	    papiDeg?: number;
	
	    static createFrom(source: any = {}) {
	        return new LayoutRunwayEnd(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.x = source["x"];
	        this.y = source["y"];
	        this.headingTrue = source["headingTrue"];
	        this.displacedM = source["displacedM"];
	        this.papiDeg = source["papiDeg"];
	    }
	}
	export class LayoutRunway {
	    widthM: number;
	    lengthM: number;
	    surface: string;
	    ends: LayoutRunwayEnd[];
	
	    static createFrom(source: any = {}) {
	        return new LayoutRunway(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.widthM = source["widthM"];
	        this.lengthM = source["lengthM"];
	        this.surface = source["surface"];
	        this.ends = this.convertValues(source["ends"], LayoutRunwayEnd);
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
	export class AirportLayout {
	    ident: string;
	    name: string;
	    elevationFt: number;
	    lat: number;
	    lon: number;
	    magVar: number;
	    runways: LayoutRunway[];
	    pavement: number[][][][];
	    taxiways: TaxiEdge[];
	    parking: ParkingSpot[];
	    windsocks: number[][];
	    tower?: number[];
	    frequencies: Frequency[];
	    approaches: Approach[];
	
	    static createFrom(source: any = {}) {
	        return new AirportLayout(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.ident = source["ident"];
	        this.name = source["name"];
	        this.elevationFt = source["elevationFt"];
	        this.lat = source["lat"];
	        this.lon = source["lon"];
	        this.magVar = source["magVar"];
	        this.runways = this.convertValues(source["runways"], LayoutRunway);
	        this.pavement = source["pavement"];
	        this.taxiways = this.convertValues(source["taxiways"], TaxiEdge);
	        this.parking = this.convertValues(source["parking"], ParkingSpot);
	        this.windsocks = source["windsocks"];
	        this.tower = source["tower"];
	        this.frequencies = this.convertValues(source["frequencies"], Frequency);
	        this.approaches = this.convertValues(source["approaches"], Approach);
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
	
	export class ChecklistCondition {
	    key: string;
	    op: string;
	    value: number;
	
	    static createFrom(source: any = {}) {
	        return new ChecklistCondition(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.key = source["key"];
	        this.op = source["op"];
	        this.value = source["value"];
	    }
	}
	export class ChecklistItem {
	    challenge: string;
	    response: string;
	    condition?: ChecklistCondition;
	
	    static createFrom(source: any = {}) {
	        return new ChecklistItem(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.challenge = source["challenge"];
	        this.response = source["response"];
	        this.condition = this.convertValues(source["condition"], ChecklistCondition);
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
	export class Checklist {
	    title: string;
	    items: ChecklistItem[];
	
	    static createFrom(source: any = {}) {
	        return new Checklist(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.title = source["title"];
	        this.items = this.convertValues(source["items"], ChecklistItem);
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
	
	export class ChecklistFile {
	    icao: string;
	    source: string;
	    text: string;
	    lists: Checklist[];
	
	    static createFrom(source: any = {}) {
	        return new ChecklistFile(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.icao = source["icao"];
	        this.source = source["source"];
	        this.text = source["text"];
	        this.lists = this.convertValues(source["lists"], Checklist);
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
	    kind: string;
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
	        this.kind = source["kind"];
	        this.name = source["name"];
	        this.type = source["type"];
	        this.writable = source["writable"];
	        this.units = source["units"];
	        this.description = source["description"];
	        this.category = source["category"];
	    }
	}
	export class Landing {
	    key: string;
	    own: boolean;
	    senderId: number;
	    time: number;
	    lat: number;
	    lon: number;
	    heading: number;
	    vsFpm: number;
	    peakG: number;
	    gsKt: number;
	    driftDeg: number;
	    bounces: number;
	    flareM: number;
	    touchAndGo: boolean;
	    icao: string;
	    callsign: string;
	    airport?: string;
	    runway?: string;
	    runwayLengthM?: number;
	    runwayWidthM?: number;
	    pastThrM?: number;
	    centerlineM?: number;
	    score: number;
	    verdict: string;
	    notes: string[];
	
	    static createFrom(source: any = {}) {
	        return new Landing(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.key = source["key"];
	        this.own = source["own"];
	        this.senderId = source["senderId"];
	        this.time = source["time"];
	        this.lat = source["lat"];
	        this.lon = source["lon"];
	        this.heading = source["heading"];
	        this.vsFpm = source["vsFpm"];
	        this.peakG = source["peakG"];
	        this.gsKt = source["gsKt"];
	        this.driftDeg = source["driftDeg"];
	        this.bounces = source["bounces"];
	        this.flareM = source["flareM"];
	        this.touchAndGo = source["touchAndGo"];
	        this.icao = source["icao"];
	        this.callsign = source["callsign"];
	        this.airport = source["airport"];
	        this.runway = source["runway"];
	        this.runwayLengthM = source["runwayLengthM"];
	        this.runwayWidthM = source["runwayWidthM"];
	        this.pastThrM = source["pastThrM"];
	        this.centerlineM = source["centerlineM"];
	        this.score = source["score"];
	        this.verdict = source["verdict"];
	        this.notes = source["notes"];
	    }
	}
	export class PeerTrack {
	    id: number;
	    callsign: string;
	    icao: string;
	    track: number[][];
	
	    static createFrom(source: any = {}) {
	        return new PeerTrack(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.callsign = source["callsign"];
	        this.icao = source["icao"];
	        this.track = source["track"];
	    }
	}
	export class FlightEvent {
	    t: number;
	    kind: string;
	    label: string;
	    lat: number;
	    lon: number;
	
	    static createFrom(source: any = {}) {
	        return new FlightEvent(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.t = source["t"];
	        this.kind = source["kind"];
	        this.label = source["label"];
	        this.lat = source["lat"];
	        this.lon = source["lon"];
	    }
	}
	export class Flight {
	    id: string;
	    start: number;
	    end: number;
	    icao: string;
	    callsign: string;
	    departure: string;
	    arrival: string;
	    track: number[][];
	    events: FlightEvent[];
	    peers: PeerTrack[];
	    landings: Landing[];
	
	    static createFrom(source: any = {}) {
	        return new Flight(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.start = source["start"];
	        this.end = source["end"];
	        this.icao = source["icao"];
	        this.callsign = source["callsign"];
	        this.departure = source["departure"];
	        this.arrival = source["arrival"];
	        this.track = source["track"];
	        this.events = this.convertValues(source["events"], FlightEvent);
	        this.peers = this.convertValues(source["peers"], PeerTrack);
	        this.landings = this.convertValues(source["landings"], Landing);
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
	
	export class FlightSummary {
	    id: string;
	    start: number;
	    end: number;
	    icao: string;
	    callsign: string;
	    departure: string;
	    arrival: string;
	    airborneMin: number;
	    distanceNm: number;
	    maxAltFt: number;
	    landings: number;
	    bestScore: number;
	    peers: number;
	
	    static createFrom(source: any = {}) {
	        return new FlightSummary(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.start = source["start"];
	        this.end = source["end"];
	        this.icao = source["icao"];
	        this.callsign = source["callsign"];
	        this.departure = source["departure"];
	        this.arrival = source["arrival"];
	        this.airborneMin = source["airborneMin"];
	        this.distanceNm = source["distanceNm"];
	        this.maxAltFt = source["maxAltFt"];
	        this.landings = source["landings"];
	        this.bestScore = source["bestScore"];
	        this.peers = source["peers"];
	    }
	}
	
	
	export class LandingBoard {
	    session: Landing[];
	    log: Landing[];
	
	    static createFrom(source: any = {}) {
	        return new LandingBoard(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.session = this.convertValues(source["session"], Landing);
	        this.log = this.convertValues(source["log"], Landing);
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
	    rightSeat: boolean;
	
	    static createFrom(source: any = {}) {
	        return new PluginPrefs(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.callsign = source["callsign"];
	        this.showLabels = source["showLabels"];
	        this.envSync = source["envSync"];
	        this.rightSeat = source["rightSeat"];
	    }
	}
	export class ProfileEntry {
	    kind: string;
	    name: string;
	    stream: boolean;
	    category: string;
	    warning?: string;
	
	    static createFrom(source: any = {}) {
	        return new ProfileEntry(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.kind = source["kind"];
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
	
	export class RunwayEnd {
	    n: string;
	    la: number;
	    lo: number;
	    d: number;
	
	    static createFrom(source: any = {}) {
	        return new RunwayEnd(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.n = source["n"];
	        this.la = source["la"];
	        this.lo = source["lo"];
	        this.d = source["d"];
	    }
	}
	export class RunwayGeometry {
	    a: string;
	    w: number;
	    e: RunwayEnd[];
	
	    static createFrom(source: any = {}) {
	        return new RunwayGeometry(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.a = source["a"];
	        this.w = source["w"];
	        this.e = this.convertValues(source["e"], RunwayEnd);
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

