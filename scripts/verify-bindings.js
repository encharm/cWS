// Refuses to publish when a prebuilt binding does not match the platform/arch in its filename or
// when dist/bindings has uncommitted changes. Reads the binary headers directly (no `file`).
// Written after 4.14.2 shipped cws_linux_x64_node137.node as an aarch64 test build.
const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

const dir = path.join(__dirname, '..', 'dist', 'bindings');
const problems = [];

function archOf(buf) {
  if (buf.readUInt32BE(0) === 0x7f454c46) {                       // ELF
    const machine = buf.readUInt16LE(18);
    return { platform: 'linux', arch: machine === 0x3e ? 'x64' : machine === 0xb7 ? 'arm64' : `elf-machine-${machine}` };
  }
  const magic = buf.readUInt32LE(0);
  if (magic === 0xfeedfacf || magic === 0xfeedface) {              // Mach-O 64/32
    const cputype = buf.readUInt32LE(4);
    return { platform: 'darwin', arch: cputype === 0x0100000c ? 'arm64' : cputype === 0x01000007 ? 'x64' : `cputype-${cputype}` };
  }
  if (buf.readUInt16LE(0) === 0x5a4d) {                            // PE: 'MZ', e_lfanew at 0x3c, machine after 'PE\0\0'
    const pe = buf.readUInt32LE(0x3c);
    const machine = buf.readUInt16LE(pe + 4);
    return { platform: 'win32', arch: machine === 0x8664 ? 'x64' : machine === 0xaa64 ? 'arm64' : `pe-machine-${machine}` };
  }
  return { platform: 'unknown', arch: 'unknown' };
}

for (const name of fs.readdirSync(dir).filter(n => n.endsWith('.node')).sort()) {
  const m = /^cws_(linux|darwin|win32)_(x64|arm64)_node\d+\.node$/.exec(name);
  if (!m) { problems.push(`${name}: unexpected filename`); continue; }
  const buf = fs.readFileSync(path.join(dir, name));
  const got = archOf(buf);
  if (got.platform !== m[1] || got.arch !== m[2]) problems.push(`${name}: file is ${got.platform}/${got.arch}, filename says ${m[1]}/${m[2]}`);
  if (buf.length > 2 * 1024 * 1024) problems.push(`${name}: ${(buf.length / 1024 / 1024).toFixed(1)} MB, looks like a debug or sanitizer build`);
}

try {
  const dirty = execSync('git status --porcelain -- dist/bindings', { cwd: path.join(__dirname, '..'), encoding: 'utf8' }).trim();
  if (dirty) problems.push(`dist/bindings has uncommitted changes:\n${dirty}`);
} catch { /* not a git checkout: skip */ }

if (problems.length) {
  console.error('verify-bindings: refusing to publish\n  ' + problems.join('\n  '));
  process.exit(1);
}
console.log('verify-bindings: all bindings match their filenames and are committed');
