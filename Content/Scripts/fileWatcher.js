//For file watching
const fs = require('fs');
const crypto = require('crypto');
const cryptoAlgorithm = 'sha1';
const k = require('./constants.js');

let watchers = {};

//obtain the hash of a file to see if changes are unique
const fileHash = (filePath, hashCallback) => {

	fs.createReadStream(filePath)
		.on('error', (err) => hashCallback(err + filePath))
		.pipe(crypto.createHash(cryptoAlgorithm)
			.setEncoding('hex'))
		.once('finish', function () {
			hashCallback(null, this.read());
	});
}

const stopWatchingScript = (scriptName)=>{
	const watcher = watchers[scriptName];

	if(watcher){
		watcher.close();
		delete watchers[scriptName];
	}
}

/** Watch for file changes in the script file*/
const watchScriptForChanges = (scriptName, changeCallback)=>{
	//already watching script?
	const currentWatcher = watchers[scriptName];
	if(currentWatcher){
		stopWatchingScript(scriptName);
	}

	const finalPath = k.projectContentScriptsFolder + scriptName;
	let watchLockOut = false;

	//store current hash
	let lastHash = null;	
	fileHash(finalPath, (err, hash)=>{		
		lastHash = hash;
	});

	const watcher = fs.watch(finalPath, (event, filename)=>{
		if (filename &&
			!watchLockOut)
		{
			watchLockOut = true;
			setTimeout(()=>watchLockOut = false, 100);

			fileHash(finalPath, (err, hash)=>{
				if(hash != lastHash){
					console.log(scriptName + " changed.");
					changeCallback(scriptName);
				}
				lastHash = hash;
			});
		}
	});

	//keep track in our watcher list
	watchers[scriptName] = watcher;

	return watcher;
}

const stopAllWatchers = ()=>{
	for(let scriptName in watchers){
		stopWatchingScript(scriptName);
	}
}

exports.stopWatchingScript = stopWatchingScript;
exports.watchScriptForChanges = watchScriptForChanges;
exports.stopAll = stopAllWatchers;