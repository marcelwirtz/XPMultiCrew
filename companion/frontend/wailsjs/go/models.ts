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

}

