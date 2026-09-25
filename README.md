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

Data is mainly passed to scripts via events: bind ```OnScriptBegin``` and call ```Emit Event``` (see the adder below). This keeps a live two-way channel open.

For one-shot launch arguments, fill `Script Params -> Args` (since v2.1). Your script sees them in `process.argv` (after the script path) and as `ipc.args`.

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
	let c = euclidean(vars.a, vars.b);
	console.log('Got a request (a^2+b^2)^0.5: ' + c);

	//emit result back as a 'result' event
	ipc.emit('result', c);
});

console.log('started');
```

On the blueprint side, our scripts start on begin play (a toggleable property on the node component) and there is an event called ```OnScriptBegin```. Use that event to know the script is ready, then call ```Emit Event``` with the event name ```myevent``` and a JSON string argument, e.g. ```{"a":3,"b":4}```. This JSON arrives in your script as the object passed to your ```ipc.on('myevent', (vars) => ...)``` handler.

```
Event OnScriptBegin --> Emit Event (EventName="myevent", JsonArgs="{\"a\":3,\"b\":4}")
```

When the script emits the ```result``` event, it returns to your component's ```OnEvent``` event. ```OnEvent``` gives you three values:

- ```EventName``` — the emitted event name (e.g. ```result```)
- ```JsonArgs``` — the args as a JSON array string (e.g. ```[5]```); parse it with any JSON utility
- ```Binary``` — a ```TArray<uint8>``` carrying the first interweaved binary buffer, if any (empty otherwise)

That's the basics! There are some other events and functions for e.g. starting/stopping and getting notifications of those states, but largely anything else will be in your node.js script side.

#### Sending binary

To interweave raw bytes, use ```Emit Event With Binary``` from Unreal (the buffer arrives in your script as a trailing Node ```Buffer``` argument), or from your script emit a ```Buffer``` directly: ```ipc.emit('frame', { meta: 1 }, myBuffer)```. On the Unreal side the bytes arrive on ```OnEvent```'s ```Binary``` parameter. Binary travels natively (no base64) so it's suitable for image/audio streaming. See ```Content/Scripts/examples/perfStream.js``` for a throughput example and ```cubeSine.js``` for an async actor-driving demo.

To send several buffers at once use ```Emit Event With Buffers```. They arrive as trailing `Buffer` args, or exactly where you put them if your JSON contains `{"_bin":i}` placeholders, e.g. `{"image":{"_bin":0},"mask":{"_bin":1}}`. Scripts can emit any number of Buffers anywhere in their args. Bind ```OnEventWithBuffers``` to receive all of them, with the script name, as a `Node Event Data` struct.

> The bundled ```ipc-event-emitter``` (in ```Content/Scripts/node_modules```) is wire-compatible with the npm package, so the ```require('ipc-event-emitter').default(process)``` one-liner works out of the box with no ```npm install```, in every launch mode and for scripts in your project folder too.

#### Replies (callbacks)

```Emit Event With Callback``` gives the script's handler a trailing reply function. Whatever the script passes to it (JSON and Buffers) comes back on your `Callback` event:

```js
ipc.on('ask', (question, reply) => {
	reply({ answer: 42 });
});
```

In C++, `EmitEventNative(Name, Args, Buffers, ScriptName, [](const FNodeEventData& Reply){ ... })` does the same with a lambda.

#### Binding events to functions

Instead of switching on `EventName` in `OnEvent`, you can route one event straight to a handler. Bound handlers fire in addition to `OnEvent`.

- ```Bind Event``` (EventName, Callback): use a Blueprint custom event taking `Node Event Data`
- ```Bind Event To Function``` (EventName, FunctionName, Target): calls a function by name. It must take `(String JsonArgs)`, `(String JsonArgs, Byte Array Binary)` or `(Node Event Data Event)`, and a mismatch is logged when you bind
- ```Unbind Event``` removes them; C++ can use `BindEventNative(EventName, Lambda)`

## Packaging

Works since v0.5, just make sure to add the folder where your project Scripts are as additional non-asset directories to copy relative to the Content directory (e.g. for the typical ```Content/Scripts``` folder add just ```Scripts```)

![](https://i.imgur.com/pURWRY7.png)

## Usage Notes

#### Launch modes, stopping and reloading

`Script Params -> Launch Mode` picks how a script runs:

| Mode | How it runs | What Stop / reload does |
|---|---|---|
| **Inline** (default) | Loaded into the node process. Lowest latency | Fires `shouldExit`, calls your `module.exports.dispose()` if you have one, then clears the script's timers (`setTimeout`/`setInterval`/`setImmediate`) and listeners |
| **Worker** | A worker thread inside the node process | Fires `shouldExit`, then terminates the thread. Everything stops, sockets included |
| **Subprocess** | Its own forked node process | Fires `shouldExit`, then disconnects, then kills it |

Inline scripts that open sockets or servers should close them in `shouldExit`, otherwise they outlive the stop:

```js
const server = require('http').createServer(handler).listen(8080);
ipc.on('shouldExit', () => server.close());
```

Calling `Start Script` on a script that's already running restarts it. On EndPlay, scripts get `Shutdown Grace Seconds` (default 0.3) to run their `shouldExit` cleanup before node is terminated.

The v2.0 `Inline Launch Script` flag (advanced) still works: unticked forces Subprocess, whatever `Launch Mode` says.

Create the ipc emitter (`require('ipc-event-emitter').default(process)`) in your entry script, not at the top of a shared helper module. Helper modules are cached across inline scripts, so a module-level emitter would be bound to whichever script loaded that helper first.

#### Live reload

Set `bWatchFileForChanges` (or call ```Watch Script``` later). Saving the script fires `OnScriptChanged`, then with `bReloadOnChange` (default) it reloads: `OnScriptEnd`, `OnScriptReloaded`, `OnScriptBegin`. With `bReloadOnChange` off you only get `OnScriptChanged` and decide yourself. ```Unwatch Script``` stops watching.

#### Script state

`Is Script Running` (empty name = the default script) and `Get Running Scripts` track what's live. `Get Script Full Path` tells you which file a script resolves to, and `Package Dependencies` lists the `dependencies` in its nearest `package.json`.

#### Where scripts are loaded from
Your script (`Default Script Params -> Script`, relative to `Script Path Root`, default `Content/Scripts/`) is looked up in your **project's** `Content/Scripts` first, and if not found there it falls back to the **plugin's** own `Content/Scripts`. That's why the bundled `examples/*.js` run without copying them into your project.

#### Errors
Script errors are emitted on the `OnScriptError` event and, by default, also dumped to the Output Log under the `LogNodeJs` category — so you can see them without wiring anything up. Turn this off with `Node Js Process Params -> Log Script Errors To Output`. Hitting save while watching re-runs the script.

![error](https://i.imgur.com/hh03jnD.png)

#### npm modules

Since v0.2 script errors caused by missing npm modules will auto-check the ```package.json``` in your script folder for missing modules. If the dependency isn't listed it will warn you about it, if it does exist it will auto-resolve the dependencies and re-run your script after installation; auto-fixing your error.

Basically keep your script's ```package.json``` up to date with the required dependencies and it will auto-install them as needed without performance implications (doesn't check every run, only on script error).

![properties](https://i.imgur.com/s5o983w.png)

You can disable this auto-resolving and auto-run on npm install via `Node Js Process Params -> Auto Resolve Npm Dependencies`. Then you can resolve Npm dependencies at your own time with the node component function ```Resolve Npm Dependencies``` (pass the script params; it runs `npm install` in the nearest `package.json` folder of that script and reports on ```OnNpmDependenciesResolved```).

![resolve npm manually](https://i.imgur.com/3slggp8.png)

#### Multiple scripts

Works: one component can run several scripts (pass a `ScriptName` when emitting), and several components can run the same script. Everything for a script is only delivered to the component that launched it.

By default every component starts its own node process. Set `Node Js Process Params -> Share Main Process` on components to run all of their scripts in one shared node process for the game instance instead (saves memory and startup time). Scripts stay isolated per component, and the process is stopped when the game instance shuts down.

#### Tests

- `Content/Scripts/test/harness.js` drives `process.js` without the engine: `Source/ThirdParty/node/node.exe Content/Scripts/test/harness.js`
- In-engine automation tests live under `NodeJs.*`: `UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests NodeJs; Quit" -unattended -nullrhi -nopause -log`


#### Using git instead of releases

Supported, with a few extra steps since the git repo doesn't carry binaries:

1. Clone into `{Project Root}/Plugins/NodeJs-Unreal` (since v2.1 other folder names work too; node is found inside the plugin wherever it's installed).
2. Clone the [CLISystem](https://github.com/getnamo/CLISystem-Unreal) dependency into `{Project Root}/Plugins/CLISystem-Unreal`.
3. Download the [Windows x64 node.js zip](https://nodejs.org/en/download) (the release bundles v24 LTS) and extract its contents into `Plugins/NodeJs-Unreal/Source/ThirdParty/node/` so that `node.exe` sits directly in that folder.

#### Limitations

Current builds are Win64 only.

Since v2.0.0 communication to the embedded node.exe takes place over the process stdin/stdout pipe using a self-delimiting binary frame protocol (built on the [CLISystem](https://github.com/getnamo/CLISystem-Unreal) plugin) — there is no longer any socket.io/TCP server. Logs, events and raw binary interweave on the one stream. Comms and scripts run on background threads with callbacks marshalled to the game thread, so nothing blocks while scripts run, but sub-tick latency is not guaranteed; a message roundtrip will usually take at least one game tick.

Binary is carried natively (no base64), so feeding large/image data is reasonable, though very high per-tick bandwidth should still be profiled for your use case.
