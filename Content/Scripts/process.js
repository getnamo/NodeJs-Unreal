/**
 *  NodeJs-Unreal v2.2.0 - process.js
 *
 *  Entry-point wrapper launched by the Unreal NodeComponent (via CLISystem) over
 *  stdin/stdout. All communication is a self-delimiting binary frame protocol so
 *  logs, control commands, events and raw binary can interweave on one pipe.
 *
 *  Frame: [4]MAGIC 'NUE\x01' [1]TYPE [4]headerLen [header utf8] [4]binLen [binary]
 *
 *  Scripts are keyed by (owner, scriptName). The owner is the Unreal component id,
 *  which lets several components share one node process (bShareMainProcess).
 *
 *  Launch modes:
 *    - inline      : loaded into this process (default, lowest latency). Stop/reload
 *                    clears the script's timers and listeners and fires 'shouldExit'.
 *    - worker      : worker_threads Worker. Stop/reload terminates it (hard stop).
 *    - subprocess  : fork()'d child with a real IPC channel. Graceful stop:
 *                    'shouldExit' -> disconnect -> kill.
 *  All three expose the same `require('ipc-event-emitter').default(process)` API.
 */

'use strict';

const { fork } = require('child_process');
const childProcess = require('child_process');
const path = require('path');
const fs = require('fs');
const util = require('util');
const Module = require('module');
const timersModule = require('timers');
const { AsyncLocalStorage } = require('async_hooks');
const { Worker } = require('worker_threads');

// Let scripts outside the plugin (e.g. {Project}/Content/Scripts) resolve the bundled
// node_modules (ipc-event-emitter). Forked children and workers inherit the env.
const BUNDLED_MODULES = path.join(__dirname, 'node_modules');
{
	const existing = process.env.NODE_PATH ? process.env.NODE_PATH.split(path.delimiter) : [];
	if (!existing.includes(BUNDLED_MODULES)) {
		process.env.NODE_PATH = [BUNDLED_MODULES, ...existing].join(path.delimiter);
		Module._initPaths();
	}
}

const WORKER_BOOTSTRAP = path.join(__dirname, 'workerBootstrap.js');
const CHILD_GUARD = path.join(__dirname, 'childGuard.js');
const SIGNAL_GRACE_MS = 200;
const EXIT_EVENT = 'shouldExit';
const SUBPROCESS_GRACE_MS = 200;
const WORKER_GRACE_MS = 100;
const WATCH_DEBOUNCE_MS = 100;

// ---------------------------------------------------------------------------
// Frame protocol
// ---------------------------------------------------------------------------

const MAGIC = Buffer.from([0x4E, 0x55, 0x45, 0x01]);
const T_LOG = 0x01, T_ACTION = 0x02, T_EVENT = 0x03, T_ERROR = 0x04,
	T_CONTROL = 0x05, T_PLOG = 0x06, T_NPM = 0x07, T_ACK = 0x08;

// Capture the real stdout write before anything is overridden.
const rawStdoutWrite = process.stdout.write.bind(process.stdout);

function u32le(n) {
	const b = Buffer.alloc(4);
	b.writeUInt32LE(n >>> 0, 0);
	return b;
}

function writeFrame(type, headerStr, binaryBuf) {
	const header = Buffer.from(headerStr != null ? String(headerStr) : '', 'utf8');
	const binary = binaryBuf || Buffer.alloc(0);
	const frame = Buffer.concat([
		MAGIC,
		Buffer.from([type]),
		u32le(header.length), header,
		u32le(binary.length), binary,
	]);
	rawStdoutWrite(frame);
}

function fmt(args) {
	return args.map(a => (typeof a === 'string' ? a : util.inspect(a))).join(' ');
}

function sendLog(owner, script, msg) {
	writeFrame(T_LOG, JSON.stringify({ owner: owner || '', script: script || '', msg: msg }));
}
function plog(msg) { writeFrame(T_PLOG, msg); }
function sendAction(owner, script, verb, fullPath, extra) {
	writeFrame(T_ACTION, JSON.stringify(Object.assign({ owner: owner || '', script: script || '', verb: verb, path: fullPath || '' }, extra)));
}
// 'end' carries the highest callback id this instance received, so Unreal only drops those.
function sendEnd(entry) {
	sendAction(entry.owner, entry.scriptName, 'end', entry.fullPath, { maxAck: entry.maxAck || 0 });
}
function sendError(owner, script, message, stack) {
	writeFrame(T_ERROR, JSON.stringify({ owner: owner || '', script: script || '', message: message || '', stack: stack || '' }));
}
function sendNpmResult(owner, script, installed, error) {
	writeFrame(T_NPM, JSON.stringify({ owner: owner || '', script: script || '', installed: !!installed, error: error || '' }));
}

