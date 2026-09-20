export namespace main {

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
	export class LanPeer {
	    name: string;
	    host: string;
	    port: number;

	    static createFrom(source: any = {}) {
	        return new LanPeer(source);
	    }

	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.host = source["host"];
	        this.port = source["port"];
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

