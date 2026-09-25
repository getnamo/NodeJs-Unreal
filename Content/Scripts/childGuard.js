/**
 *  NodeJs-Unreal - childGuard.js
 *
 *  Preloaded (--require) into Subprocess-mode scripts. If the parent process.js goes
 *  away (Unreal stopped or crashed, node was killed), the IPC channel disconnects:
 *  give the script a moment to run its 'shouldExit' cleanup, then exit so it can't
 *  outlive Unreal. On Linux, orphaned children otherwise keep running.
 */

'use strict';

const ORPHAN_GRACE_MS = 500;

if (typeof process.send === 'function') {
	process.on('disconnect', () => {
		setTimeout(() => process.exit(0), ORPHAN_GRACE_MS);
	});
}
