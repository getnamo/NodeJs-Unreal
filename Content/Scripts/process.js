const { fork } = require('child_process');
const path = require('path');
const IPC = require('ipc-event-emitter');

const activeChildren = {}; // Store active child processes by name
let scriptRoot = './';

// Function to launch a child process for a given script
function launchChild(scriptName, scriptPath) {
	const fullPath = path.resolve(scriptRoot + scriptPath + scriptName);

	if (activeChildren[scriptName]) {
		console.log(`Child process for "${scriptName}" is already running.`);
		return;
	}

	//console.log('fullpath is', fullPath);

	try {
		const child = fork(fullPath);//, { stdio: ['pipe', 'pipe', 'pipe', 'ipc'] });

		//console.log('ipc is', IPC);

		//console.log('child is', child);

		/*const ipc = IPC(child);

		

		// Handle child process events
		ipc.on('log', (message) => {
			console.log(`[Child ${scriptName} log]: ${message}`);
		});

		ipc.on('error', (error) => {
			console.error(`[Child ${scriptName} error]: ${error}`);
		});*/

		child.on('message', data =>{
			console.log(data);
		});

		child.on('exit', (code) => {
			console.log(`Child "${scriptName}" exited with code ${code}`);
			delete activeChildren[scriptName]; // Remove from active children
		});

		activeChildren[scriptName] = { child }// ipc };
		console.log(`Launched child process for "${scriptName}" at "${scriptPath}".`);
	} catch (error) {
		console.error(`Failed to launch child process for "${scriptName}": ${error.message}`);
		console.error(error);
	}
}

// Function to send input to a specific child
function sendMessageToChild(scriptName, message) {
	if (!activeChildren[scriptName]) {
		console.error(`No active child process for "${scriptName}".`);
		return;
	}
	activeChildren[scriptName].ipc.emit('input', message);
}

// CLI input handling
process.stdin.on('data', (data) => {
	const input = data.toString().trim();
	const [command, ...args] = input.split(' ');

	if (command === 'launch') {
		const [scriptName, scriptPath] = args;
		if (scriptName && scriptPath) {
			launchChild(scriptName, scriptPath);
		} else {
			console.error('Usage: launch <scriptName> <scriptPath>');
		}
	} else if (command === 'send') {
		const [scriptName, ...messageParts] = args;
		const message = messageParts.join(' ');
		if (scriptName && message) {
			sendMessageToChild(scriptName, message);
		} else {
			console.error('Usage: send <scriptName> <message>');
		}
	} else if (command === 'scriptsPath'){
		scriptRoot = args.join(' ');

		console.log('updated scriptsRoot to: ' + scriptRoot);
	}
	else if (command === 'exit') {
		console.log('Exiting parent script.');
		for (const [scriptName, { child }] of Object.entries(activeChildren)) {
			console.log(`Killing child process "${scriptName}".`);
			child.kill();
		}
		process.exit(0);
	} else {
		console.error('Unknown command. Available commands: launch, send, exit.');
	}
});

// Handle unexpected errors
process.on('uncaughtException', (err) => {
	console.error('Unhandled exception:', err);
	process.exit(1);
});
