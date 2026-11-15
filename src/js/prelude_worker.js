
/* ---------------- the worker global ----------------
 * The last part of a worker's prelude (prelude.js, prelude_web.js,
 * prelude_net.js, this file). A worker has no document: its global is a
 * DedicatedWorkerGlobalScope with the APIs above, messages to and from
 * the Worker object in the page, importScripts and close. Worker,
 * SharedWorker and ServiceWorker are not defined here (a worker cannot
 * start workers). */
const workerInfo = N.workerInfo();
const settings = N.settings();
class WorkerGlobalScope extends EventTarget {
    constructor() { throw new TypeError('Illegal constructor'); }
}
class DedicatedWorkerGlobalScope extends WorkerGlobalScope {}
tag(WorkerGlobalScope); tag(DedicatedWorkerGlobalScope);
defGlobal('WorkerGlobalScope', WorkerGlobalScope);
defGlobal('DedicatedWorkerGlobalScope', DedicatedWorkerGlobalScope);
Object.setPrototypeOf(G, DedicatedWorkerGlobalScope.prototype);
/* Event handler attributes are own properties of the global, so that
 * "var onmessage = ..." and "onmessage = ..." both set the handler. */
defineHandlers(G, ['message', 'messageerror', 'error', 'unhandledrejection', 'rejectionhandled', 'languagechange', 'online', 'offline']);
for (const t of ['message', 'messageerror', 'error', 'unhandledrejection', 'rejectionhandled', 'languagechange', 'online', 'offline'])
    Object.defineProperty(G, 'on' + t, { enumerable: false });

class WorkerLocation {
    constructor() { throw new TypeError('Illegal constructor'); }
    get href() { return N.docUrl(0); }
    toString() { return N.docUrl(0); }
    get origin() { return N.docUrl(2); }
}
for (const k of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash'])
    Object.defineProperty(WorkerLocation.prototype, k, { get() { return new URL(N.docUrl(0))[k]; }, configurable: true });
tag(WorkerLocation);
const theLocation = Object.create(WorkerLocation.prototype);
class WorkerNavigator {
    constructor() { throw new TypeError('Illegal constructor'); }
    get userAgent() { return settings.userAgent; }
    get appCodeName() { return 'Mozilla'; }
    get appName() { return 'Netscape'; }
    get appVersion() { return settings.userAgent.replace(/^Mozilla\//, ''); }
    get platform() { return 'MacPPC'; }
    get product() { return 'Gecko'; }
    get language() { return settings.language; }
    get languages() { return Object.freeze([settings.language]); }
    get onLine() { return settings.online; }
    get hardwareConcurrency() { return 1; }
}
tag(WorkerNavigator);
const theNavigator = Object.create(WorkerNavigator.prototype);
const thePerformance = {
    now() { return N.now() - timeOriginNow; },
    get timeOrigin() { return timeOrigin; },
    mark() {}, measure() {}, clearMarks() {}, clearMeasures() {},
    getEntries() { return []; }, getEntriesByName() { return []; }, getEntriesByType() { return []; },
    toJSON() { return { timeOrigin }; },
};
let closing = false;
const globals = {
    get self() { return G; },
    get location() { return theLocation; },
    get navigator() { return theNavigator; },
    get performance() { return thePerformance; },
    get name() { return workerInfo[0]; },
    get origin() { return N.docUrl(2); },
    get isSecureContext() { return /^(https:|file:|blob:https:)/.test(N.docUrl(0)) || /^http:\/\/(localhost|127\.0\.0\.1)[:/]/.test(N.docUrl(0)); },
    get crossOriginIsolated() { return false; },
};
for (const k of Object.keys(globals)) {
    const d = Object.getOwnPropertyDescriptor(globals, k);
    Object.defineProperty(G, k, { get: d.get, set(v) { Object.defineProperty(G, k, { value: v, writable: true, configurable: true, enumerable: true }); }, configurable: true, enumerable: true });
}
defGlobal('WorkerLocation', WorkerLocation);
defGlobal('WorkerNavigator', WorkerNavigator);
defGlobal('postMessage', function postMessage(data, transfer) {
    if (!arguments.length) throw new TypeError('postMessage: 1 argument required');
    const [tree, cids, buffers] = encodeMessage(data, transfer);
    N.workerPostOut(tree, cids);
    detachAll(buffers);
});
defGlobal('close', function close() { closing = true; N.workerClose(); });
/* importScripts(urls...): each script is fetched and run before the call
 * returns. The worker's stack waits for the network while the page runs
 * (N.fetchSync); a worker without its own stack can import only blob: and
 * data: URLs. */
defGlobal('importScripts', function importScripts(...urls) {
    const list = urls.map((u) => {
        try { return new URL(str(u), N.docUrl(1)).href; } catch (e) { throw domErr('importScripts: invalid URL ' + u, 'SyntaxError'); }
    });
    for (const url of list) {
        let text = null;
        if (/^data:/i.test(url)) { const d = decodeDataUrl(url); if (d) text = N.utf8Decode(d[0], false, false); }
        else if (/^blob:/i.test(url)) { const b = blobUrls.get(url); if (b) text = N.utf8Decode(b._b, false, false); }
        else {
            let r = null;
            /* credentials same-origin (1), no-cors (1), a worker's script (4) */
            try { r = N.fetchSync('GET', url, [], null, 1, 1, 4, null, {}); } catch (e) { r = null; }
            if (r && r[0] >= 200 && r[0] < 300) text = N.utf8Decode(r[2], false, false);
        }
        if (text === null) throw domErr('importScripts: could not load ' + url, 'NetworkError');
        if (!N.runScript(text, url)) throw domErr('importScripts: the script at ' + url + ' threw', 'NetworkError');
    }
});

/* Uncaught errors also reach the Worker object in the page. */
const baseReportException = P.reportException;
P.reportException = function (err) {
    const shown = baseReportException(err);
    if (shown) {
        let file = N.docUrl(0), line = 0;
        if (err instanceof Error && typeof err.stack === 'string') {
            const m = /^\s*at (?:.*? \()?(.*?):(\d+)(?::\d+)?\)?$/m.exec(err.stack.replace(/^.*<null>.*$/gm, ''));
            if (m) { file = m[1]; line = +m[2]; }
        }
        N.workerError(err instanceof Error ? String(err) : inspect(err, 0, new Set()), file, line);
    }
    return shown;
};
/* From worker.c. kind 0: a message for the global; 1: for a port; with 16
 * added the message could not be read. */
P.workerDeliver = function (kind, id, tree, cids) {
    if (closing) return;
    if (kind === 1) { P.portDeliver(id, tree, cids); return; }
    if (kind & 16) { fire(G, new MessageEvent('messageerror', {})); return; }
    const ports = adoptEnds(cids);
    let data;
    try { data = scDecode(tree, ports); } catch (e) { fire(G, new MessageEvent('messageerror', {})); return; }
    fire(G, new MessageEvent('message', { data, ports }));
};
/* Runs the worker's script. */
P.startWorker = function (source, url, module) {
    if (module) runModuleScript(url, source, null);
    else N.runScript(source, url);
};

return P;
})