// ---------------------------------------------------------------------------
// Per-script async context (inline attribution + cleanup)
// ---------------------------------------------------------------------------

// Everything an inline script schedules runs inside its context, so logs can be
// attributed and its timers found again on stop/reload.
const als = new AsyncLocalStorage();

function currentCtx() {
	return als.getStore() || null;
}

function logFromCurrent(text) {
	const ctx = currentCtx();
	sendLog(ctx ? ctx.owner : '', ctx ? ctx.scriptName : '', text);
}

// Route all script/console output through framed LOG messages.
console.log = (...a) => logFromCurrent(fmt(a));
console.info = (...a) => logFromCurrent(fmt(a));
console.warn = (...a) => logFromCurrent(fmt(a));
console.error = (...a) => logFromCurrent(fmt(a));
console.debug = (...a) => logFromCurrent(fmt(a));

// Direct process.stdout/stderr writes would corrupt the frame stream; frame them too.
function framedWrite(chunk, encoding, cb) {
	const text = Buffer.isBuffer(chunk) ? chunk.toString('utf8') : String(chunk);
	const trimmed = text.replace(/\s+$/, '');
	if (trimmed.length) logFromCurrent(trimmed);
	if (typeof encoding === 'function') encoding();
	else if (typeof cb === 'function') cb();
	return true;
}
process.stdout.write = framedWrite;
process.stderr.write = framedWrite;

const CTX = Symbol('nodeJsUnrealCtx');
const origTimers = {
	setTimeout: global.setTimeout,
	setInterval: global.setInterval,
	setImmediate: global.setImmediate,
	clearTimeout: global.clearTimeout,
	clearInterval: global.clearInterval,
	clearImmediate: global.clearImmediate,
};

function trackedSchedule(kind, schedule, clear) {
	return function (fn, ...rest) {
		const ctx = als.getStore();
		if (!ctx || typeof fn !== 'function') {
			return schedule(fn, ...rest);
		}
		if (ctx.dead) {
			// Leftover async work from a stopped script: don't let it re-arm timers.
			const h = schedule(() => {}, ...rest);
			clear(h);
			return h;
		}
		const once = kind !== 'interval';
		let handle = null;
		handle = schedule(function (...a) {
			if (once) ctx.timers.delete(handle);
			return fn.apply(this, a);
		}, ...rest);
		handle[CTX] = ctx;
		ctx.timers.set(handle, kind);
		return handle;
	};
}

function trackedClear(clear) {
	return function (handle) {
		if (handle && typeof handle === 'object' && handle[CTX]) {
			handle[CTX].timers.delete(handle);
		}
		return clear(handle);
	};
}

global.setTimeout = trackedSchedule('timeout', origTimers.setTimeout, origTimers.clearTimeout);
global.setInterval = trackedSchedule('interval', origTimers.setInterval, origTimers.clearInterval);
global.setImmediate = trackedSchedule('immediate', origTimers.setImmediate, origTimers.clearImmediate);
global.clearTimeout = trackedClear(origTimers.clearTimeout);
global.clearInterval = trackedClear(origTimers.clearInterval);
global.clearImmediate = trackedClear(origTimers.clearImmediate);
global.setTimeout[util.promisify.custom] = origTimers.setTimeout[util.promisify.custom];
global.setImmediate[util.promisify.custom] = origTimers.setImmediate[util.promisify.custom];
for (const name of Object.keys(origTimers)) {
	timersModule[name] = global[name];
}

// ---------------------------------------------------------------------------
// Binary interweaving helpers
// ---------------------------------------------------------------------------

function buildBinaryTable(buffers) {
	const parts = [u32le(buffers.length)];
	for (const b of buffers) {
		parts.push(u32le(b.length), b);
	}
	return Buffer.concat(parts);
}

function parseBinaryTable(buf) {
	const out = [];
	if (!buf || buf.length < 4) return out;
	let off = 0;
	const count = buf.readUInt32LE(off); off += 4;
	for (let i = 0; i < count; i++) {
		const len = buf.readUInt32LE(off); off += 4;
		out.push(Buffer.from(buf.subarray(off, off + len)));
		off += len;
	}
	return out;
}

// Replace Buffer/Uint8Array instances in an arg tree with { _bin: i } placeholders.
function extractBinaries(value, buffers) {
	if (value instanceof Uint8Array) {
		const idx = buffers.length;
		buffers.push(Buffer.isBuffer(value) ? value : Buffer.from(value.buffer, value.byteOffset, value.byteLength));
		return { _bin: idx };
	}
	if (Array.isArray(value)) {
		return value.map(v => extractBinaries(v, buffers));
	}
	if (value && typeof value === 'object') {
		const out = {};
		for (const k of Object.keys(value)) out[k] = extractBinaries(value[k], buffers);
		return out;
	}
	return value;
}

