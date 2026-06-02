// Test fixture: requires a package that does not exist and is not in package.json.
// Exercises the npm auto-resolve "not listed -> warn, do not install" path.
require('definitely-not-a-real-pkg-xyz');
console.log('should not reach here');
