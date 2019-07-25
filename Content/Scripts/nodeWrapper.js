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
const scriptLogEvent = "script.log";
const npmInstallEvent = "npmInstall";

//folders
const pluginRootFolder = "../../../";
const pluginContentScriptsFolder = pluginRootFolder + "Content/Scripts/";
const projectRootFolder = pluginRootFolder + "../../";
const projectContentScriptsFolder = projectRootFolder + "Content/Scripts/";
const defaultScriptPath = projectContentScriptsFolder;

//Process functions
const gracefulExit = (socket)=>{
	console.log('Done, exiting');
	socket.emit(mainScriptEnd);

	setTimeout(()=>{
		io.server.close();
		process.exit(1);
	}, 100);
}

//convenience log wrappers
//re-direct console.log to our socket.io pipe
const emitLog = (msg) =>{
	console.log(msg);
	//io.emit(logEvent, msg);
}

const scriptLog = (socket, msg, pid)=>{
	socket.emit(scriptLogEvent, [msg, pid]);
}

const startNpmScript = (targetPath, callback) =>{
	/*if(!callback){
		callback = ()=>{};
	}*/

	//startup the npmManager script
	let scriptName = 'npmManager.js';
	let child = childProcess.fork(pluginContentScriptsFolder + scriptName, [], { silent: true });
	ipc = new IPCEventEmitter(child);
	let pid = child.pid;

	child.stderr.setEncoding('utf8');
	child.stderr.on('data', (err) => {
		console.log(err);
	});

	console.log('started npm with ' + pid);

	//bind ipc data and emit call
	ipc.on('installIfNeededCallback', (result)=>{
		callback(result);
		ipc.emit('quit');
	});

	ipc.emit('installIfNeeded', targetPath);
}

const startScript = (scriptName, socket, scriptPath)=>{	
	//default path is home path
	if(!scriptPath){
		scriptPath = './';
	}
	let fullScriptPath = scriptPath + scriptName;

	let child = childProcess.fork(scriptPath + scriptName, [], { silent: true });
	ipc = new IPCEventEmitter(child);
	let pid = child.pid;

	//emitLog(fullScriptPath + ' started.');

	//catch messages directly and auto-forward the messages to our socket.io pipe
	child.on('message', data =>{
		//emitLog(data);
		if(	data.type == 'ipc-event-emitter' &&
			data.emit &&
			data.emit.length > 0){
			let eventName = data.emit.shift();
			let args = data.emit;
			
			let combinedEventName = child.pid + "@" + eventName;

			//scriptLog(socket, 'event: ' + combinedEventName + ", args: " + args);

			if(socket){
				if(args.length == 1){
					socket.emit(combinedEventName, args[0]);
				}
				else{
					socket.emit(combinedEventName, args);
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
		//emitLog('got: ' + msg);
		scriptLog(socket, finalMsg, pid);
	});

	child.on('exit', (code, signal) =>{
		//emitLog(fullScriptPath + ' finished with ' + code);
		//abnormal process exit, emit error back to unreal via sio
		if(code == 1){
			//emitLog('child error: ' + lastError);
			if(socket){
				socket.emit(childScriptError, lastError);
				lastError = "";
			}
		}
		
		if(socket){
			socket.emit(childScriptEnd, pid);
		}

		//clear up our ipc
		ipc.isRunning = false;
		child = null;
	});

	ipc.isRunning = true;

	let result = {};
	result.ipc = ipc;
	result.child = child;

	return result;
}

//Connection logic
io.on('connection', (socket)=>{
	let childProcesses = {};
	let bundle = {}
	bundle.socket = socket;

	//Middleware catch all events so we can forward them to our IPC bridge
	socket.use((packet, next) => {
		//socket.emit(logEvent, packet);
		//socket.emit(logEvent, 'Is ipc valid? ' + (ipc != null));
		//do we have a valid child running?
		let eventName = packet[0];
		let args = packet[1];
		let idIndex = eventName.indexOf('@');

		//socket.emit(logEvent, 'pid? ' + idIndex);
		
		//No split? emit to all
		if(idIndex == -1)
		{
			for(let pid in childProcesses){
				const ipc = childProcesses[pid].ipc;
				if(ipc && ipc.isRunning){
					if(packet.length>1){
						ipc.emit(eventName, args);
					}
					else{
						ipc.emit(eventName);
					}
				}
			}
		}
		//found the pid, emit only to the ipc for pid
		else
		{
			let pid = eventName.substring(0, idIndex);
			let eventOnly = eventName.substring(idIndex+1);
			let foundProcess = childProcesses[pid];

			if(foundProcess){
				const ipc = foundProcess.ipc;
				if(ipc && ipc.isRunning){
					if(packet.length>1){
						ipc.emit(eventOnly, args);
					}
					else{
						ipc.emit(eventOnly);
					}
				}
			}
		}
		next();
	});

	emitLog(socket.id + ' connected.');

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

			emitLog('started script: ' + scriptName);
		}
		catch(e){
			emitLog('script start Error: ' + util.inspect(e));
		}
	});

	socket.on(stopMainScript, (stopType)=>{
		emitLog('Stopping main script due to ' + stopType);
		gracefulExit();
	});

	socket.on(stopChildScript, (processId)=>{
		try{
			const processInfo = childProcesses[processId];
			if(	processInfo &&
			 	processInfo.ipc &&
			 	processInfo.child &&
			 	processInfo.child.connected){

				//emitLog(util.inspect(processInfo));

				processInfo.ipc.emit('kill');
				setTimeout(()=>{
					try{
						processInfo.child.disconnect();
						emitLog('stopped script: ' + processId);
						delete childProcesses[processId];
						socket.emit(childScriptEnd, processId);
					}
					catch(e){
						emitLog('script disconnect error: ' + util.inspect(e));
					}
				},100);
			}
			else{
				emitLog(processId + ' process no longer valid for termination.');
			}
		}
		catch(e){
			emitLog('stop script error: ' + util.inspect(e));
		}
		
	});

	//linkup npm install procedure
	socket.on(npmInstallEvent, (pathToInstallPackages, callback)=>{
		startNpmScript(pathToInstallPackages, (result)=>{
			console.log('NPM install result: ' + util.inspect(result));
			callback(result);
		});
	});
});

http.listen(port, ()=>{
	console.log('listening on *:' + port);
});

//startScript('./child.js');

//projectContentScriptsFolder
startNpmScript(projectContentScriptsFolder, (result)=>{
	
});