// Reverse of extractBinaries: swap { _bin: i } placeholders for real Buffers.
function injectBinaries(value, buffers) {
	if (value && typeof value === 'object') {
		if (typeof value._bin === 'number' && Object.keys(value).length === 1) {
			return buffers[value._bin];
		}
		if (Array.isArray(value)) {
			return value.map(v => injectBinaries(v, buffers));
		}
		const out = {};
		for (const k of Object.keys(value)) out[k] = injectBinaries(value[k], buffers);
		return out;
	}
	return value;
}

function sendEventToUnreal(owner, script, name, args) {
	const buffers = [];
	const replaced = (args || []).map(a => extractBinaries(a, buffers));
	const header = JSON.stringify({ owner: owner || '', script: script || '', name: name, args: replaced });
	writeFrame(T_EVENT, header, buildBinaryTable(buffers));
}

function sendAck(owner, script, ack, args, error) {
	const buffers = [];
	const replaced = (args || []).map(a => extractBinaries(a, buffers));
	const header = JSON.stringify({ owner: owner || '', script: script || '', ack: ack, args: replaced, error: error || '' });
	writeFrame(T_ACK, header, buildBinaryTable(buffers));
}

// ---------------------------------------------------------------------------
// Script registry
// ---------------------------------------------------------------------------

/**
 * entry: {
 *   key, owner, scriptName, scriptPath, fullPath, mode, args, npmAutoResolve,
 *   running, ctx (inline), module (inline), child (subprocess), worker (worker),
 *   watcher, watchReload
 * }
 */
const scripts = new Map();
const npmAttempted = new Set(); // entry keys, guards against install loops
const retiring = new Set();     // stopped children/workers still winding down (killed on shutdown)

let scriptRoot = '../../../../../';
let defaultNpmAutoResolve = true; // legacy global toggle (npmAutoResolve control)

const keyOf = (owner, scriptName) => (owner || '') + '|' + scriptName;

// Resolve a script to a full path, preferring <projectRoot>/<scriptPath> and
// falling back to the plugin's own Content/Scripts (where process.js lives), so
// the bundled examples run without copying them into the project.
function resolveScriptFullPath(scriptName, scriptPath) {
	const primary = path.resolve(scriptRoot, scriptPath || '', scriptName);
	if (fs.existsSync(primary)) return primary;

	// __dirname == <plugin>/Content/Scripts ; step up to <plugin> then re-apply scriptPath.
	const fallback = path.resolve(__dirname, '..', '..', scriptPath || '', scriptName);
	if (fs.existsSync(fallback)) return fallback;

	return primary; // default to the project path so messages point where users expect
}

// Walk up from a script's folder to the nearest package.json (npm install target).
function findPackageDir(startDir) {
	let dir = startDir;
	for (let i = 0; i < 8; i++) {
		if (fs.existsSync(path.join(dir, 'package.json'))) return dir;
		const parent = path.dirname(dir);
		if (parent === dir) break;
		dir = parent;
	}
	return startDir;
}

// ---------------------------------------------------------------------------
// Inline bridge (used by the bundled ipc-event-emitter)
// ---------------------------------------------------------------------------

globalThis.__unrealBridge = {
	version: 2,
	context() { return currentCtx(); },
	register(ctx, emitter) {
		if (ctx && !ctx.dead) ctx.emitters.add(emitter);
	},
	sendEvent(ctx, name, args) {
		if (ctx && ctx.dead) return;
		sendEventToUnreal(ctx ? ctx.owner : '', ctx ? ctx.scriptName : '', name, args);
	},
};

// ---------------------------------------------------------------------------
// npm
// ---------------------------------------------------------------------------

function missingModuleFrom(message) {
	const m = /Cannot find module '([^']+)'/.exec(message || '');
	// Ignore relative/absolute path requires - only bare package names are installable.
	if (m && m[1] && !m[1].startsWith('.') && !path.isAbsolute(m[1])) {
		return m[1];
	}
	return null;
}

// Run the bundled npm's `install` in pkgDir, report via an NPM frame, then call onDone(installed).
// Windows zips keep npm next to node.exe; Linux/macOS tarballs put it in ../lib beside bin/node.
function findNpmCli() {
	const nodeDir = path.dirname(process.execPath);
	const candidates = [
		path.join(nodeDir, 'node_modules', 'npm', 'bin', 'npm-cli.js'),
		path.join(nodeDir, '..', 'lib', 'node_modules', 'npm', 'bin', 'npm-cli.js'),
	];
	return candidates.find(p => fs.existsSync(p)) || candidates[0];
}

