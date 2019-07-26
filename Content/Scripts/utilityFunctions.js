const constants = require('./constants.js');

exports.routedEventName = (pid, eventName) =>{
	return pid + "@" + eventName;
}

exports.scriptLog = (socket, msg, pid)=>{
	socket.emit(constants.scriptLogEvent, [msg, pid]);
}