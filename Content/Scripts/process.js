const { fork } = require('child_process');
const path = require('path');
// const IPC = require('ipc-event-emitter'); // Uncomment if IPC is needed

const activeChildren = {}; // Store active child processes by name
let scriptRoot = '../../../../../';

// Helper function to log messages with a category
function log(message, category = 'Parent') {
	console.log(`[${category}] ${message}`);
}

// Function to launch a child process for a given script
function launchChild(scriptName, scriptPath) {
	const fullPath = path.resolve(scriptRoot + scriptPath + scriptName);

	if (activeChildren[scriptName]) {
		log(`Child process for "${scriptName}" is already running.`);
		return;
	}

	try {
		const child = fork(fullPath); // , { stdio: ['pipe', 'pipe', 'pipe', 'ipc'] });

		child.on('message', (data) => {
			log(`[${scriptName}] ${data}`, 'Child');
		});

		child.on('exit', (code) => {
			log(`Child "${scriptName}" exited with code ${code}.`);
			delete activeChildren[scriptName]; // Remove from active children
		});

		activeChildren[scriptName] = { child }; // Store the child process
		log(`Launched child process for "${scriptName}" at "${scriptPath}".`);
	} catch (error) {
		log(`Failed to launch child process for "${scriptName}": ${error.message}`);
		console.error(error);
	}
}

function inlineChild(scriptName, scriptPath) {
	const fullPath = path.resolve(scriptRoot + scriptPath + scriptName);
	try {
		// Resolve the full path of the module
		const resolvedPath = require.resolve(fullPath);

		if (resolvedPath) {
			// Remove the module from the require cache
			delete require.cache[resolvedPath];
		}

		// Re-require the module
		const module = require(fullPath);
		log(`Module "${scriptName}" reloaded successfully.`);
		return module;
	} catch (error) {
		log(`Failed to reload module "${scriptName}": ${error.message}`);
		console.error(error);
		return null;
	}
}

// Function to send input to a specific child
function sendMessageToChild(scriptName, message) {
	if (!activeChildren[scriptName]) {
		log(`No active child process for "${scriptName}".`);
		return;
	}
	activeChildren[scriptName].child.send(message);
	log(`Sent message to child "${scriptName}": ${message}`);
}

// CLI input handling
process.stdin.on('data', (data) => {
	const input = data.toString().trim();
	const [command, ...args] = input.split(' ');

	if (command === 'launchChild') {
		const [scriptName, scriptPath] = args;
		if (scriptName && scriptPath) {
			launchChild(scriptName, scriptPath);
		} else {
			log('Usage: launchChild <scriptName> <scriptPath>');
		}
	} else if (command === 'send') {
		const [scriptName, ...messageParts] = args;
		const message = messageParts.join(' ');
		if (scriptName && message) {
			sendMessageToChild(scriptName, message);
		} else {
			log('Usage: send <scriptName> <message>');
		}
	} else if (command === 'launchInline') {
		const [scriptName, scriptPath] = args;

		if (scriptName && scriptPath) {
			inlineChild(scriptName, scriptPath);
		} else {
			log('Usage: launchInline <scriptName> <scriptPath>');
		}
	} else if (command === 'scriptsPath') {
		scriptRoot = args.join(' ');
		log(`Updated scriptsRoot to: ${scriptRoot}`);
	} else if (command === 'exit') {
		log('Exiting parent script.');
		for (const [scriptName, { child }] of Object.entries(activeChildren)) {
			log(`Killing child process "${scriptName}".`);
			child.kill();
		}
		process.exit(0);
	} else {
		log('Unknown command. Available commands: launchChild, send, launchInline, scriptsPath, exit.');
	}
});

// Handle unexpected errors
process.on('uncaughtException', (err) => {
	log(`Unhandled exception: ${err.message}`);
	console.error(err);
	process.exit(1);
});