function runNpmInstall(owner, script, pkgDir, onDone) {
	const npmCli = findNpmCli();
	plog(`npm install in ${pkgDir} ...`);

	const npm = childProcess.execFile(
		process.execPath,
		[npmCli, 'install'],
		{ cwd: pkgDir },
		(error, stdout, stderr) => {
			if (error) {
				sendNpmResult(owner, script, false, (stderr || error.message || '').toString().trim());
				if (onDone) onDone(false);
				return;
			}
			sendNpmResult(owner, script, true, '');
			if (onDone) onDone(true);
		}
	);
	npm.on('error', (e) => sendNpmResult(owner, script, false, e.message));
}

// A script failed to load/run. If it's a missing module listed in package.json, install and relaunch.
function scriptFailed(entry, errMessage) {
	if (!entry.npmAutoResolve) return false;
	const moduleName = missingModuleFrom(errMessage);
	if (!moduleName || npmAttempted.has(entry.key)) return false;

	// Only auto-install when the missing module is declared in the script's package.json.
	const pkgDir = findPackageDir(path.dirname(entry.fullPath));
	const pkgPath = path.join(pkgDir, 'package.json');

	let listed = false;
	try {
		if (fs.existsSync(pkgPath)) {
			const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
			listed = !!(
				(pkg.dependencies && pkg.dependencies[moduleName]) ||
				(pkg.devDependencies && pkg.devDependencies[moduleName])
			);
		}
	} catch (e) {
		plog(`Could not read ${pkgPath}: ${e.message}`);
	}

	if (!listed) {
		plog(`Missing module '${moduleName}' is not listed in ${pkgPath}; not auto-installing.`);
		sendNpmResult(entry.owner, entry.scriptName, false, `Missing module '${moduleName}' is not in package.json. Add it to enable auto-resolve.`);
		return false;
	}

	npmAttempted.add(entry.key);
	plog(`Installing '${moduleName}' ...`);
	runNpmInstall(entry.owner, entry.scriptName, pkgDir, (installed) => {
		// Skip if the entry was removed, or already came back (e.g. a watch reload).
		if (!installed || scripts.get(entry.key) !== entry || entry.running) return;
		plog(`npm install complete, relaunching '${entry.scriptName}'.`);
		launchEntry(entry);
	});
	return true;
}

// Manual resolve (Unreal ResolveNpmDependencies): install the package.json nearest the script.
function resolveNpmForScript(owner, scriptName, scriptPath) {
	const fullPath = resolveScriptFullPath(scriptName, scriptPath);
	const pkgDir = findPackageDir(path.dirname(fullPath));
	if (!fs.existsSync(path.join(pkgDir, 'package.json'))) {
		sendNpmResult(owner, scriptName, false, `No package.json found for ${fullPath}`);
		return;
	}
	runNpmInstall(owner, scriptName, pkgDir);
}

// ---------------------------------------------------------------------------
// Script launching
// ---------------------------------------------------------------------------

function launchEntry(entry) {
	entry.fullPath = resolveScriptFullPath(entry.scriptName, entry.scriptPath);
	entry.running = true;
	sendAction(entry.owner, entry.scriptName, 'begin', entry.fullPath);

	if (entry.mode === 'subprocess') {
		launchSubprocess(entry);
	} else if (entry.mode === 'worker') {
		launchWorker(entry);
	} else {
		launchInline(entry);
	}
}

// Messages from a worker or forked child (ipc-event-emitter envelope or callback reply).
function handleScriptMessage(entry, data) {
	if (!data || typeof data !== 'object') return;
	if (data.type === 'ipc-event-emitter' && Array.isArray(data.emit) && data.emit.length) {
		const [name, ...rest] = data.emit;
		sendEventToUnreal(entry.owner, entry.scriptName, name, rest);
	} else if (data.type === 'nue-ack') {
		sendAck(entry.owner, entry.scriptName, data.ack, data.args || []);
	}
}

