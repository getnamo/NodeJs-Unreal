# nodejs-ue4
Embed node.js as an unreal plugin. This enables you to embed cool things like: https://www.npmjs.com/.

[![GitHub release](https://img.shields.io/github/release/getnamo/nodejs-ue4.svg)](https://github.com/getnamo/nodejs-ue4/releases)
[![Github All Releases](https://img.shields.io/github/downloads/getnamo/nodejs-ue4/total.svg)](https://github.com/getnamo/nodejs-ue4/releases)

Want to control unreal with javascript? consider using https://github.com/ncsoft/Unreal.js which is much more feature-rich. This plugin instead focuses on bringing node.js and npm api on background threads.

Currently in an early working state, may have bugs!

#### Questions and Feedback

Got questions or problems? post to https://github.com/getnamo/nodejs-ue4/issues

or discuss in the [Unreal forum thread](https://forums.unrealengine.com/community/work-in-progress/1644397-node-js)

## What can you do with it?

A lot of really useful programming solutions exist for node.js; all the npm modules can now be used with unreal. A small sample of possibilities:


#### Communications

Great native support for http and embedding simple servers. You could for example run a local embedded webserver and serve that webpage as a UI using e.g. https://github.com/getnamo/BLUI

- Websocket https://www.npmjs.com/package/ws
- WebRTC https://www.npmjs.com/package/simple-peer
- Socket.io server https://www.npmjs.com/package/socket.io
- email https://www.npmjs.com/package/nodemailer
- RSync https://www.npmjs.com/package/rsync

#### Math
- Lib https://www.npmjs.com/package/mathjs
- Expressions https://www.npmjs.com/package/math-expressions

#### Commandline

You can e.g. embed any other bat or commandline executable and parse args to control

- Shell https://www.npmjs.com/package/shelljs
- Arg parsing https://www.npmjs.com/package/argparse
- Zip https://www.npmjs.com/package/jszip
- Sandboxed js VM https://www.npmjs.com/package/vm2

#### Utilities
- Image manipulation https://www.npmjs.com/package/sharp
- PDF Generation https://www.npmjs.com/package/pdfkit

#### And much much more:
- https://www.npmjs.com/
- https://github.com/bsonntag/cool-node-modules
- https://colorlib.com/wp/npm-packages-node-js/.

## Quick Install & Setup ##

### Via Github Releases
 1. [Download Latest Release](https://github.com/getnamo/nodejs-ue4/releases)
 2. Create new or choose project.
 3. Browse to your project folder (typically found at Documents/Unreal Project/{Your Project Root})
 4. Copy *Plugins* folder into your Project root.
 5. Plugin should be now ready to use.
 
## How to use - Basics

### Blueprint side

Add a ```Node Component``` to actor of choice

![add component](https://i.imgur.com/Xvc6v98.png)

In your component properties set the name of the script you wish to run e.g. ```myscript.js```. This path is relative to ```{Your Project Root}/Content/Scripts/```.

![set script](https://i.imgur.com/xalQplZ.png)

Now let's look at a basic script

### Node Scripts
Place your script files inside ```{Project Root}/Content/Scripts```

The script files can be vanilla javascript, node.js, and/or include npm modules (since v0.2 ensure you add them to your folder's package.json to auto-resolve on run).

#### A basic example with just console.log output

```js
//1) simple basics work: Just log stuff!
const euclidean = (a, b) =>{
	return ((a ** 2) + (b ** 2)) ** 0.5;
}

a = 3;
b = 4;
c = euclidean(a,b);

console.log('(a^2+b^2)^0.5: ' + c);
```

To listen to your script log bind to the node component event ```On Console Log```

![on console log](https://i.imgur.com/IzIlhqQ.png)

but what if you want to send data/receive data to your script?

#### A basic adder

Let's expand the script to include the npm module ```ipc-event-emitter```. We will use this to communicate events back and forth to our blueprint component

```js
//2) Let's connect our euclidean function via IPC

//One liner include
const ipc = require('ipc-event-emitter').default(process);

const euclidean = (a, b) =>{
	return ((a ** 2) + (b ** 2)) ** 0.5;
}

//Listen to 'myevent' event
ipc.on('myevent', (vars) => {
	let c = euclidean(vars.x, vars.y);
	console.log('Got a request (a^2+b^2)^0.5: ' + c);

	//emit result back as a 'result' event
	ipc.emit('result', c);
});

console.log('started');
```

On the blueprint side, because our script runs on begin play, bind to the ```result``` event first and then emit a vector2d with x and y float values and convert it to a SIOJsonValue, this will autoconvert to a json object in your script side.

![bp comms](https://i.imgur.com/XVe64kA.png)

When the script emits the ```result``` event, it will return to our component ```OnEvent``` event (you can also bind directly to a function see https://github.com/getnamo/socketio-client-ue4#binding-events-to-functions for example syntax). In the example above we simply re-encode the message to json and print to string.

That's the basics! There are some other events and functions for e.g. starting/stopping and getting notifications of those states, but largely anything else will be in your node.js script side.

## Usage Notes

#### Errors
If you write an error in your script, it will spit it out in your output log. Hitting save and re-running the component will re-run the script.

![error](https://i.imgur.com/hh03jnD.png)

#### npm modules

Since v0.2 script errors caused by missing npm modules will auto-check the ```package.json``` in your script folder for missing modules. If the dependency isn't listed it will warn you about it, if it does exist it will auto-resolve the dependencies and re-run your script after installation; auto-fixing your error.

Basically keep your script's ```package.json``` up to date with the required dependencies and it will auto-install them as needed without performance implications (doesn't check every run, only on script error).

![properties](https://i.imgur.com/s5o983w.png)

You can disable this auto-resolving and auto-run on npm install via the node component properties. Then you can resolve Npm dependencies at your own time with the node component function ```Resolve Npm Dependencies```.

![resolve npm manually](https://i.imgur.com/3slggp8.png)

#### Multiple scripts

Works, just add another component and all action for a script will be filtered to only communicate to the component that launched it.

#### Limitations

Current builds are Win64 only.

Communication to embeded node.exe takes place internally via socket.io protocol with tcp. Comms and scripts run on background threads with callbacks on game thread (one subprocess for each script). This means nothing blocks while the scripts run, but sub-tick communcation latency is not possible as each message roundtrip will take at least one game tick. e.g. sending a message to your script on this tick will usually result in a callback on next tick.

Very large bandwidth may become an issue (e.g. feeding image data each tick), but hasn't been tested and there are some optimizations that can be used.
