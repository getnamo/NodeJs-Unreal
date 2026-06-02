// Binary interweaving round-trip example: echoes back any { meta, buffer } it receives.
// Used by the test harness to validate that binary survives the bridge intact.

const ipc = require('ipc-event-emitter').default(process);

ipc.on('echo', (meta, buf) => {
	// buf arrives as a Node Buffer; emit it straight back alongside the meta.
	ipc.emit('echoed', meta, buf);
});

console.log('binEcho ready');
