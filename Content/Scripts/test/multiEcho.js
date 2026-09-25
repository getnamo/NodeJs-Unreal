// Test fixture: echoes every arg (incl. multiple Buffers) back as 'echoed'.
const ipc = require('ipc-event-emitter').default(process);

ipc.on('echo', (...args) => ipc.emit('echoed', ...args));

console.log('multiEcho ready');
