// Test fixture: reports its launch args via both ipc.args and process.argv.
const ipc = require('ipc-event-emitter').default(process);

ipc.emit('args', { ipcArgs: ipc.args, argv: process.argv.slice(2) });
