// npm/yarn `install` hook. The package ships prebuilt bindings under dist/bindings; only
// fall back to a node-gyp source build (against Node's own zlib, no zlib-ng) when none
// matches this platform and Node ABI. Set CWS_FORCE_BUILD=1 to build regardless.
// The build is best effort: failures are logged to build_log.txt and never fail the install.
'use strict';
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.join(__dirname, '..');
const binding = path.join(root, 'dist', 'bindings', `cws_${process.platform}_${process.arch}_node${process.versions.modules}.node`);

if (fs.existsSync(binding) && !process.env.CWS_FORCE_BUILD) {
  process.exit(0);
}

const log = fs.openSync(path.join(root, 'build_log.txt'), 'w');
fs.writeSync(log, process.env.CWS_FORCE_BUILD ? `CWS_FORCE_BUILD set, building from source (prebuilt: ${binding})\n` : `no prebuilt binding at ${binding}, building from source\n`);
spawnSync('node-gyp', ['rebuild'], { cwd: root, stdio: ['ignore', log, log], shell: true });
fs.closeSync(log);
process.exit(0);
