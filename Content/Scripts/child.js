//One liner include
let ipc = require('ipc-event-emitter').default(process);

console.log('child.js script begin');

ipc.on('echo', (msg) =>{
	console.log(msg);
	//modify script
	msg.x = 4;
	ipc.emit('echo', msg);
});

ipc.on('quit', (msg) =>{
	console.log('quit signal received');
	setTimeout(()=>{
		process.exit();
	}, 100);
});

//asdasdbagj
//throw 'bad stuff'
//console.error('bad stuff man')

ipc.pin('ready');

console.log('child.js script endoffile');