# NodeJs-Unreal
Embed node.js as an unreal plugin. This enables you to embed cool things like: https://www.npmjs.com/.

[![GitHub release](https://img.shields.io/github/release/getnamo/NodeJs-Unreal.svg)](https://github.com/getnamo/NodeJs-Unreal/releases)
[![Github All Releases](https://img.shields.io/github/downloads/getnamo/NodeJs-Unreal/total.svg)](https://github.com/getnamo/NodeJs-Unreal/releases)

Want to control unreal with javascript? consider using [UnrealJs](https://github.com/getnamo/Unrealjs) which is much more feature-rich. This plugin instead focuses on bringing node.js and npm api on background threads.

Currently in an early working state, may have bugs!


[![](https://i.imgur.com/kk6lERT.gif)](https://i.imgur.com/qhVMtde.mp4)

Novelty example controlling boxes using javascript with live reload in an async loop which shows upward of ~20k messages/sec not impacting game thread.

#### Questions and Feedback

Got questions or problems? post to https://github.com/getnamo/NodeJs-Unreal/issues

[Unreal forum thread](https://forums.unrealengine.com/community/work-in-progress/1644397-node-js)

[Discord Server](https://discord.gg/qfJUyxaW4s)

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
 1. [Download Latest Release](https://github.com/getnamo/NodeJs-Unreal/releases)
 2. Create new or choose project.
 3. Browse to your project folder (typically found at Documents/Unreal Project/{Your Project Root})
 4. Copy *Plugins* folder into your Project root.
 5. Plugin should be now ready to use.
 
## How to use - Basics

### Early example project

See NodeJSExampleProject-v0.4.2.7z in https://github.com/getnamo/NodeJs-Unreal/releases/tag/0.4.2 for a drag and drop example project.

### Blueprint side

Add a ```Node Component``` to actor of choice

![add component](https://i.imgur.com/Xvc6v98.png)

In your component properties set the name of the script you wish to run e.g. ```myscript.js```. This path is relative to ```{Your Project Root}/Content/Scripts/```.

![set script](https://i.imgur.com/xalQplZ.png)

### Passing data to scripts

Since v2.0.0 data is passed to scripts via events rather than command-line arguments: bind ```OnScriptBegin``` and call ```Emit Event``` (see the adder below). This keeps a live two-way channel open instead of one-shot launch args.

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

On the blueprint side, our scripts start on begin play (a toggleable property on the node component) and there is an event called ```OnScriptBegin```. Use that event to know the script is ready, then call ```Emit Event``` with the event name ```myevent``` and a JSON string argument, e.g. ```{"x":3,"y":4}```. This JSON arrives in your script as the object passed to your ```ipc.on('myevent', (vars) => ...)``` handler.

```
Event OnScriptBegin --> Emit Event (EventName="myevent", JsonArgs="{\"x\":3,\"y\":4}")
```

When the script emits the ```result``` event, it returns to your component's ```OnEvent``` event. ```OnEvent``` gives you three values:

- ```EventName``` — the emitted event name (e.g. ```result```)
- ```JsonArgs``` — the args as a JSON array string (e.g. ```[5]```); parse it with any JSON utility
- ```Binary``` — a ```TArray<uint8>``` carrying the first interweaved binary buffer, if any (empty otherwise)

That's the basics! There are some other events and functions for e.g. starting/stopping and getting notifications of those states, but largely anything else will be in your node.js script side.

#### Sending binary

To interweave raw bytes, use ```Emit Event With Binary``` from Unreal (the buffer arrives in your script as a trailing Node ```Buffer``` argument), or from your script emit a ```Buffer``` directly: ```ipc.emit('frame', { meta: 1 }, myBuffer)```. On the Unreal side the bytes arrive on ```OnEvent```'s ```Binary``` parameter. Binary travels natively (no base64) so it's suitable for image/audio streaming. See ```Content/Scripts/examples/perfStream.js``` for a throughput example and ```cubeSine.js``` for an async actor-driving demo.

> The bundled ```ipc-event-emitter``` (in ```Content/Scripts/node_modules```) is wire-compatible with the npm package, so the ```require('ipc-event-emitter').default(process)``` one-liner works out of the box with no ```npm install``` — for both inline and subprocess scripts.

## Packaging

Works since v0.5, just make sure to add the folder where your project Scripts are as additional non-asset directories to copy relative to the Content directory (e.g. for the typical ```Content/Scripts``` folder add just ```Scripts```)

![](https://i.imgur.com/pURWRY7.png)

## Usage Notes

#### Where scripts are loaded from
Your script (`Default Script Params -> Script`, relative to `Script Path Root`, default `Content/Scripts/`) is looked up in your **project's** `Content/Scripts` first, and if not found there it falls back to the **plugin's** own `Content/Scripts`. That's why the bundled `examples/*.js` run without copying them into your project.

#### Errors
Script errors are emitted on the `OnScriptError` event and, by default, also dumped to the Output Log under the `LogNodeJs` category — so you can see them without wiring anything up. Turn this off with `Node Js Process Params -> Log Script Errors To Output`. Hitting save while watching re-runs the script.

![error](https://i.imgur.com/hh03jnD.png)

#### npm modules

Since v0.2 script errors caused by missing npm modules will auto-check the ```package.json``` in your script folder for missing modules. If the dependency isn't listed it will warn you about it, if it does exist it will auto-resolve the dependencies and re-run your script after installation; auto-fixing your error.

Basically keep your script's ```package.json``` up to date with the required dependencies and it will auto-install them as needed without performance implications (doesn't check every run, only on script error).

![properties](https://i.imgur.com/s5o983w.png)

You can disable this auto-resolving and auto-run on npm install via the node component properties. Then you can resolve Npm dependencies at your own time with the node component function ```Resolve Npm Dependencies```.

![resolve npm manually](https://i.imgur.com/3slggp8.png)

#### Multiple scripts

Works, just add another component and all action for a script will be filtered to only communicate to the component that launched it.


#### Using git instead of releases

This is supported, just download https://github.com/getnamo/NodeJs-Unreal/releases/download/0.5.0/nodejs-v0.5.0git-thirdparty-dependencies-only.7z in https://github.com/getnamo/NodeJs-Unreal/releases/tag/0.5.0 release and extract it into your project root (where the plugins folder is). This will add dependencies that are missing if you pulled a fresh clone from git.

#### Limitations

Current builds are Win64 only.

Since v2.0.0 communication to the embedded node.exe takes place over the process stdin/stdout pipe using a self-delimiting binary frame protocol (built on the [CLISystem](https://github.com/getnamo/CLISystem-Unreal) plugin) — there is no longer any socket.io/TCP server. Logs, events and raw binary interweave on the one stream. Comms and scripts run on background threads with callbacks marshalled to the game thread, so nothing blocks while scripts run, but sub-tick latency is not guaranteed; a message roundtrip will usually take at least one game tick.

Binary is carried natively (no base64), so feeding large/image data is reasonable, though very high per-tick bandwidth should still be profiled for your use case.
