// Engine-free verification for the NodeJs-Unreal v2.0.0 frame bridge.
//
// Spawns `node process.js`, talks the framed protocol to it exactly like the
// Unreal C++ side would, and validates:
//   1. inline adder round-trip       (myevent -> result)
//   2. subprocess adder round-trip   (fork + real IPC channel)
//   3. binary interweaving round-trip (binEcho echoes a Buffer unchanged)
//   ...plus the v2.1 lifecycle/event features (numbered sections 7+ below)
//
// Run:  <bundled node.exe>  test\harness.js     (cwd = Content/Scripts)
// Exit code 0 = all passed.

const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');

const SCRIPTS_DIR = path.resolve(__dirname, '..');
const PROCESS_JS = path.join(SCRIPTS_DIR, 'process.js');

// ---- frame protocol (mirror of process.js) ----
const MAGIC = Buffer.from([0x4E, 0x55, 0x45, 0x01]);
const T_LOG = 0x01, T_ACTION = 0x02, T_EVENT = 0x03, T_ERROR = 0x04,
	T_CONTROL = 0x05, T_PLOG = 0x06, T_NPM = 0x07, T_ACK = 0x08;

function u32le(n) { const b = Buffer.alloc(4); b.writeUInt32LE(n >>> 0, 0); return b; }

function frame(type, headerStr, binaryBuf) {
	const header = Buffer.from(headerStr || '', 'utf8');
	const binary = binaryBuf || Buffer.alloc(0);
	return Buffer.concat([MAGIC, Buffer.from([type]), u32le(header.length), header, u32le(binary.length), binary]);
}

function buildBinaryTable(buffers) {
	const parts = [u32le(buffers.length)];
	for (const b of buffers) parts.push(u32le(b.length), b);
	return Buffer.concat(parts);
}
function parseBinaryTable(buf) {
	const out = [];
	if (!buf || buf.length < 4) return out;
	let off = 0; const count = buf.readUInt32LE(off); off += 4;
	for (let i = 0; i < count; i++) { const len = buf.readUInt32LE(off); off += 4; out.push(Buffer.from(buf.subarray(off, off + len))); off += len; }
	return out;
}

function controlFrame(line) { return frame(T_CONTROL, line); }
function eventFrame(script, name, args, buffers) {
	const header = JSON.stringify({ script, name, args });
	return frame(T_EVENT, header, buildBinaryTable(buffers || []));
}
// v2.1: full event header ({ owner, script, name, args, ack })
function eventFrameObj(obj, buffers) {
	return frame(T_EVENT, JSON.stringify(obj), buildBinaryTable(buffers || []));
}

// ---- spawn process.js ----
const child = spawn(process.execPath, [PROCESS_JS], { cwd: SCRIPTS_DIR, stdio: ['pipe', 'pipe', 'inherit'] });

const listeners = []; // { predicate, resolve }
function waitFor(predicate, timeoutMs, label) {
	return new Promise((resolve, reject) => {
		const entry = { predicate, resolve };
		listeners.push(entry);
		const to = setTimeout(() => {
			const idx = listeners.indexOf(entry);
			if (idx >= 0) listeners.splice(idx, 1);
			reject(new Error('timeout waiting for ' + label));
		}, timeoutMs);
		entry.resolve = (v) => { clearTimeout(to); resolve(v); };
	});
}

function matchMagic(buf, i) { return i + 4 <= buf.length && buf[i] === MAGIC[0] && buf[i + 1] === MAGIC[1] && buf[i + 2] === MAGIC[2] && buf[i + 3] === MAGIC[3]; }
function findMagic(buf, start) { for (let i = start; i + 4 <= buf.length; i++) if (matchMagic(buf, i)) return i; return -1; }

