// Test fixture: answers 'ask' through the Emit Event With Callback reply function.
const ipc = require('ipc-event-emitter').default(process);

ipc.on('ask', (q, reply) => {
	reply({ doubled: q.n * 2 }, Buffer.from([7, 8, 9]));
});

console.log('acker ready');
