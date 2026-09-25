// Test fixture: emits 'tick' on an interval until stopped. 'bye' on shouldExit.
// ipc.args[0] = period in ms (default 20).
const ipc = require('ipc-event-emitter').default(process);

const id = Math.random().toString(36).slice(2);
const period = Number(ipc.args[0]) || 20;

setInterval(() => ipc.emit('tick', { id, pid: process.pid }), period);
ipc.on('shouldExit', () => ipc.emit('bye', { id }));

console.log('ticker ready ' + id);
