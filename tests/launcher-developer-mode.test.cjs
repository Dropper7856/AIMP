const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

const root = path.resolve(__dirname, '..');

test('Electron forwards only the explicit -dev argument to the API', () => {
  const sourcePath = process.env.AIMP_TEST_ELECTRON_MAIN || path.join(root, 'AIMP.Electron/main.js');
  const source = fs.readFileSync(sourcePath, 'utf8');
  for (const [args, expected] of [
    [[], []], [['-dev'], ['-dev']], [['-DEV'], ['-dev']],
    [['--dev', '-developer', '--debug'], []],
  ]) {
    let launch;
    const app = {
      isPackaged: true, disableHardwareAcceleration() {}, requestSingleInstanceLock: () => false,
      quit() {}, on() {}, getPath: () => 'userdata',
    };
    const child = { stdout: { pipe() {} }, stderr: { pipe() {} }, once() {} };
    const context = {
      require(name) {
        if (name.startsWith('./')) return require(path.resolve(path.dirname(sourcePath), name));
        if (name === 'electron') return { app, ipcMain: { on() {} } };
        if (name === 'node:child_process') return {
          spawn(executable, arguments_, options) { launch = { executable, arguments_, options }; return child; },
        };
        if (name === 'node:fs') return {
          existsSync: () => true, mkdirSync() {}, appendFileSync() {},
          createWriteStream: () => ({ write() {} }),
        };
        return require(name);
      },
      process: { argv: ['launcher.exe', ...args], resourcesPath: 'resources', execPath: 'launcher.exe', env: {} },
      console: { log() {}, error() {} }, __dirname: root,
    };
    vm.runInNewContext(`${source}\nstartApi('https://aimp.dblabs.ca');`, context);
    assert.deepEqual(Array.from(launch.arguments_), expected);
    assert.equal(path.basename(launch.executable), 'AIMP.Launcher.exe');
    assert.equal(launch.options.windowsHide, true);
  }
});

// Execute the small menu handlers with their UI dependencies stubbed. The
// compiled GFX files are checked separately after importing these AS3 sources.
function menuMethod(source, name) {
  const signature = new RegExp(`private function ${name}\\([^)]*\\) : void\\s*\\{`, 'g');
  const match = signature.exec(source);
  assert.ok(match, name);
  const start = signature.lastIndex;
  let depth = 1;
  let end = start;
  while (depth && end < source.length) {
    if (source[end] === '{') depth++;
    if (source[end] === '}') depth--;
    end++;
  }
  const body = source.slice(start, end - 1).replace(/\b(var\s+\w+):\w+/g, '$1')
    .replace(/\bint\(/g, 'Number(');
  return new Function(name === 'voiceSettings' ? 'values' : 'event', body);
}

for (const [menu, className] of [['NEWFRONTENDMENU', 'NewFrontendMenu'], ['PAUSEMENU', 'PauseMenu']]) {
  test(`${menu}: developer option stays hidden until the launcher grants access`, () => {
    const source = fs.readFileSync(path.join(root,
      `AIMP.Launcher/src/AIMP.Launcher/Resources/UI/Source/${menu}/frontend/${className}.as`), 'utf8');
    assert.match(source, /_developerModeAvailable:Boolean = false/);
    assert.match(source, /this\._developerItem\.visible = false/);
    const sends = [];
    const context = {
      _developerItem: {}, _voiceDevices: [], voiceLabels() {},
      voiceSend: (...args) => sends.push(args),
    };
    const refresh = menuMethod(source, 'voiceSettings');
    const toggle = menuMethod(source, 'developerToggle');
    for (const [available, enabled, visible, active] of [
      [undefined, '1', false, false], ['0', '1', false, false],
      ['1', '0', true, false], ['1', '1', true, true], ['0', '0', false, false],
    ]) {
      refresh.call(context, { developerModeAvailable: available, developerMode: enabled, count: '0' });
      assert.equal(context._developerItem.visible, visible);
      assert.equal(context._developerMode, active);
      const previous = sends.length;
      toggle.call(context, null);
      assert.equal(sends.length, previous + (visible ? 1 : 0));
      if (visible) assert.deepEqual(sends.at(-1), ['DEVELOPER_MODE', { enabled: active ? '0' : '1' }]);
    }
  });
}
