// Engine-free verification for the NodeJs-Unreal v2.0.0 frame bridge.
//
// Spawns `node process.js`, talks the framed protocol to it exactly like the
// Unreal C++ side would, and validates:
//   1. inline adder round-trip       (myevent -> result)
//   2. subprocess adder round-trip   (fork + real IPC channel)
//   3. binary interweaving round-trip (binEcho echoes a Buffer unchanged)
//
// Run:  <bundled node.exe>  test\harness.js     (cwd = Content/Scripts)
// Exit code 0 = all passed.

const { spawn } = require('child_process');
const path = require('path');

const SCRIPTS_DIR = path.resolve(__dirname, '..');
const PROCESS_JS = path.join(SCRIPTS_DIR, 'process.js');

// ---- frame protocol (mirror of process.js) ----
const MAGIC = Buffer.from([0x4E, 0x55, 0x45, 0x01]);
const T_LOG = 0x01, T_ACTION = 0x02, T_EVENT = 0x03, T_ERROR = 0x04,
	T_CONTROL = 0x05, T_PLOG = 0x06, T_NPM = 0x07;

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

let rxBuf = Buffer.alloc(0);
function matchMagic(buf, i) { return i + 4 <= buf.length && buf[i] === MAGIC[0] && buf[i + 1] === MAGIC[1] && buf[i + 2] === MAGIC[2] && buf[i + 3] === MAGIC[3]; }
function findMagic(buf, start) { for (let i = start; i + 4 <= buf.length; i++) if (matchMagic(buf, i)) return i; return -1; }

child.stdout.on('data', (chunk) => {
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
		dispatch(type, header, binary);
		cursor = p;
	}
	if (cursor > 0) rxBuf = Buffer.from(rxBuf.subarray(cursor));
});

function dispatch(type, header, binary) {
	const tag = { [T_LOG]: 'LOG', [T_PLOG]: 'PLOG', [T_ACTION]: 'ACTION', [T_EVENT]: 'EVENT', [T_ERROR]: 'ERROR', [T_NPM]: 'NPM' }[type] || ('0x' + type.toString(16));
	let parsed = null;
	if (type === T_EVENT) { try { parsed = JSON.parse(header); parsed._buffers = parseBinaryTable(binary); } catch (e) { /* */ } }
	console.error(`  <- ${tag} ${header.length > 120 ? header.slice(0, 120) + '...' : header}${binary.length ? ` [+${binary.length}b]` : ''}`);
	const msg = { type, tag, header, binary, parsed };
	for (let i = listeners.length - 1; i >= 0; i--) {
		if (listeners[i].predicate(msg)) { const l = listeners[i]; listeners.splice(i, 1); l.resolve(msg); }
	}
}

function send(buf) { child.stdin.write(buf); }
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

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
	send(eventFrame('adder.js', 'myevent', [{ x: 3, y: 4 }]));
	{
		const m = await waitFor(m => m.type === T_EVENT && m.parsed && m.parsed.name === 'result', 5000, 'inline result');
		check(Math.abs(m.parsed.args[0] - 5) < 1e-9, 'inline adder: euclidean(3,4) == 5');
	}

	// ---- 2) subprocess adder ----
	send(controlFrame('launchSubprocess adder.js examples' + path.sep));
	await waitFor(m => m.type === T_LOG && m.header.includes('started'), 5000, 'subprocess adder started');
	send(eventFrame('adder.js', 'myevent', [{ x: 5, y: 12 }]));
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

	send(controlFrame('exit'));
	await sleep(200);
}

run()
	.then(() => { console.error(`\n${failures === 0 ? 'ALL PASSED' : failures + ' FAILURE(S)'}`); try { child.kill(); } catch (e) {} process.exit(failures === 0 ? 0 : 1); })
	.catch((e) => { console.error('HARNESS ERROR: ' + e.message); try { child.kill(); } catch (x) {} process.exit(2); });
