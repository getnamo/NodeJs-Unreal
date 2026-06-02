// Classic NodeJs-Unreal demo: drives a cube/actor on a sine wave from a
// background thread without touching the game thread loop.
//
// Unreal side: on OnScriptBegin, EmitEvent("init", "{\"amplitude\":100,\"speed\":0.05}").
// Bind OnEvent for "transform" and apply { z } to your actor's relative location.
// EmitEvent("stop", "{}") to halt.

const ipc = require('ipc-event-emitter').default(process);

let timer = null;
let t = 0;

ipc.on('init', (opts) => {
	const amplitude = (opts && opts.amplitude) || 100;
	const speed = (opts && opts.speed) || 0.05;

	if (timer) clearInterval(timer);
	t = 0;
	timer = setInterval(() => {
		t += speed;
		ipc.emit('transform', { z: Math.sin(t) * amplitude });
	}, 16); // ~60Hz
});

ipc.on('stop', () => {
	if (timer) { clearInterval(timer); timer = null; }
});

console.log('cubeSine ready');
