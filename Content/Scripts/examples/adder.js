// NodeJs-Unreal "basic adder" example.
// Bind OnScriptBegin on the component, then EmitEvent("myevent", "{\"x\":3,\"y\":4}").
// The 'result' event comes back on the component's OnEvent.

const ipc = require('ipc-event-emitter').default(process);

const euclidean = (a, b) => ((a ** 2) + (b ** 2)) ** 0.5;

ipc.on('myevent', (vars) => {
	const c = euclidean(vars.x, vars.y);
	console.log('Got a request (a^2+b^2)^0.5: ' + c);

	// emit the result back to Unreal
	ipc.emit('result', c);
});

console.log('started');
