/**
	Wraps subprocess IPC between spawned child processes and unreal 
	via socket.io protocol.

	Support console.log forwarding as well as ipc-event-emitter two way binding.

	Status: early PoC dev builds, unstable api.
*/

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
const stopChildScript = "stopChildScript";
const logEvent = "console.log";	//uniqueish

//folders
const pluginRootFolder = "../../../";
const pluginContentScriptsFolder = pluginRootFolder + "Content/Scripts/";
const projectRootFolder = pluginRootFolder + "../../";
const projectContentScriptsFolder = projectRootFolder + "Content/Scripts/";
const defaultScriptPath = projectContentScriptsFolder;

//Process functions
const gracefulExit = (socket)=>{
	console.log('Done, exiting');
	if(child){
		child.disconnect();
	}
	io.emit('stdout', 'Exit');
	io.emit(mainScriptEnd);

	setTimeout(()=>{
		io.server.close();
		process.exit(1);
	}, 100);
}

//convenience log wrapper
const emitLog = (socket, msg)=>{
	socket.emit(logEvent, msg);
}

const startScript = (scriptName, socket, scriptPath)=>{	
	//default path is home path
	if(!scriptPath){
		scriptPath = './';
	}
	let fullScriptPath = scriptPath + scriptName;

	child = childProcess.fork(scriptPath + scriptName, [], { silent: true });
	ipc = new IPCEventEmitter(child);

	//console.log(fullScriptPath + ' started.');

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

	let lastError = "";

	//wrap around stderr to catch compile errors
	child.stderr.setEncoding('utf8');
	child.stderr.on('data', (err) => {
		//for some reason we get newlines separately, just spit out the long error event
		if(err.length>1){
			//set last error, we will emit on process.exit
			lastError = lastError + err.trim();
		}
	});

	//handle console.log from child processes
	child.stdout.setEncoding('utf8');
	child.stdout.on('data', (msg)=>{
		let finalMsg = msg.toString().trim();
		emitLog(socket, finalMsg);
	});

	child.on('exit', (code, signal) =>{
		//console.log(fullScriptPath + ' finished with ' + code);
		//abnormal process exit, emit error back to unreal via sio
		if(code == 1){
			//console.log('child error: ' + lastError);
			if(socket){
				socket.emit('childScriptError', lastError);
				lastError = "";
			}

		}
		//clear up our ipc
		ipc.isRunning = false;
		child = null;

		if(socket){
			socket.emit(childScriptEnd, fullScriptPath);
		}
	});

	ipc.isRunning = true;

	let result = {};
	result.ipc = ipc;
	result.child = child;

	return result;
}


//Connection logic
io.on('connection', (socket)=>{
	//re-direct console.log to our socket.io pipe
	console.log = (msg) =>{
		emitLog(socket, msg);
	}

	//we scope our connection info so we can route it correctly
	let ipc = null;
	let child = null;

	let childProcesses = {};

	//Middleware catch all events so we can forward them to our IPCs
	socket.use((packet, next) => {
		//socket.emit(logEvent, packet);
		//socket.emit(logEvent, 'Is ipc valid? ' + (ipc != null));

		//do we have a valid child running?
		for(let idx in childProcesses){
			const ipc = childProcesses[idx].ipc;
			//todo: filter ipc forward by process id in received event
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
		}

		next();
	});

	emitLog(socket, 'Connected as ' + socket.id);

	socket.on(runChildScript, (scriptName, startCallback)=>{
		//Start the specified script
		try{
			let processInfo = startScript(scriptName, socket, defaultScriptPath);
		
			//store our child processes in hashlist
			childProcesses[processInfo.child.pid] = processInfo;

			if(startCallback){
				//callback with id for muxing emits from unreal side
				startCallback(Number(processInfo.child.pid));
			}

			emitLog(socket, 'started script: ' + scriptName);
		}
		catch(e){
			emitLog(socket, 'script start Error: ' + util.inspect(e));
		}
	});

	socket.on(stopMainScript, (stopType)=>{
		emitLog(socket, 'Stopping main script due to ' + stopType);
		gracefulExit();
	});

	socket.on(stopChildScript, (processId)=>{
		try{
			const processInfo = childProcesses[processId];
			if(processInfo && processInfo.ipc && processInfo.child){
				processInfo.ipc.emit('kill');
				setTimeout(()=>{
					processInfo.child.disconnect();
					emitLog(socket, 'stopped script: ' + processId);
				},100);
			}
		}
		catch(e){
			emitLog(socket, 'stop script error: ' + util.inspect(e));
		}
	});
});

http.listen(port, ()=>{
	console.log('listening on *:' + port);
});

//startScript('./child.js');