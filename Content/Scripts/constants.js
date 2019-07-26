//Fixed events
exports.mainScriptEnd = "mainScriptEnd";
exports.stopMainScript = "stopMainScript";
exports.childScriptEnd = "childScriptEnd";
exports.childScriptError = "childScriptError";
exports.runChildScript = "runChildScript";
exports.stopChildScript = "stopChildScript";
exports.logEvent = "console.log";	//uniqueish
exports.scriptLogEvent = "script.log";
exports.npmInstallEvent = "npmInstall";
exports.watchChildScript = "watchScriptFile";
exports.unwatchChildScript = "unwatchScriptFile";

//folders
exports.pluginRootFolder = "../../../";
exports.pluginContentScriptsFolder = exports.pluginRootFolder + "Content/Scripts/";
exports.projectRootFolder = exports.pluginRootFolder + "../../";
exports.projectContentScriptsFolder = exports.projectRootFolder + "Content/Scripts/";
exports.defaultScriptPath = exports.projectContentScriptsFolder;