function launchInline(entry) {
	const ctx = {
		owner: entry.owner,
		scriptName: entry.scriptName,
		args: entry.args.slice(),
		timers: new Map(),   // handle -> kind
		emitters: new Set(),
		modules: [],         // require.cache keys first loaded by this script
		dead: false,
	};
	entry.ctx = ctx;
	entry.module = null;

	const before = new Set(Object.keys(require.cache));
	const savedArgv = process.argv;
	let loadError = null;
	process.argv = [process.execPath, entry.fullPath, ...entry.args];

	try {
		// Load a fresh module instance every time (not via require's cache) so reloads
		// re-run the file and two owners can run the same script independently.
		const m = new Module(entry.fullPath, module);
		m.filename = entry.fullPath;
		m.paths = Module._nodeModulePaths(path.dirname(entry.fullPath));
		entry.module = m;
		als.run(ctx, () => m.load(entry.fullPath));
		plog(`Module "${entry.scriptName}" loaded.`);
	} catch (error) {
		loadError = error;
	} finally {
		process.argv = savedArgv;
		const sep = path.sep;
		ctx.modules = Object.keys(require.cache).filter(k => !before.has(k) && !k.includes(`${sep}node_modules${sep}`));
	}

	if (loadError) {
		// Nothing is running: release anything the partial load scheduled.
		teardownInline(entry, false);
		entry.running = false;
		sendError(entry.owner, entry.scriptName, loadError.message, loadError.stack);
		sendEnd(entry);
		scriptFailed(entry, loadError.message);
	}
}

function teardownInline(entry, graceful) {
	const ctx = entry.ctx;
	if (!ctx || ctx.dead) return;

	if (graceful) {
		als.run(ctx, () => {
			for (const em of ctx.emitters) {
				try { em._deliver(EXIT_EVENT, []); }
				catch (e) { sendError(entry.owner, entry.scriptName, e.message, e.stack); }
			}
			const exp = entry.module && entry.module.exports;
			if (exp && typeof exp.dispose === 'function') {
				try { exp.dispose(); }
				catch (e) { sendError(entry.owner, entry.scriptName, e.message, e.stack); }
			}
		});
	}

	ctx.dead = true;
	for (const [handle, kind] of ctx.timers) {
		if (kind === 'immediate') origTimers.clearImmediate(handle);
		else origTimers.clearTimeout(handle); // also clears intervals
	}
	ctx.timers.clear();
	for (const em of ctx.emitters) em.removeAllListeners();
	ctx.emitters.clear();
	for (const k of ctx.modules) delete require.cache[k];
	entry.ctx = null;
	entry.module = null;
}

function launchWorker(entry) {
	let worker;
	try {
		worker = new Worker(WORKER_BOOTSTRAP, {
			workerData: { __nodeJsUnreal: true, fullPath: entry.fullPath, args: entry.args },
			argv: entry.args,
			stdout: true,
			stderr: true,
		});
	} catch (error) {
		sendError(entry.owner, entry.scriptName, error.message, error.stack);
		entry.running = false;
		sendEnd(entry);
		return;
	}
	entry.worker = worker;

	const forward = (chunk) => {
		const text = chunk.toString().replace(/\s+$/, '');
		if (text.length) sendLog(entry.owner, entry.scriptName, text);
	};
	worker.stdout.on('data', forward);
	worker.stderr.on('data', forward);

	worker.on('message', (data) => handleScriptMessage(entry, data));
	worker.on('error', (error) => {
		sendError(entry.owner, entry.scriptName, error.message, error.stack);
		if (entry.worker === worker) scriptFailed(entry, error.message);
	});
	worker.on('exit', () => {
		// Superseded (stopped/reloaded) workers already reported their end.
		if (entry.worker !== worker) return;
		entry.worker = null;
		entry.running = false;
		sendEnd(entry);
	});
	plog(`Launched worker for "${entry.scriptName}".`);
}

function stopWorker(entry, quick) {
	const worker = entry.worker;
	if (!worker) return;
	entry.worker = null;
	retiring.add(worker);
	worker.once('exit', () => retiring.delete(worker));
	try { worker.postMessage({ type: 'nue-exit' }); } catch (e) { /* already gone */ }
	origTimers.setTimeout(() => { worker.terminate().catch(() => {}); }, quick ? 0 : WORKER_GRACE_MS);
}

function launchSubprocess(entry) {
	let child;
	try {
		// silent: pipe stdout/stderr so we can re-frame them.
		// advanced serialization: preserve Buffers across the IPC channel.
		// childGuard: exit shortly after losing process.js, so scripts can't outlive Unreal.
		child = fork(entry.fullPath, entry.args, {
			silent: true,
			serialization: 'advanced',
			execArgv: process.execArgv.concat(['--require', CHILD_GUARD]),
		});
	} catch (error) {
		sendError(entry.owner, entry.scriptName, error.message, error.stack);
		entry.running = false;
		sendEnd(entry);
		return;
	}
	entry.child = child;

	child.on('message', (data) => handleScriptMessage(entry, data));

	let lastError = '';
	if (child.stderr) {
		child.stderr.setEncoding('utf8');
		child.stderr.on('data', (err) => { lastError += err; });
	}
	if (child.stdout) {
		child.stdout.setEncoding('utf8');
		child.stdout.on('data', (msg) => {
			const text = msg.toString().replace(/\s+$/, '');
			if (text.length) sendLog(entry.owner, entry.scriptName, text);
		});
	}
	child.on('error', (error) => sendError(entry.owner, entry.scriptName, error.message, error.stack));

	child.on('exit', (code) => {
		// Superseded (stopped/reloaded) children already reported their end.
		if (entry.child !== child) return;
		entry.child = null;
		entry.running = false;
		sendEnd(entry);
		if (code !== 0 && code !== null && lastError) {
			sendError(entry.owner, entry.scriptName, lastError.trim());
			scriptFailed(entry, lastError);
		} else if (lastError.trim().length) {
			sendLog(entry.owner, entry.scriptName, lastError.trim());
		}
	});
	plog(`Launched child process for "${entry.scriptName}".`);
}

