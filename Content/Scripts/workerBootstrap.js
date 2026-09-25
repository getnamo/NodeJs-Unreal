/**
 *  NodeJs-Unreal - workerBootstrap.js
 *
 *  Entry point for scripts launched in Worker mode. process.js starts this in a
 *  worker_threads Worker; it points process.argv at the user script and loads it.
 *  The bundled ipc-event-emitter detects the worker (workerData.__nodeJsUnreal)
 *  and talks to process.js over parentPort.
 */

'use strict';

const { workerData } = require('worker_threads');

process.argv[1] = workerData.fullPath;
require(workerData.fullPath);
