const { fork } = require('child_process');
const path = require('path');
const fs = require('fs');

const activeChildren = {}; // Store active child processes by name
const watchedScripts = {}; // Track watched scripts and their reload methods
const launchedScripts = {}; // Track launched scripts with their methods
let scriptRoot = '../../../../../';

// Helper function to log messages with a category
function log(message, category = 'Process') {
	console.log(`~[${category}]~ ${message}`);
}

// Function to launch a child process for a given script
function launchSubprocess(scriptName, scriptPath) {
	const fullPath = path.resolve(scriptRoot + scriptPath + scriptName);

	if (activeChildren[scriptName]) {
		log(`Child process for "${scriptName}" is already running.`);
		return;
	}

	try {
		const child = fork(fullPath);

		child.on('message', (data) => {
			log(`[${scriptName}] ${data}`, 'Child');
		});

		child.on('exit', (code) => {
			log(`Child "${scriptName}" exited with code ${code}.`);

			// Signal stop
			log('end ' + fullPath, 'Action');
			delete activeChildren[scriptName]; // Remove from active children
		});

		activeChildren[scriptName] = { child }; // Store the child process
		launchedScripts[fullPath] = { scriptName, method: 'child' }; // Track the launch method
		log(`Launched child process for "${scriptName}" at "${scriptPath}".`);
	} catch (error) {
		log(`Failed to launch child process for "${scriptName}": ${error.message}`);
		console.error(error);
	}
}

//inline module
function inlineChild(scriptName, scriptPath) {
	const fullPath = path.resolve(scriptRoot + scriptPath + scriptName);
	try {
		// Resolve the full path of the module
		const resolvedPath = require.resolve(fullPath);

		if (resolvedPath) {
			// Remove the module from the require cache
			delete require.cache[resolvedPath];

			// Signal stop
			log('end ' + fullPath, 'Action');
		}

		// Re-require the module
		const module = require(fullPath);
		launchedScripts[fullPath] = { scriptName, method: 'inline' }; // Track the launch method
		log(`Module "${scriptName}" reloaded successfully.`);
		return module;
	} catch (error) {
		log(`Failed to reload module "${scriptName}": ${error.message}`);
		console.error(error);
		return null;
	}
}

// Function to stop a script
function stopScript(scriptName) {
    const scriptEntry = Object.entries(launchedScripts).find(
        ([, details]) => details.scriptName === scriptName
    );

    if (!scriptEntry) {
        log(`"${scriptName}" is not currently running or has not been launched.`, 'Error');
        return;
    }

    const [fullPath, { method }] = scriptEntry;

    try {
        if (method === 'inline') {
            // Unload the module from the require cache
            const resolvedPath = require.resolve(fullPath);
            if (require.cache[resolvedPath]) {
                delete require.cache[resolvedPath];
                log(`Unloaded inline script "${scriptName}".`);
            } else {
                log(`Inline script "${scriptName}" is not in the require cache.`);
            }
        } else if (method === 'child') {
            // Kill the child process
            if (activeChildren[scriptName]) {
                activeChildren[scriptName].child.kill();
                delete activeChildren[scriptName];
                log(`Killed child process for "${scriptName}".`);
            } else {
                log(`Child process for "${scriptName}" is not active.`);
            }
        }

        // Stop watching the script
        if (watchedScripts[fullPath]) {
            watchedScripts[fullPath].close();
            delete watchedScripts[fullPath];
            log(`Stopped watching "${scriptName}".`);
        }

        // Remove the script entry from launchedScripts
        delete launchedScripts[fullPath];
    } catch (error) {
        log(`Failed to stop "${scriptName}": ${error.message}`, 'Error');
        console.error(error);
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

// Function to watch a script file for changes
function watchScript(scriptName, scriptPath) {
	const debounceDuration = 100; //in ms
    const fullPath = path.resolve(scriptRoot + scriptPath + scriptName);

    if (watchedScripts[fullPath]) {
        log(`"${scriptName}" is already being watched.`);
        return;
    }

    // Ensure the script was previously launched
    if (!launchedScripts[fullPath]) {
        log(`"${scriptName}" has not been launched yet. Please launch it first.`);
        return;
    }

    const { method } = launchedScripts[fullPath];
    let reloadTimeout; // Timeout for debouncing

    try {
        const reload = () => {
            log(`Detected change in "${scriptName}". Reloading...`);
            log('reload ' + fullPath, 'Action');
            if (method === 'inline') {
                inlineChild(scriptName, scriptPath);
            } else if (method === 'child') {
                if (activeChildren[scriptName]) {
                    log(`Restarting child process for "${scriptName}".`);
                    activeChildren[scriptName].child.kill();
                    delete activeChildren[scriptName];
                }
                launchSubprocess(scriptName, scriptPath);
            }
        };

        const watcher = fs.watch(fullPath, (eventType) => {
            if (eventType === 'change') {
                // Debounce the reload to prevent multiple triggers
                if (reloadTimeout) {
                    clearTimeout(reloadTimeout);
                }
                reloadTimeout = setTimeout(() => {
                    reload();
                }, debounceDuration); // 100ms debounce delay
            }
        });

        watchedScripts[fullPath] = watcher;
        log(`Watching "${scriptName}" for changes.`);
    } catch (error) {
        log(`Failed to watch "${scriptName}": ${error.message}`);
        console.error(error);
    }
}

// CLI input handling
process.stdin.on('data', (data) => {
    const input = data.toString().trim();
    const [command, ...args] = input.split(' ');

    if (command === 'launchSubprocess') {
        const [scriptName, scriptPath] = args;
        if (scriptName && scriptPath) {
			const fullPath = path.resolve(scriptRoot + scriptPath + scriptName);
			log('begin ' + fullPath, 'Action');
            launchSubprocess(scriptName, scriptPath);
        } else {
            log('Usage: launchSubprocess <scriptName> <scriptPath>');
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
			const fullPath = path.resolve(scriptRoot + scriptPath + scriptName);
			log('begin ' + fullPath, 'Action');
			
            inlineChild(scriptName, scriptPath);
        } else {
            log('Usage: launchInline <scriptName> <scriptPath>');
        }
    } else if (command === 'watch') {
        const [scriptName, scriptPath] = args;

        if (scriptName && scriptPath) {
            watchScript(scriptName, scriptPath);
        } else {
            log('Usage: watch <scriptName> <scriptPath>');
        }
    } else if (command === 'stop') {
        const [scriptName] = args;

        if (scriptName) {
            stopScript(scriptName);
        } else {
            log('Usage: stop <scriptName>');
        }
    } else if (command === 'scriptsPath') {
        scriptRoot = args.join(' ');
        log(`Updated scriptsRoot to: ${scriptRoot}`);
    } else if (command === 'exit') {
        log('Exiting parent script.');
        for (const [scriptName, { child }] of Object.entries(activeChildren)) {
            log(`Killing child process "${scriptName}".`);
			log('end ' + scriptName, 'Action');
            child.kill();
        }
        for (const [filePath, watcher] of Object.entries(watchedScripts)) {
            log(`Stopping watch on "${filePath}".`);
            watcher.close();
        }
        process.exit(0);
    } else {
        log('Unknown command. Available commands: launchSubprocess, send, launchInline, watch, stop, scriptsPath, exit.');
    }
});

// Handle unexpected errors
process.on('uncaughtException', (err) => {
	log(`Unhandled exception: ${err.message}`);
	console.error(err);
	process.exit(1);
});
