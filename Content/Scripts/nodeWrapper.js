console.log('Starting up Testbed');

const port = 4269;	//fairly unique
const app = require('express')();
const http = require('http').Server(app);
const io = require('socket.io')(http);
const childProcess = require('child_process');
const IPC = require('ipc-event-emitter').default;
const IPCEventEmitter = require('ipc-event-emitter').IPCEventEmitter;
const util = require('util');

//Fixed events
const mainScriptEnd = "mainScriptEnd";
const stopMainScript = "stopMainScript";
const childScriptEnd = "childScriptEnd";
const childScriptError = "childScriptError";
const runChildScript = "runChildScript";
const logEvent = "console.log";	//uniqueish

let ipc = null;

const gracefulExit = (socket)=>{
	console.log('Done, exiting');
	io.emit('stdout', 'Exit');
	io.emit(mainScriptEnd);

	setTimeout(()=>{
		io.server.close();
		process.exit(1);
	}, 100);
}

const startScript = (scriptName, socket, scriptPath)=>{	
	//default path is home path
	if(!scriptPath){
		scriptPath = './';
	}
	let fullScriptPath = scriptPath + scriptName;

	let child = childProcess.fork(scriptPath + scriptName, [], { silent: true });
	ipc = new IPCEventEmitter(child);

	console.log(fullScriptPath + ' started.');

	//catch messages directly and auto-forward the messages to our socket.io pipe
	child.on('message', data =>{
		//console.log(data);
		if(data.type == 'ipc-event-emitter' 
			&& data.emit
			&& data.emit.length > 0){
			let eventName = data.emit.shift();
			let args = data.emit;
			//console.log('event: ' + eventName + ", args: " + args);

			if(socket){
				if(args.length == 1){
					socket.emit(eventName, args[0]);
				}
				else{
					socket.emit(eventName, args);
				}
				
			}
		}
	});

	let lastError = null;

	//wrap around stderr to catch compile errors
	child.stderr.setEncoding('utf8');
	child.stderr.on('data', (err) => {
		//for some reason we get newlines separately, just spit out the long error event
		if(err.length>1){
			//set last error, we will emit on process.exit

			lastError = err.trim();
			//console.log(lastError);
		}
	});

	//handle console.log from child processes
	child.stdout.setEncoding('utf8');
	child.stdout.on('data', (msg)=>{
		let finalMsg = msg.toString().trim();
		console.log(finalMsg);

		//log any child process console.log to our unreal log
		if(socket){
			socket.emit('log', finalMsg);
		}
	});

	child.on('exit', (code, signal) =>{
		console.log(scriptPath + ' finished with ' + code);
		//abnormal process exit, emit error back to unreal via sio
		if(code == 1){
			//console.log('child error: ' + lastError);
			if(socket){
				socket.emit('childScriptError', lastError);
			}

		}
		//clear up our ipc
		ipc.isRunning = false;

		if(socket){
			socket.emit(childScriptEnd, fullScriptPath);
		}
	});

	ipc.isRunning = true;
	
	//todo: replace these with catch-all links
	/*ipc.on('ready', () => {
		console.log('got "ready", sending "ping"');
		ipc.emit('ping');
	});
	ipc.on('pong', () => {
		console.log('got "pong", disconnecting');
		child.disconnect();
	});*/

	//console.log(ipc);

	return ipc;
}


//connection logic
io.on('connection', (socket)=>{
	//re-direct console.log to our socket.io pipe
	console.log = (msg) =>{
		socket.emit(logEvent, msg);
	}

	//Middleware all event catcher so we can forward them to our IPCs
	socket.use((packet, next) => {
    	// Handler

    	//debug this back for now
    	socket.emit(logEvent, packet);
    	socket.emit(logEvent, 'Is ipc valid? ' + (ipc != null));

    	//do we have a child running?
    	if(ipc && ipc.isRunning){
    		let eventName = packet[0];
    		let args = packet[1];
    		if(packet.length>1){
    			ipc.emit(eventName, args);
    		}
    		else{
    			ipc.emit(eventName);
    		}
    	}

    	next();
	});

	socket.emit(logEvent, 'Connected as ' + socket.id);

	socket.on(runChildScript, (scriptName)=>{
		//Start the specified script
		ipc = startScript(scriptName, socket);

		socket.emit(logEvent, 'started script: ' + scriptName);
		
		//todo: catch all socket.io events map to ipc events
	});

	socket.on(stopMainScript, (stopType)=>{
		console.log('Stopping main script due to ' + stopType);
		gracefulExit();
	});
});

http.listen(port, ()=>{
	console.log('listening on *:' + port);
});

//startScript('./child.js');