/**
 *  NodeJs-Unreal v2.0.0 - process.js
 *
 *  Entry-point wrapper launched by the Unreal NodeComponent (via CLISystem) over
 *  stdin/stdout. All communication is a self-delimiting binary frame protocol so
 *  logs, control commands, events and raw binary can interweave on one pipe.
 *
 *  Frame: [4]MAGIC 'NUE\x01' [1]TYPE [4]headerLen [header utf8] [4]binLen [binary]
 *
 *  It can run user scripts two ways:
 *    - inline      : require()'d into this process (default, lowest latency)
 *    - subprocess  : fork()'d as a child (isolated, real IPC channel)
 *  Both expose the same `require('ipc-event-emitter').default(process)` API,
 *  bridged here to the Unreal side.
 */

const { fork } = require('child_process');
const path = require('path');
const fs = require('fs');
const util = require('util');
const childProcess = require('child_process');

const activeChildren = {};   // scriptName -> { child }
const watchedScripts = {};   // fullPath   -> fs watcher
const launchedScripts = {};  // fullPath   -> { scriptName, method, scriptPath }
const inlineEmitters = new Map(); // scriptName -> Set<emitter>
const npmAttempted = new Set();   // scriptName guard against install loops

let scriptRoot = '../../../../../';
let autoResolveNpm = true;        // toggled from Unreal via the npmAutoResolve control

// Resolve a script to a full path, preferring <projectRoot>/<scriptPath> and
// falling back to the plugin's own Content/Scripts (where process.js lives), so
// the bundled examples run without copying them into the project.
function resolveScriptFullPath(scriptName, scriptPath) {
	const primary = path.resolve(scriptRoot + scriptPath + scriptName);
	if (fs.existsSync(primary)) return primary;

	// __dirname == <plugin>/Content/Scripts ; step up to <plugin> then re-apply scriptPath.
	const fallback = path.resolve(__dirname, '..', '..', scriptPath + scriptName);
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
// Frame protocol
// ---------------------------------------------------------------------------

const MAGIC = Buffer.from([0x4E, 0x55, 0x45, 0x01]);
const T_LOG = 0x01, T_ACTION = 0x02, T_EVENT = 0x03, T_ERROR = 0x04,
	T_CONTROL = 0x05, T_PLOG = 0x06, T_NPM = 0x07;

// Capture the real stdout write before console is overridden.
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

function sendLog(msg) { writeFrame(T_LOG, msg); }
function plog(msg) { writeFrame(T_PLOG, msg); }
function sendAction(action) { writeFrame(T_ACTION, action); }
function sendError(scriptName, message, stack) {
	writeFrame(T_ERROR, JSON.stringify({ script: scriptName || '', message: message || '', stack: stack || '' }));
}
function sendNpmResult(installed, error) {
	writeFrame(T_NPM, JSON.stringify({ installed: !!installed, error: error || '' }));
}

// Route all script/console output through framed LOG messages.
console.log = (...a) => sendLog(fmt(a));
console.info = (...a) => sendLog(fmt(a));
console.warn = (...a) => sendLog(fmt(a));
console.error = (...a) => sendLog(fmt(a));

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

// Replace Buffer instances in an arg tree with { _bin: i } placeholders.
function extractBinaries(value, buffers) {
	if (Buffer.isBuffer(value)) {
		const idx = buffers.length;
		buffers.push(value);
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

// ---------------------------------------------------------------------------
// Unreal <-> script event bridge
// ---------------------------------------------------------------------------

function sendEventToUnreal(scriptName, name, args) {
	const buffers = [];
	const replaced = (args || []).map(a => extractBinaries(a, buffers));
	const header = JSON.stringify({ script: scriptName || '', name: name, args: replaced });
	writeFrame(T_EVENT, header, buildBinaryTable(buffers));
}

function deliverEventToScript(scriptName, name, args) {
	const set = inlineEmitters.get(scriptName);
	if (set && set.size) {
		for (const em of set) {
			try { em._deliver(name, args); }
			catch (e) { sendError(scriptName, e.message, e.stack); }
		}
		return;
	}
	const info = activeChildren[scriptName];
	if (info && info.child && info.child.connected) {
		info.child.send({ type: 'ipc-event-emitter', emit: [name, ...args] });
		return;
	}
	plog(`No live target for event '${name}' on script '${scriptName}'.`);
}

// Installed for inline scripts: require('ipc-event-emitter').default(process)
// finds this and binds to the currently-loading script.
globalThis.__unrealBridge = {
	sendEvent(scriptName, name, args) { sendEventToUnreal(scriptName, name, args); },
	registerInline(scriptName, emitter) {
		let set = inlineEmitters.get(scriptName);
		if (!set) { set = new Set(); inlineEmitters.set(scriptName, set); }
		set.add(emitter);
	},
};

// ---------------------------------------------------------------------------
// npm auto-resolve
// ---------------------------------------------------------------------------

function missingModuleFrom(message) {
	const m = /Cannot find module '([^']+)'/.exec(message || '');
	// Ignore relative/absolute path requires - only bare package names are installable.
	if (m && m[1] && !m[1].startsWith('.') && !path.isAbsolute(m[1])) {
		return m[1];
	}
	return null;
}

function resolveNpmAndRelaunch(scriptName, scriptPath, method, errMessage) {
	if (!autoResolveNpm) {
		return false;
	}
	const moduleName = missingModuleFrom(errMessage);
	if (!moduleName || npmAttempted.has(scriptName)) {
		return false;
	}

	// Only auto-install when the missing module is declared in the script's package.json.
	const fullPath = resolveScriptFullPath(scriptName, scriptPath);
	const pkgDir = findPackageDir(path.dirname(fullPath));
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
		sendNpmResult(false, `Missing module '${moduleName}' is not in package.json. Add it to enable auto-resolve.`);
		return false;
	}

	npmAttempted.add(scriptName);
	const npmCli = path.join(path.dirname(process.execPath), 'node_modules', 'npm', 'bin', 'npm-cli.js');
	plog(`Installing '${moduleName}' (npm install in ${pkgDir}) ...`);

	const npm = childProcess.execFile(
		process.execPath,
		[npmCli, 'install'],
		{ cwd: pkgDir },
		(error, stdout, stderr) => {
			if (error) {
				sendNpmResult(false, (stderr || error.message || '').toString().trim());
				return;
			}
			sendNpmResult(true, '');
			plog(`npm install complete, relaunching '${scriptName}'.`);
			if (method === 'inline') {
				launchInline(scriptName, scriptPath);
			} else {
				launchSubprocess(scriptName, scriptPath);
			}
		}
	);
	npm.on('error', (e) => sendNpmResult(false, e.message));
	return true;
}