// Graceful: 'shouldExit' -> disconnect (fires 'shouldExit' again if missed) -> kill.
function stopSubprocess(entry, quick) {
	const child = entry.child;
	if (!child) return;
	entry.child = null;
	if (child.exitCode === null && child.signalCode === null) {
		retiring.add(child);
		child.once('exit', () => retiring.delete(child));
	}
	try { if (child.connected) child.send({ type: 'nue-exit' }); } catch (e) { /* channel closed */ }
	origTimers.setTimeout(() => {
		try { if (child.connected) child.disconnect(); } catch (e) { /* ignore */ }
		origTimers.setTimeout(() => {
			if (child.exitCode === null && child.signalCode === null) {
				try { child.kill(); } catch (e) { /* ignore */ }
			}
		}, quick ? 0 : 100);
	}, quick ? 0 : SUBPROCESS_GRACE_MS);
}

// Stop whatever is running for this entry. Reports 'end' immediately; the old
// worker/child keeps winding down in the background.
function stopEntry(entry, { quick = false, keepWatcher = false } = {}) {
	const wasRunning = entry.running;
	entry.running = false;

	try {
		if (entry.mode === 'subprocess') {
			stopSubprocess(entry, quick);
		} else if (entry.mode === 'worker') {
			stopWorker(entry, quick);
		} else {
			teardownInline(entry, true);
		}
	} catch (error) {
		sendError(entry.owner, entry.scriptName, error.message, error.stack);
	}

	if (!keepWatcher && entry.watcher) {
		try { entry.watcher.close(); } catch (e) { /* ignore */ }
		entry.watcher = null;
	}
	if (wasRunning) {
		sendEnd(entry);
	}
}

function removeEntry(entry) {
	stopEntry(entry);
	scripts.delete(entry.key);
	npmAttempted.delete(entry.key);
}

function reloadEntry(entry) {
	stopEntry(entry, { keepWatcher: true });
	sendAction(entry.owner, entry.scriptName, 'reload', entry.fullPath);
	launchEntry(entry);
}

// ---------------------------------------------------------------------------
// Watching
// ---------------------------------------------------------------------------

function watchEntry(entry, reload) {
	entry.watchReload = reload !== false;
	if (entry.watcher) return;

	let debounce = null;
	try {
		entry.watcher = fs.watch(entry.fullPath, (eventType) => {
			if (eventType !== 'change') return;
			if (debounce) origTimers.clearTimeout(debounce);
			debounce = origTimers.setTimeout(() => {
				debounce = null;
				if (scripts.get(entry.key) !== entry) return;
				sendAction(entry.owner, entry.scriptName, 'changed', entry.fullPath);
				if (entry.watchReload) reloadEntry(entry);
			}, WATCH_DEBOUNCE_MS);
		});
		plog(`Watching "${entry.scriptName}" for changes.`);
	} catch (error) {
		sendError(entry.owner, entry.scriptName, error.message, error.stack);
	}
}

function unwatchEntry(entry) {
	if (entry.watcher) {
		try { entry.watcher.close(); } catch (e) { /* ignore */ }
		entry.watcher = null;
		plog(`Stopped watching "${entry.scriptName}".`);
	}
}

// ---------------------------------------------------------------------------
// Unreal -> script events
// ---------------------------------------------------------------------------

