// Throughput / large-binary stress example.
// Unreal side: EmitEvent("start", "{\"size\":262144,\"count\":100}").
// Each 'chunk' event carries a binary Buffer (surfaced on OnEvent's Binary param).
// A final 'done' event reports elapsed time so you can measure MB/s.

const ipc = require('ipc-event-emitter').default(process);

ipc.on('start', (opts) => {
	const size = (opts && opts.size) || 256 * 1024; // 256 KB
	const count = (opts && opts.count) || 100;
	const buf = Buffer.alloc(size, 0xAB);

	const startT = Date.now();
	for (let i = 0; i < count; i++) {
		ipc.emit('chunk', { index: i }, buf);
	}
	const ms = Date.now() - startT;
	ipc.emit('done', { count, size, ms, mb: (count * size) / (1024 * 1024) });
});

console.log('perfStream ready');