// ---------------------------------------------------------------------------
// Script launching
// ---------------------------------------------------------------------------

function launchSubprocess(scriptName, scriptPath) {
	const fullPath = resolveScriptFullPath(scriptName, scriptPath);

	if (activeChildren[scriptName]) {
		plog(`Child process for "${scriptName}" is already running.`);
		return;
	}

	try {
		sendAction('begin ' + fullPath);

		// silent: pipe stdout/stderr so we can re-frame them.
		// advanced serialization: preserve Buffers across the IPC channel.
		const child = fork(fullPath, [], { silent: true, serialization: 'advanced' });

		child.on('message', (data) => {
			if (data && data.type === 'ipc-event-emitter' && Array.isArray(data.emit)) {
				const [name, ...rest] = data.emit;
				sendEventToUnreal(scriptName, name, rest);
			}
		});

		let lastError = '';
		if (child.stderr) {
			child.stderr.setEncoding('utf8');
			child.stderr.on('data', (err) => { lastError += err; });
		}
		if (child.stdout) {
			child.stdout.setEncoding('utf8');
			child.stdout.on('data', (msg) => {
				const trimmed = msg.toString();
				if (trimmed.length) sendLog(trimmed.replace(/\s+$/, ''));
			});
		}

		child.on('exit', (code) => {
			sendAction('end ' + fullPath);
			delete activeChildren[scriptName];
			if (code === 1 && lastError) {
				sendError(scriptName, lastError.trim());
				if (resolveNpmAndRelaunch(scriptName, scriptPath, 'child', lastError)) {
					// relaunch scheduled
				}
			}
		});

		activeChildren[scriptName] = { child };
		launchedScripts[fullPath] = { scriptName, method: 'child', scriptPath };
		plog(`Launched child process for "${scriptName}".`);
	} catch (error) {
		sendError(scriptName, error.message, error.stack);
	}
}

function launchInline(scriptName, scriptPath) {
	const fullPath = resolveScriptFullPath(scriptName, scriptPath);

	// Clear any prior inline emitters for this script so reload re-binds cleanly.
	inlineEmitters.delete(scriptName);

	try {
		let resolvedPath;
		try { resolvedPath = require.resolve(fullPath); }
		catch (e) { resolvedPath = null; }

		if (resolvedPath && require.cache[resolvedPath]) {
			sendAction('end ' + fullPath);
			delete require.cache[resolvedPath];
		}

		sendAction('begin ' + fullPath);

		// Bind the next ipc-event-emitter created during require to this script.
		globalThis.__currentInlineScript = scriptName;
		const loaded = require(fullPath);
		globalThis.__currentInlineScript = '';

		launchedScripts[fullPath] = { scriptName, method: 'inline', scriptPath };
		plog(`Module "${scriptName}" loaded.`);
		return loaded;
	} catch (error) {
		globalThis.__currentInlineScript = '';
		sendError(scriptName, error.message, error.stack);
		resolveNpmAndRelaunch(scriptName, scriptPath, 'inline', error.message);
		return null;
	}
}