function deliverEventToScript(owner, scriptName, name, args, ack) {
	const entry = scripts.get(keyOf(owner, scriptName));
	const hasAck = ack !== undefined && ack !== null;

	if (entry && entry.running) {
		if (hasAck) entry.maxAck = Math.max(entry.maxAck || 0, ack);
		if (entry.mode === 'inline' && entry.ctx && entry.ctx.emitters.size) {
			const ctx = entry.ctx;
			const finalArgs = hasAck ? args.concat([makeInlineAck(entry, ack)]) : args;
			als.run(ctx, () => {
				for (const em of ctx.emitters) {
					try { em._deliver(name, finalArgs); }
					catch (e) { sendError(owner, scriptName, e.message, e.stack); }
				}
			});
			return;
		}
		const message = { type: 'ipc-event-emitter', emit: [name, ...args] };
		if (hasAck) message.ack = ack;
		if (entry.mode === 'worker' && entry.worker) {
			entry.worker.postMessage(message);
			return;
		}
		if (entry.mode === 'subprocess' && entry.child && entry.child.connected) {
			entry.child.send(message);
			return;
		}
	}
	plog(`No live target for event '${name}' on script '${scriptName}'.`);
	if (hasAck) sendAck(owner, scriptName, ack, [], 'no live target');
}

function makeInlineAck(entry, ack) {
	let done = false;
	return (...result) => {
		if (done) return;
		done = true;
		sendAck(entry.owner, entry.scriptName, ack, result);
	};
}

// ---------------------------------------------------------------------------
// Control command dispatch (from Unreal via CONTROL frames)
// ---------------------------------------------------------------------------

// v2.0 sent space-separated command lines; still accepted.
function parseLegacyControl(line) {
	const [command, ...args] = line.split(' ');
	switch (command) {
		case 'launchInline': return { cmd: 'launch', mode: 'inline', script: args[0], path: args[1] };
		case 'launchSubprocess': return { cmd: 'launch', mode: 'subprocess', script: args[0], path: args[1] };
		case 'watch': return { cmd: 'watch', script: args[0], path: args[1], reload: true };
		case 'stop': return { cmd: 'stop', script: args[0] };
		case 'npmInstall': return { cmd: 'npmInstall', script: args[0], path: args[1] };
		case 'scriptsPath': return { cmd: 'scriptsPath', path: args.join(' ') };
		case 'npmAutoResolve': return { cmd: 'npmAutoResolve', value: (args[0] === '1' || args[0] === 'true') };
		case 'send': return { cmd: 'send', script: args[0], message: args.slice(1).join(' ') };
		case 'reloadComplete': return null;
		case 'exit': return { cmd: 'exit', grace: 0 };
		default:
			plog(`Unknown command: ${command}`);
			return null;
	}
}

function handleControl(text) {
	const line = (text || '').trim();
	if (!line) return;

	let c;
	if (line[0] === '{') {
		try { c = JSON.parse(line); }
		catch (e) { plog(`Bad control json: ${e.message}`); return; }
	} else {
		c = parseLegacyControl(line);
	}
	if (!c) return;

	const owner = c.owner || '';
	const entry = c.script ? scripts.get(keyOf(owner, c.script)) : null;

	switch (c.cmd) {
		case 'launch': {
			if (!c.script) { plog('launch: missing script'); break; }
			if (entry) removeEntry(entry); // relaunch = restart
			const fresh = {
				key: keyOf(owner, c.script),
				owner: owner,
				scriptName: c.script,
				scriptPath: c.path || '',
				fullPath: '',
				mode: (c.mode === 'worker' || c.mode === 'subprocess') ? c.mode : 'inline',
				args: Array.isArray(c.args) ? c.args.map(String) : [],
				npmAutoResolve: (typeof c.npmAutoResolve === 'boolean') ? c.npmAutoResolve : defaultNpmAutoResolve,
				running: false,
				maxAck: 0,
				ctx: null, module: null, child: null, worker: null,
				watcher: null, watchReload: true,
			};
			scripts.set(fresh.key, fresh);
			launchEntry(fresh);
			if (c.watch) watchEntry(fresh, c.reload);
			break;
		}
		case 'stop': {
			if (entry) removeEntry(entry);
			else plog(`"${c.script}" is not currently running.`);
			break;
		}
		case 'stopOwner': {
			for (const e of Array.from(scripts.values())) {
				if (e.owner === owner) removeEntry(e);
			}
			break;
		}
		case 'watch': {
			if (entry) watchEntry(entry, c.reload);
			else plog(`"${c.script}" has not been launched yet; cannot watch.`);
			break;
		}
		case 'unwatch': {
			if (entry) unwatchEntry(entry);
			break;
		}
		case 'npmInstall': {
			if (c.script) resolveNpmForScript(owner, c.script, c.path || '');
			else plog('npmInstall: missing script');
			break;
		}
		case 'send': {
			if (entry && entry.child && entry.child.connected) entry.child.send(c.message);
			else plog(`No active child process for "${c.script}".`);
			break;
		}
		case 'scriptsPath': {
			scriptRoot = c.path || scriptRoot;
			plog(`Updated scriptRoot to: ${scriptRoot}`);
			break;
		}
		case 'npmAutoResolve': {
			defaultNpmAutoResolve = !!c.value;
			break;
		}
		case 'exit': {
			shutdown(c.grace);
			break;
		}
		default:
			plog(`Unknown command: ${c.cmd}`);
	}
}