// Returns a stdout 'data' handler that decodes frames and calls onFrame(type, header, binary).
function frameReader(onFrame) {
	let rxBuf = Buffer.alloc(0);
	return (chunk) => {
		rxBuf = Buffer.concat([rxBuf, chunk]);
		let cursor = 0;
		while (true) {
			if (rxBuf.length - cursor < 9) break;
			if (!matchMagic(rxBuf, cursor)) { const f = findMagic(rxBuf, cursor + 1); if (f === -1) { cursor = Math.max(cursor, rxBuf.length - 3); break; } cursor = f; continue; }
			let p = cursor + 4;
			const type = rxBuf[p]; p += 1;
			const hl = rxBuf.readUInt32LE(p); p += 4;
			if (rxBuf.length < p + hl + 4) break;
			const header = rxBuf.toString('utf8', p, p + hl); p += hl;
			const bl = rxBuf.readUInt32LE(p); p += 4;
			if (rxBuf.length < p + bl) break;
			const binary = Buffer.from(rxBuf.subarray(p, p + bl)); p += bl;
			onFrame(type, header, binary);
			cursor = p;
		}
		if (cursor > 0) rxBuf = Buffer.from(rxBuf.subarray(cursor));
	};
}

child.stdout.on('data', frameReader(dispatch));

function dispatch(type, header, binary) {
	const tag = { [T_LOG]: 'LOG', [T_PLOG]: 'PLOG', [T_ACTION]: 'ACTION', [T_EVENT]: 'EVENT', [T_ERROR]: 'ERROR', [T_NPM]: 'NPM', [T_ACK]: 'ACK' }[type] || ('0x' + type.toString(16));
	let parsed = null;
	if (type === T_EVENT || type === T_ACK) { try { parsed = JSON.parse(header); parsed._buffers = parseBinaryTable(binary); } catch (e) { /* */ } }
	console.error(`  <- ${tag} ${header.length > 120 ? header.slice(0, 120) + '...' : header}${binary.length ? ` [+${binary.length}b]` : ''}`);
	const msg = { type, tag, header, binary, parsed };
	for (let i = listeners.length - 1; i >= 0; i--) {
		if (listeners[i].predicate(msg)) { const l = listeners[i]; listeners.splice(i, 1); l.resolve(msg); }
	}
}

function send(buf) { child.stdin.write(buf); }
function ctl(obj) { send(controlFrame(JSON.stringify(obj))); }
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// Gather every matching frame for ms milliseconds.
function collect(predicate, ms) {
	return new Promise((resolve) => {
		const got = [];
		const entry = { predicate: (m) => { if (predicate(m)) got.push(m); return false; }, resolve: () => {} };
		listeners.push(entry);
		setTimeout(() => { listeners.splice(listeners.indexOf(entry), 1); resolve(got); }, ms);
	});
}
function jsonHeader(m) { try { return JSON.parse(m.header); } catch (e) { return null; } }
const isEvent = (name, owner) => (m) => m.type === T_EVENT && m.parsed && m.parsed.name === name && (owner === undefined || m.parsed.owner === owner);
const isAction = (verb, script, owner) => (m) => {
	if (m.type !== T_ACTION) return false;
	const a = jsonHeader(m);
	return !!a && a.verb === verb && (script === undefined || a.script === script) && (owner === undefined || a.owner === owner);
};
const isLog = (text, owner) => (m) => {
	if (m.type !== T_LOG) return false;
	const l = jsonHeader(m);
	return !!l && l.msg.includes(text) && (owner === undefined || l.owner === owner);
};
async function attempt(promise) { try { return await promise; } catch (e) { return null; } }
const MODES = ['inline', 'worker', 'subprocess'];

let failures = 0;
function check(cond, label) {
	console.error(`${cond ? 'PASS' : 'FAIL'} - ${label}`);
	if (!cond) failures++;
}