function stopScript(scriptName) {
	const entry = Object.entries(launchedScripts).find(
		([, d]) => d.scriptName === scriptName
	);
	if (!entry) {
		plog(`"${scriptName}" is not currently running.`);
		return;
	}
	const [fullPath, { method }] = entry;

	try {
		if (method === 'inline') {
			const resolvedPath = require.resolve(fullPath);
			if (require.cache[resolvedPath]) {
				delete require.cache[resolvedPath];
			}
			inlineEmitters.delete(scriptName);
			sendAction('end ' + fullPath);
		} else if (method === 'child') {
			if (activeChildren[scriptName]) {
				activeChildren[scriptName].child.kill();
				delete activeChildren[scriptName];
			}
		}

		if (watchedScripts[fullPath]) {
			watchedScripts[fullPath].close();
			delete watchedScripts[fullPath];
		}
		delete launchedScripts[fullPath];
		npmAttempted.delete(scriptName);
	} catch (error) {
		sendError(scriptName, error.message, error.stack);
	}
}

function sendMessageToChild(scriptName, message) {
	const info = activeChildren[scriptName];
	if (!info) {
		plog(`No active child process for "${scriptName}".`);
		return;
	}
	info.child.send(message);
}

function watchScript(scriptName, scriptPath) {
	const debounceDuration = 100;
	const fullPath = resolveScriptFullPath(scriptName, scriptPath);

	if (watchedScripts[fullPath]) {
		return;
	}
	if (!launchedScripts[fullPath]) {
		plog(`"${scriptName}" has not been launched yet; cannot watch.`);
		return;
	}

	const { method } = launchedScripts[fullPath];
	let reloadTimeout;

	try {
		const reload = () => {
			sendAction('reload ' + fullPath);
			if (method === 'inline') {
				launchInline(scriptName, scriptPath);
			} else if (method === 'child') {
				if (activeChildren[scriptName]) {
					activeChildren[scriptName].child.kill();
					delete activeChildren[scriptName];
				}
				launchSubprocess(scriptName, scriptPath);
			}
		};

		const watcher = fs.watch(fullPath, (eventType) => {
			if (eventType === 'change') {
				if (reloadTimeout) clearTimeout(reloadTimeout);
				reloadTimeout = setTimeout(reload, debounceDuration);
			}
		});

		watchedScripts[fullPath] = watcher;
		plog(`Watching "${scriptName}" for changes.`);
	} catch (error) {
		sendError(scriptName, error.message, error.stack);
	}
}

// ---------------------------------------------------------------------------
// Control command dispatch (from Unreal via CONTROL frames)
// ---------------------------------------------------------------------------

function handleControl(commandLine) {
	const [command, ...args] = commandLine.trim().split(' ');

	switch (command) {
		case '': return;
		case 'launchInline': {
			const [scriptName, scriptPath] = args;
			if (scriptName && scriptPath) launchInline(scriptName, scriptPath);
			else plog('Usage: launchInline <scriptName> <scriptPath>');
			break;
		}
		case 'launchSubprocess': {
			const [scriptName, scriptPath] = args;
			if (scriptName && scriptPath) launchSubprocess(scriptName, scriptPath);
			else plog('Usage: launchSubprocess <scriptName> <scriptPath>');
			break;
		}
		case 'watch': {
			const [scriptName, scriptPath] = args;
			if (scriptName && scriptPath) watchScript(scriptName, scriptPath);
			else plog('Usage: watch <scriptName> <scriptPath>');
			break;
		}
		case 'stop': {
			const [scriptName] = args;
			if (scriptName) stopScript(scriptName);
			else plog('Usage: stop <scriptName>');
			break;
		}
		case 'send': {
			const [scriptName, ...messageParts] = args;
			if (scriptName && messageParts.length) sendMessageToChild(scriptName, messageParts.join(' '));
			else plog('Usage: send <scriptName> <message>');
			break;
		}
		case 'scriptsPath': {
			scriptRoot = args.join(' ');
			plog(`Updated scriptRoot to: ${scriptRoot}`);
			break;
		}
		case 'npmAutoResolve': {
			autoResolveNpm = (args[0] === '1' || args[0] === 'true');
			break;
		}
		case 'reloadComplete': {
			// Unreal acked the reload; nothing further required.
			break;
		}
		case 'exit': {
			for (const [scriptName, { child }] of Object.entries(activeChildren)) {
				try { child.kill(); } catch (e) { /* ignore */ }
			}
			for (const watcher of Object.values(watchedScripts)) {
				try { watcher.close(); } catch (e) { /* ignore */ }
			}
			process.exit(0);
			break;
		}
		default:
			plog(`Unknown command: ${command}`);
	}
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
			deliverEventToScript(obj.script || '', obj.name, args);
		} catch (e) {
			sendError('', 'event parse error: ' + e.message, e.stack);
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

// Keep the bridge alive even if a script throws asynchronously; surface it.
process.on('uncaughtException', (err) => {
	sendError('', err.message, err.stack);
});
process.on('unhandledRejection', (reason) => {
	const err = reason instanceof Error ? reason : new Error(String(reason));
	sendError('', err.message, err.stack);
});

plog('NodeJs-Unreal process bridge ready.');