// Stop everything: scripts get 'shouldExit' and up to graceMs to wind down, then any
// child/worker still alive is killed. Only then is 'exiting' acked, so nothing outlives us.
let shuttingDown = false;
function shutdown(graceMs) {
	if (shuttingDown) return;
	shuttingDown = true;
	for (const entry of Array.from(scripts.values())) {
		stopEntry(entry);
	}
	scripts.clear();

	const deadline = Date.now() + Math.max(0, Number(graceMs) || 0);
	const finish = () => {
		for (const r of retiring) {
			try {
				if (typeof r.kill === 'function') r.kill();
				else r.terminate();
			} catch (e) { /* already gone */ }
		}
		sendAction('', '', 'exiting', '');
		origTimers.setTimeout(() => process.exit(0), 20);
	};
	const poll = () => {
		if (retiring.size === 0 || Date.now() >= deadline) finish();
		else origTimers.setTimeout(poll, 10);
	};
	poll();
}

// ---------------------------------------------------------------------------
// Stdin frame decoding
// ---------------------------------------------------------------------------

let stdinBuf = Buffer.alloc(0);

function matchMagic(buf, i) {
	return i + 4 <= buf.length
		&& buf[i] === MAGIC[0] && buf[i + 1] === MAGIC[1]
		&& buf[i + 2] === MAGIC[2] && buf[i + 3] === MAGIC[3];
}

function findMagic(buf, start) {
	for (let i = start; i + 4 <= buf.length; i++) {
		if (matchMagic(buf, i)) return i;
	}
	return -1;
}

function handleFrame(type, header, binary) {
	if (type === T_CONTROL) {
		handleControl(header);
	} else if (type === T_EVENT) {
		try {
			const obj = JSON.parse(header);
			const buffers = parseBinaryTable(binary);
			const args = (obj.args || []).map(a => injectBinaries(a, buffers));
			deliverEventToScript(obj.owner || '', obj.script || '', obj.name, args, obj.ack);
		} catch (e) {
			sendError('', '', 'event parse error: ' + e.message, e.stack);
		}
	}
}

function parseStdin() {
	let cursor = 0;
	while (true) {
		if (stdinBuf.length - cursor < 9) break;

		if (!matchMagic(stdinBuf, cursor)) {
			const found = findMagic(stdinBuf, cursor + 1);
			if (found === -1) { cursor = Math.max(cursor, stdinBuf.length - 3); break; }
			cursor = found;
			continue;
		}

		let p = cursor + 4;
		const type = stdinBuf[p]; p += 1;
		const headerLen = stdinBuf.readUInt32LE(p); p += 4;
		if (stdinBuf.length < p + headerLen + 4) break;
		const header = stdinBuf.toString('utf8', p, p + headerLen); p += headerLen;
		const binLen = stdinBuf.readUInt32LE(p); p += 4;
		if (stdinBuf.length < p + binLen) break;
		const binary = Buffer.from(stdinBuf.subarray(p, p + binLen)); p += binLen;

		handleFrame(type, header, binary);
		cursor = p;
	}
	if (cursor > 0) stdinBuf = Buffer.from(stdinBuf.subarray(cursor));
}

process.stdin.on('data', (chunk) => {
	stdinBuf = Buffer.concat([stdinBuf, chunk]);
	parseStdin();
});

// Unreal closed our input pipe (component stopped): clean up and leave.
process.stdin.on('end', () => shutdown(100));

// Unreal's pipe reader is gone: writes would fail with EPIPE, just shut down.
process.stdout.on('error', () => shutdown(0));

// On Linux, Unreal's TerminateProc sends SIGTERM. Stop scripts (and their children) first.
for (const signal of ['SIGTERM', 'SIGINT', 'SIGHUP']) {
	try { process.on(signal, () => shutdown(SIGNAL_GRACE_MS)); } catch (e) { /* not supported on this platform */ }
}

// Keep the bridge alive even if a script throws asynchronously; surface it.
process.on('uncaughtException', (err) => {
	const ctx = currentCtx();
	sendError(ctx ? ctx.owner : '', ctx ? ctx.scriptName : '', err.message, err.stack);
});
process.on('unhandledRejection', (reason) => {
	const err = reason instanceof Error ? reason : new Error(String(reason));
	const ctx = currentCtx();
	sendError(ctx ? ctx.owner : '', ctx ? ctx.scriptName : '', err.message, err.stack);
});

plog('NodeJs-Unreal process bridge ready.');
