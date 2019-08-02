/**
*	Nodejs-ue4 Plugin - nodeWrapper.js
*	Wraps subprocess IPC between spawned child processes and unreal 
*	via socket.io protocol.
*
*	Supports console.log forwarding as well as ipc-event-emitter two way binding.
*	Supports npm package.json installation
*
*	Status: early PoC dev builds, unstable api.
*/

//External Dependencies
const app = require('express')();
const http = require('http').Server(app);
const io = require('socket.io')(http);
const util = require('util');

//Local Dependencies
const k = require('./constants.js');
const uf = require('./utilityFunctions.js');
const scriptHandler = require('./scriptHandler.js');
const watcher = require('./fileWatcher.js');

//Process functions
const gracefulExit = (socket)=>{
	console.log('Done, exiting');
	socket.emit(k.mainScriptEnd);

	setTimeout(()=>{
		io.server.close();
		process.exit(1);
	}, 100);
}

//convenience log wrappers
//re-direct console.log to our socket.io pipe
const emitLog = (msg) =>{
	console.log(msg);
	if(io){
		//io.emit(k.logEvent, msg);
	}
}

//Connection logic
io.on('connection', (socket)=>{
	let childProcesses = {};
	let bundle = {}
	bundle.socket = socket;

	//Middleware catch all events so we can forward them to our IPC bridge
	socket.use((packet, next) => {
		//socket.emit(k.logEvent, packet);
		//socket.emit(k.logEvent, 'Is ipc valid? ' + (ipc != null));
		//do we have a valid child running?
		let eventName = packet[0];
		let args = packet[1];
		let idIndex = eventName.indexOf('@');

		//emitLog('pid? ' + idIndex);
		//emitLog(childProcesses);
		
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

	socket.on(k.runChildScript, (scriptName, startCallback)=>{
		//Start the specified script
		try{
			let processInfo = scriptHandler.startScript(scriptName, socket, k.defaultScriptPath);
			processInfo.scriptName = scriptName;
			const processId = processInfo.child.pid;
		
			//store our child processes in hashlist
			childProcesses[processId] = processInfo;

			if(startCallback){
				//callback with id for muxing emits from unreal side
				startCallback(Number(processInfo.child.pid));
			}

			emitLog('started script: ' + scriptName + ' at pid: ' + processId);
		}
		catch(e){
			emitLog('script start Error: ' + util.inspect(e));
		}
	});

	socket.on(k.stopChildScript, (processId)=>{
		try{
			const processInfo = childProcesses[processId];

			if(processInfo){
				
				scriptHandler.stopScript(processInfo, (err, processId)=>{
					if(err){
						emitLog(err);
					}
					else{
						delete childProcesses[processId];
						emitLog('stopped script: ' + processId);
						socket.emit(k.childScriptEnd, processId);
					}
				});
			}
		}
		catch(e){
			emitLog('stop script error: ' + util.inspect(e));
		}
		
	});

	socket.on(k.watchChildScript, (scriptName, onChangeCallback)=>{
		watcher.watchScriptForChanges(scriptName, (fileName)=>{
			//callback variant - callbacks can only be called once :(
			if(onChangeCallback){
				onChangeCallback(fileName);
			}

			//emit variant watchCallback@scriptName
			socket.emit(k.watchCallback + scriptName, fileName);

		});
		emitLog('Started watching ' + scriptName);
	});

	socket.on(k.unwatchChildScript, (scriptName) => {
		watcher.stopWatchingScript(scriptName);
		emitLog('Stopped watching ' + scriptName);
	});

	socket.on(k.stopMainScript, (stopType)=>{
		emitLog('Stopping main script due to ' + stopType);

		//stop any remaining watchers
		watcher.stopAll();

		//exit
		gracefulExit();
	});

	//linkup npm install procedure
	socket.on(k.npmInstallEvent, (projectRootRelativePath, callback)=>{
		//We expect a project root relative path
		const finalPath = k.projectRootFolder + projectRootRelativePath;

		emitLog(finalPath);
		scriptHandler.startNpmScript(finalPath, (result)=>{
			emitLog('NPM install result: ' + util.inspect(result));
			callback(result);
		});
	});
});

http.listen(k.port, ()=>{
	emitLog('listening on *:' + k.port);
});

//Debug tests
//scriptHandler.startScript('./child.js');
/*const finalPath = k.projectRootFolder + 'Content/Scripts';
startNpmScript(finalPath, (result)=>{
	console.log(result);
});*/

/*watcher.watchScriptForChanges("myscript.js", (fileName)=>{
	console.log(`${fileName} changed.`)
});*/