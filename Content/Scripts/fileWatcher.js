//For file watching
const fs = require('fs');
const crypto = require('crypto');
const cryptoAlgorithm = 'sha1';
const k = require('./constants.js');

//obtain the hash of a file to see if unique
const fileHash = (filePath, hashCallback) => {

	fs.createReadStream(filePath)
		.on('error', (err) => hashCallback(err + filePath))
		.pipe(crypto.createHash(cryptoAlgorithm)
			.setEncoding('hex'))
		.once('finish', function () {
			hashCallback(null, this.read());
	});
}

/** Watch for file changes in the script file*/
const watchScriptForChanges = (scriptName, changeCallback)=>{
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
					changeCallback(scriptName);
				}
				lastHash = hash;
			});
		}
	});
	return watcher;
}


exports.watchScriptForChanges = watchScriptForChanges;