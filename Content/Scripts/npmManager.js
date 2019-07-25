/**
*	Nodejs-ue4 Plugin - npmManager.js
*	Installs package.json at specified path from using the ipc communication pipeline as a subprocess.
*/

const npm = require('./../../Source/ThirdParty/node/node_modules/npm');
const util = require('util');
const fs = require('fs');
const ipc = require('ipc-event-emitter').default(process);

let project = '../../../../../Content/Scripts/';	//game path
//let project = '../../../Content/Scripts/';	//plugin path

const installIfNeeded = (project, callback)=>{
	if(!callback){
		callback = ()=>{};
	}

	if(!project){
		callback('No path specified');
		return;
	}

	//change directory of process context for npm
	let previous = process.cwd();
	try{
		process.chdir(project);
	}
	catch(e){
		callback(project + ' path is not valid.');
		return;
	}

	let conf = {'bin-links': false, verbose: false, prefix: project}

	npm.load({conf}, function (err) {
	 	if (err) {
	 		callback(err);
	 		return console.log(err);
	 	}

	 	//list dependencies, are they all installed/met? if not re-install
	  	npm.commands.ls([], (er, data)=>{
			if(er) {
				console.log('dependencies not found, installing');
	 			npm.commands.install([], (er, data)=>{
		 			console.log('installed:');
		 			console.log(data);
		 			callback(null, { didInstall:data, isInstalled: true });
	 			});
				return;
			};

			let deps = data._dependencies;
			console.log('Dependencies met.');
			callback(null, { isInstalled:true });
		});

		//console.log(util.inspect(npm.commands.ls));

		npm.on('log', function(message) {
	    	// log installation progress
	   		console.log('log: ' + message);
	  	});
	});
}

const installPackage = (project, packageName, callback)=>{
	process.chdir(project);
	npm.load({conf}, function (er) {
		if (er) {
			return console.log(er);
		}
		npm.commands.install([packageName], callback);
	});
}

const uninstallPackage = (project, packageName, callback)=>{
	process.chdir(project);
	npm.load({conf}, function (er) {
	 	if (er) {
	 		return console.log(er);
	 	}
		npm.commands.uninstall([packageName], callback);
	});
}

//read package.json
const packages = (project, callback) => {
	fs.readFile(project + 'package.json', 'utf8', function(err, contents) {
    	callback(contents);
	});
}

//for debug purposes only
exports.installIfNeeded = installIfNeeded;

//we expect this script to be run as a subprocess
ipc.on('installIfNeeded', (path)=>{
	installIfNeeded(path, (err, result)=>{
		if(err){
			//make it empty as we check for isInstalled
			result = {};
			result.err = err;
		}
		ipc.emit('installIfNeededCallback', result);
	})
});

ipc.on('quit', ()=>{
	process.exit(0);
});

//exports.installIfNeeded(project);