async function run() {
	await waitFor(m => m.type === T_PLOG && m.header.includes('ready'), 5000, 'bridge ready');

	// point scriptRoot at the Scripts dir so "examples/adder.js" resolves
	send(controlFrame('scriptsPath ' + SCRIPTS_DIR + path.sep));

	// ---- 1) inline adder ----
	send(controlFrame('launchInline adder.js examples' + path.sep));
	await waitFor(m => m.type === T_LOG && m.header.includes('started'), 5000, 'inline adder started');
	send(eventFrame('adder.js', 'myevent', [{ a: 3, b: 4 }]));
	{
		const m = await waitFor(m => m.type === T_EVENT && m.parsed && m.parsed.name === 'result', 5000, 'inline result');
		check(Math.abs(m.parsed.args[0] - 5) < 1e-9, 'inline adder: euclidean(3,4) == 5');
	}

	// ---- 2) subprocess adder ----
	send(controlFrame('launchSubprocess adder.js examples' + path.sep));
	await waitFor(m => m.type === T_LOG && m.header.includes('started'), 5000, 'subprocess adder started');
	send(eventFrame('adder.js', 'myevent', [{ a: 5, b: 12 }]));
	{
		const m = await waitFor(m => m.type === T_EVENT && m.parsed && m.parsed.name === 'result', 5000, 'subprocess result');
		check(Math.abs(m.parsed.args[0] - 13) < 1e-9, 'subprocess adder: euclidean(5,12) == 13');
	}

	// ---- 3) binary round-trip (inline) ----
	send(controlFrame('launchInline binEcho.js examples' + path.sep));
	await waitFor(m => m.type === T_LOG && m.header.includes('binEcho ready'), 5000, 'binEcho started');
	const payload = Buffer.from([1, 2, 3, 250, 251, 252, 0, 255, 10, 13]);
	send(eventFrame('binEcho.js', 'echo', [{ tag: 'hi' }, { _bin: 0 }], [payload]));
	{
		const m = await waitFor(m => m.type === T_EVENT && m.parsed && m.parsed.name === 'echoed', 5000, 'binary echo');
		const bufs = m.parsed._buffers;
		check(bufs.length === 1 && bufs[0].equals(payload), 'binary interweaving: echoed buffer matches byte-for-byte');
		check(m.parsed.args[0] && m.parsed.args[0].tag === 'hi', 'binary interweaving: meta arg preserved');
	}

	// ---- 4) large-data / throughput (subprocess, many binary frames) ----
	send(controlFrame('launchSubprocess perfStream.js examples' + path.sep));
	await waitFor(m => m.type === T_LOG && m.header.includes('perfStream ready'), 5000, 'perfStream started');
	const COUNT = 64, SIZE = 128 * 1024; // 64 x 128KB = 8 MB
	let chunks = 0;
	let allChunksValid = true;
	const collector = (m) => {
		if (m.type === T_EVENT && m.parsed && m.parsed.name === 'chunk') {
			chunks++;
			if (!(m.parsed._buffers.length === 1 && m.parsed._buffers[0].length === SIZE)) allChunksValid = false;
		}
	};
	listeners.push({ predicate: (m) => { collector(m); return false; }, resolve: () => {} });
	send(eventFrame('perfStream.js', 'start', [{ size: SIZE, count: COUNT }]));
	{
		const m = await waitFor(m => m.type === T_EVENT && m.parsed && m.parsed.name === 'done', 15000, 'perf done');
		check(chunks === COUNT, `throughput: received all ${COUNT} chunks (got ${chunks})`);
		check(allChunksValid, `throughput: every chunk was a ${SIZE}-byte buffer`);
		console.error(`  perf: ${m.parsed.args[0].mb} MB emitted in ${m.parsed.args[0].ms} ms`);
	}

	// ---- 5) npm auto-resolve: missing module not listed in package.json ----
	send(controlFrame('launchInline needsMissing.js test' + path.sep));
	{
		const m = await waitFor(m => m.type === T_NPM, 5000, 'npm result');
		const npm = JSON.parse(m.header);
		check(npm.installed === false && /not in package\.json/i.test(npm.error), 'npm auto-resolve: unlisted module warned, not installed');
	}

	// ---- 5b) manual npm install (ResolveNpmDependencies) against a throwaway package.json ----
	{
		const fs = require('fs');
		const os = require('os');
		const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'nue-npm-'));
		fs.writeFileSync(path.join(tmpRoot, 'package.json'), JSON.stringify({ name: 'nue-npm-test', version: '1.0.0', private: true, dependencies: {} }));
		send(controlFrame('scriptsPath ' + tmpRoot + path.sep));
		send(controlFrame('npmInstall any.js ./'));
		const m = await waitFor(m => m.type === T_NPM, 60000, 'manual npm install result');
		const npm = JSON.parse(m.header);
		check(npm.installed === true, 'npm manual install: bundled npm ran install successfully' + (npm.error ? ` (${npm.error})` : ''));
		try { fs.rmSync(tmpRoot, { recursive: true, force: true }); } catch (e) { /* */ }
	}

	// ---- 6) path fallback: project root lacks the script -> plugin Content/Scripts ----
	send(controlFrame('scriptsPath ' + path.join('C:', 'nonexistent_root_' + 'xyz') + path.sep));
	send(controlFrame('launchInline examples' + path.sep + 'adder.js Content' + path.sep + 'Scripts' + path.sep));
	await waitFor(m => m.type === T_LOG && m.header.includes('started'), 5000, 'fallback adder started');
	send(eventFrame('examples' + path.sep + 'adder.js', 'myevent', [{ a: 8, b: 15 }]));
	{
		const m = await waitFor(m => m.type === T_EVENT && m.parsed && m.parsed.name === 'result', 5000, 'fallback result');
		check(Math.abs(m.parsed.args[0] - 17) < 1e-9, 'path fallback: script found under plugin Content/Scripts');
	}

	// ====================== v2.1 ======================
	send(controlFrame('scriptsPath ' + SCRIPTS_DIR + path.sep));
	const T = 'test' + path.sep;

	// ---- 7) stop per launch mode: shouldExit runs, then the script really stops ----
	for (const mode of MODES) {
		const owner = 'stop-' + mode;
		ctl({ cmd: 'launch', owner, script: 'ticker.js', path: T, mode });
		await waitFor(isEvent('tick', owner), 5000, mode + ' first tick');
		const byeP = attempt(waitFor(isEvent('bye', owner), 2000, mode + ' bye'));
		ctl({ cmd: 'stop', owner, script: 'ticker.js' });
		check(!!(await byeP), `${mode} stop: shouldExit handler ran`);
		await sleep(mode === 'subprocess' ? 500 : 250); // graceful windows
		const late = await collect(isEvent('tick', owner), 300);
		check(late.length === 0, `${mode} stop: no ticks after stop (got ${late.length})`);
	}

	// ---- 8) inline relaunch/reload leaves exactly one instance running ----
	{
		const owner = 'reload';
		ctl({ cmd: 'launch', owner, script: 'ticker.js', path: T, mode: 'inline' });
		const first = await waitFor(isEvent('tick', owner), 5000, 'reload first tick');
		ctl({ cmd: 'launch', owner, script: 'ticker.js', path: T, mode: 'inline' });
		await sleep(50);
		const ticks = await collect(isEvent('tick', owner), 300);
		const ids = new Set(ticks.map(t => t.parsed.args[0].id));
		check(ticks.length > 0 && ids.size === 1 && !ids.has(first.parsed.args[0].id),
			`inline relaunch: only the new instance ticks (instances seen: ${ids.size})`);
		ctl({ cmd: 'stop', owner, script: 'ticker.js' });
	}

	// ---- 9) project scripts + paths with spaces + watch/unwatch ----
	// A temp "project" outside the plugin: exercises resolution of the bundled
	// ipc-event-emitter (NODE_PATH) and JSON control paths containing spaces.
	{
		const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'nue project '));
		const scriptDir = path.join(tmpRoot, 'Content', 'Scripts');
		fs.mkdirSync(scriptDir, { recursive: true });
		const scriptFile = path.join(scriptDir, 'my ticker.js');
		fs.writeFileSync(scriptFile, fs.readFileSync(path.join(SCRIPTS_DIR, 'test', 'ticker.js'), 'utf8'));
		ctl({ cmd: 'scriptsPath', path: tmpRoot + path.sep });
		const sp = 'Content' + path.sep + 'Scripts' + path.sep;
		const S = 'my ticker.js';
		const owner = 'watch';

		ctl({ cmd: 'launch', owner, script: S, path: sp, mode: 'inline', watch: true, reload: false });
		const first = await attempt(waitFor(isEvent('tick', owner), 5000, 'project script ticks'));
		check(!!first, 'project script (path with spaces) resolves the bundled ipc-event-emitter inline');
		if (first) {
			const firstId = first.parsed.args[0].id;

			fs.appendFileSync(scriptFile, '\n// edit 1\n');
			check(!!(await attempt(waitFor(isAction('changed', S, owner), 3000, 'changed'))), 'watch reload=false: change reported');
			const kept = await collect(isEvent('tick', owner), 250);
			check(kept.length > 0 && kept.every(t => t.parsed.args[0].id === firstId), 'watch reload=false: running instance kept');

			ctl({ cmd: 'watch', owner, script: S, reload: true });
			fs.appendFileSync(scriptFile, '\n// edit 2\n');
			check(!!(await attempt(waitFor(isAction('reload', S, owner), 3000, 'reload'))), 'watch reload=true: reload reported');
			await sleep(50);
			const swapped = await collect(isEvent('tick', owner), 250);
			const ids = new Set(swapped.map(t => t.parsed.args[0].id));
			check(swapped.length > 0 && ids.size === 1 && !ids.has(firstId), 'watch reload=true: one fresh instance after reload');

			ctl({ cmd: 'unwatch', owner, script: S });
			await sleep(50);
			fs.appendFileSync(scriptFile, '\n// edit 3\n');
			const ignored = await collect(isAction('changed', S, owner), 400);
			check(ignored.length === 0, 'unwatch: later edits ignored');
		}
		ctl({ cmd: 'stop', owner, script: S });

		for (const mode of ['worker', 'subprocess']) {
			const o = 'proj-' + mode;
			ctl({ cmd: 'launch', owner: o, script: S, path: sp, mode });
			check(!!(await attempt(waitFor(isEvent('tick', o), 5000, o))), `project script resolves the bundled ipc-event-emitter in ${mode} mode`);
			ctl({ cmd: 'stop', owner: o, script: S });
		}
		await sleep(500);
		ctl({ cmd: 'scriptsPath', path: SCRIPTS_DIR + path.sep });
		try { fs.rmSync(tmpRoot, { recursive: true, force: true }); } catch (e) { /* still locked by a winding-down child */ }
	}

	// ---- 10) launch args ----
	for (const mode of MODES) {
		const owner = 'args-' + mode;
		const p = attempt(waitFor(isEvent('args', owner), 5000, mode + ' args'));
		ctl({ cmd: 'launch', owner, script: 'argsEcho.js', path: T, mode, args: ['a b', 'c'] });
		const m = await p;
		const a = m && m.parsed.args[0];
		check(!!a && JSON.stringify(a.ipcArgs) === '["a b","c"]' && JSON.stringify(a.argv) === '["a b","c"]',
			`${mode} args: ipc.args and process.argv are ["a b","c"] (${JSON.stringify(a)})`);
		ctl({ cmd: 'stop', owner, script: 'argsEcho.js' });
	}

	// ---- 11) Emit Event With Callback: reply routed back (json + binary) ----
	for (const mode of MODES) {
		const owner = 'ack-' + mode;
		const ready = attempt(waitFor(isLog('acker ready', owner), 5000, mode + ' acker ready'));
		ctl({ cmd: 'launch', owner, script: 'acker.js', path: T, mode });
		await ready;
		send(eventFrameObj({ owner, script: 'acker.js', name: 'ask', args: [{ n: 21 }], ack: 42 }));
		const m = await attempt(waitFor(x => x.type === T_ACK && x.parsed && x.parsed.owner === owner, 5000, mode + ' ack'));
		check(!!m && m.parsed.ack === 42 && m.parsed.args[0].doubled === 42
			&& m.parsed._buffers.length === 1 && m.parsed._buffers[0].equals(Buffer.from([7, 8, 9])),
			`${mode} callback: reply carries json + binary back to the caller`);
		const end = attempt(waitFor(isAction('end', 'acker.js', owner), 3000, mode + ' acker end'));
		ctl({ cmd: 'stop', owner, script: 'acker.js' });
		const e = await end;
		check(!!e && jsonHeader(e).maxAck === 42, `${mode} callback: end reports the highest callback id it received`);
	}

	// ---- 11b) callback to a script that isn't running fails fast instead of leaking ----
	{
		send(eventFrameObj({ owner: 'ack-none', script: 'nosuch.js', name: 'ask', args: [1], ack: 99 }));
		const m = await attempt(waitFor(x => x.type === T_ACK && x.parsed && x.parsed.ack === 99, 3000, 'no-target ack'));
		check(!!m && m.parsed.error === 'no live target', 'callback: no live target answers with an error ack');
	}

	// ---- 12) multiple binary buffers per event (worker exercises Uint8Array rewrap) ----
	for (const mode of MODES) {
		const owner = 'multi-' + mode;
		const ready = attempt(waitFor(isLog('multiEcho ready', owner), 5000, mode + ' multiEcho ready'));
		ctl({ cmd: 'launch', owner, script: 'multiEcho.js', path: T, mode });
		await ready;
		const b0 = Buffer.from([1, 2, 3]);
		const b1 = Buffer.alloc(1000, 5);
		send(eventFrameObj({ owner, script: 'multiEcho.js', name: 'echo', args: [{ tag: 'x' }, { _bin: 0 }, { nested: { _bin: 1 } }] }, [b0, b1]));
		const m = await attempt(waitFor(isEvent('echoed', owner), 5000, mode + ' echoed'));
		check(!!m && m.parsed._buffers.length === 2 && m.parsed._buffers[0].equals(b0) && m.parsed._buffers[1].equals(b1)
			&& m.parsed.args[0].tag === 'x' && m.parsed.args[2].nested._bin === 1,
			`${mode} multi-buffer: both buffers round-trip in place`);
		ctl({ cmd: 'stop', owner, script: 'multiEcho.js' });
	}

	// ---- 13) owners: same script under two owners stays isolated; stopOwner ----
	{
		const readyA = attempt(waitFor(isLog('multiEcho ready', 'A'), 5000, 'A ready'));
		const readyB = attempt(waitFor(isLog('multiEcho ready', 'B'), 5000, 'B ready'));
		ctl({ cmd: 'launch', owner: 'A', script: 'multiEcho.js', path: T, mode: 'inline' });
		ctl({ cmd: 'launch', owner: 'B', script: 'multiEcho.js', path: T, mode: 'inline' });
		check(!!(await readyA) && !!(await readyB), 'owners: same script loads independently for two owners');

		const leakedToB = collect(isEvent('echoed', 'B'), 400);
		send(eventFrameObj({ owner: 'A', script: 'multiEcho.js', name: 'echo', args: [1] }));
		const a = await attempt(waitFor(isEvent('echoed', 'A'), 3000, 'A echoed'));
		check(!!a && (await leakedToB).length === 0, 'owners: event reaches only its owner');

		const endA = attempt(waitFor(isAction('end', 'multiEcho.js', 'A'), 2000, 'A end'));
		ctl({ cmd: 'stopOwner', owner: 'A' });
		check(!!(await endA), 'stopOwner: owner A scripts ended');
		send(eventFrameObj({ owner: 'B', script: 'multiEcho.js', name: 'echo', args: [2] }));
		check(!!(await attempt(waitFor(isEvent('echoed', 'B'), 3000, 'B echoed'))), 'stopOwner: owner B unaffected');
		ctl({ cmd: 'stop', owner: 'B', script: 'multiEcho.js' });
	}

	// ---- 14) shutdown: exiting ack, bridge exits, subprocess scripts don't linger ----
	// 'late' was already stopped (still winding down) when exit arrives, as in shared mode.
	{
		ctl({ cmd: 'launch', owner: 'shut', script: 'ticker.js', path: T, mode: 'subprocess' });
		ctl({ cmd: 'launch', owner: 'late', script: 'ticker.js', path: T, mode: 'subprocess' });
		const t = await waitFor(isEvent('tick', 'shut'), 5000, 'shutdown ticker');
		const t2 = await waitFor(isEvent('tick', 'late'), 5000, 'late ticker');
		const pids = [t.parsed.args[0].pid, t2.parsed.args[0].pid];
		const byeShut = attempt(waitFor(isEvent('bye', 'shut'), 2000, 'shut bye'));
		const exited = new Promise(r => child.on('exit', r));
		const ack = attempt(waitFor(isAction('exiting'), 3000, 'exiting'));
		ctl({ cmd: 'stopOwner', owner: 'late' });
		ctl({ cmd: 'exit', grace: 300 });
		check(!!(await byeShut), 'exit: running scripts get shouldExit before shutdown');
		check(!!(await ack), 'exit: bridge acknowledged with exiting');
		await Promise.race([exited, sleep(2000)]);
		check(child.exitCode !== null || child.signalCode !== null, 'exit: bridge process exited');
		await sleep(300);
		const alive = pids.filter(pid => { try { process.kill(pid, 0); return true; } catch (e) { return false; } });
		check(alive.length === 0, `exit: no subprocess script outlived the bridge (alive: ${alive.length})`);
		for (const pid of alive) { try { process.kill(pid); } catch (e) { /* */ } }
	}

	// ---- 15) bridge killed outright (crash / SIGKILL): subprocess scripts still exit ----
	// Uses a second bridge; the first one has exited. Linux doesn't reap orphans for us.
	for (const signal of ['SIGKILL', 'SIGTERM']) {
		const bridge = spawn(process.execPath, [PROCESS_JS], { cwd: SCRIPTS_DIR, stdio: ['pipe', 'pipe', 'inherit'] });
		let tickPid = null;
		bridge.stdout.on('data', frameReader((type, header) => {
			if (type !== T_EVENT) return;
			const e = JSON.parse(header);
			if (e.name === 'tick' && e.args[0]) tickPid = e.args[0].pid;
		}));
		bridge.stdin.write(controlFrame(JSON.stringify({ cmd: 'scriptsPath', path: SCRIPTS_DIR + path.sep })));
		bridge.stdin.write(controlFrame(JSON.stringify({ cmd: 'launch', owner: 'k', script: 'ticker.js', path: T, mode: 'subprocess' })));
		for (let i = 0; i < 100 && !tickPid; i++) await sleep(50);
		bridge.kill(signal);
		await sleep(1500); // childGuard grace is 500ms
		let alive = !!tickPid;
		if (tickPid) { try { process.kill(tickPid, 0); } catch (e) { alive = false; } }
		check(!!tickPid && !alive, `bridge ${signal}: subprocess script exited on its own`);
		if (alive) { try { process.kill(tickPid); } catch (e) { /* */ } }
	}
}

run()
	.then(() => { console.error(`\n${failures === 0 ? 'ALL PASSED' : failures + ' FAILURE(S)'}`); try { child.kill(); } catch (e) {} process.exit(failures === 0 ? 0 : 1); })
	.catch((e) => { console.error('HARNESS ERROR: ' + e.message); try { child.kill(); } catch (x) {} process.exit(2); });
