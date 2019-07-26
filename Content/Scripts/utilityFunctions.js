const k = require('./constants.js');

exports.routedEventName = (pid, eventName) =>{
	return pid + "@" + eventName;
}

exports.scriptLog = (socket, msg, pid)=>{
	socket.emit(k.scriptLogEvent, [msg, pid]);
}