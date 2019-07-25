const npm = require('./../../Source/ThirdParty/node/node_modules/npm');
const util = require('util');
const fs = require('fs');

//console.log(npm.commands);
//let project = '../../../../../Content/Scripts/';	//game path
let project = '../../../Content/Scripts/';	//plugin path

const installIfNeeded = (project)=>{
	//change directory of process context for npm
	let previous = process.cwd();
	process.chdir(project);

	let conf = {'bin-links': false, verbose: false, prefix: project}

	npm.load({conf}, function (er) {
	 	if (er) {
	 		return console.log(er);
	 	}

	 	//list dependencies, are they all installed/met? if not re-install
	  	npm.commands.ls([], (er, data)=>{
			if(er) {
				console.log('dependencies not found, installing');
	 			npm.commands.install([], (er, data)=>{
		 			console.log('installed:');
		 			console.log(data);
	 		});
				return;
			};

			let deps = data._dependencies;
			console.log('installed.');
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

installIfNeeded(project);

//we need to run a subprocess it seems...
//process.chdir(previous);