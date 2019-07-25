const npm = require('npm-programmatic');

const exec = require('child_process').exec;

let cmdString = "npm ls --depth=0";

//console.log(npm.commands);

//let previous = process.cwd();
let project = '../../../Content/Scripts/';
//process.chdir(project);

npm.list(project).then((packages)=>{
	console.log(packages);
}).catch((e)=>{
	console.log(e);
});