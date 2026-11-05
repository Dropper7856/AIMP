const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const path = require('node:path');
const fs = require('node:fs');
const vm = require('node:vm');
const { test } = require('node:test');
const electronDir = process.env.AIMP_TEST_ELECTRON_DIR || path.join(__dirname, '..', 'AIMP.Electron');
const { runStartupUpdate, canAutoUpdate } = require(path.join(electronDir, 'startup-updater.js'));
const tick = () => new Promise(setImmediate);

function fixture() {
  const app = new EventEmitter();
  app.isPackaged = true;
  app.quitCalls = 0;
  app.quit = () => { app.quitCalls++; app.emit('before-quit'); };
  const updater = new EventEmitter();
  const calls = [];
  const token = { cancelled: false, cancel() { this.cancelled = true; } };
  const available = { isUpdateAvailable: true, updateInfo: { version: '1.0.2' }, cancellationToken: token };
  updater.checkForUpdates = async () => { calls.push('check'); return { isUpdateAvailable: false }; };
  updater.downloadUpdate = async (value) => {
    assert.equal(value, token);
    calls.push('download');
    updater.emit('download-progress', { percent: 41.8 });
    return ['verified-installer.exe'];
  };
  updater.quitAndInstall = (...args) => calls.push(['install', ...args]);
  const view = { ready: Promise.resolve(), states: [], closed: false,
    update(...args) { this.states.push(args); }, close() { this.closed = true; } };
  const logs = [];
  const options = { app, log: (value) => logs.push(value), env: {}, platform: 'win32',
    executablePath: 'C:/Applications/AI-MP/AIMP.Launcher.Desktop.exe',
    exists: (candidate) => path.basename(candidate) === 'Uninstall AIMP.Launcher.Desktop.exe',
    isGameRunning: async () => false,
    getUpdater: () => updater, createView: (cancel) => { view.cancel = cancel; return view; },
    checkTimeoutMs: 100, downloadTimeoutMs: 100 };
  return { app, updater, calls, token, available, view, logs, options, run: () => runStartupUpdate(options) };
}

test('only an installed Windows setup build checks for updates', async () => {
  const f = fixture();
  assert.equal(canAutoUpdate(f.options), true);
  for (const change of [{ platform: 'linux' }, { env: { PORTABLE_EXECUTABLE_FILE: 'portable.exe' } },
    { env: { PORTABLE_EXECUTABLE_DIR: 'C:/Portable' } }, { exists: () => false }]) {
    assert.equal(await runStartupUpdate({ ...f.options, ...change }), 'continue');
  }
  f.app.isPackaged = false;
  assert.equal(await f.run(), 'continue');
  assert.deepEqual(f.calls, []);
});

test('setup detection matches the actual NSIS filename derived by electron-builder', (t) => {
  const sourceDir = path.join(__dirname, '..', 'AIMP.Electron');
  const pkg = JSON.parse(fs.readFileSync(path.join(sourceDir, 'package.json')));
  const { AppInfo } = require(path.join(sourceDir, 'node_modules/app-builder-lib/out/appInfo'));
  const info = new AppInfo({ metadata: pkg, config: pkg.build }, null, pkg.build.win);
  const testRoot = path.resolve(__dirname, '..', 'AIMP.Launcher/src/AIMP.Launcher/bin/updater-install-tests');
  fs.mkdirSync(testRoot, { recursive: true });
  const installDir = fs.mkdtempSync(path.join(testRoot, 'installed-'));
  t.after(() => {
    const resolved = fs.realpathSync(installDir);
    assert.ok(resolved.startsWith(testRoot + path.sep));
    fs.rmSync(resolved, { recursive: true, force: true });
  });
  const executablePath = path.join(installDir, info.productFilename + '.exe');
  const uninstallerPath = path.join(installDir, 'Uninstall ' + info.productFilename + '.exe');
  fs.writeFileSync(executablePath, 'Synthetic file; do not execute.');
  fs.writeFileSync(uninstallerPath, 'Synthetic file; do not execute.');
  const options = { app: { isPackaged: true }, env: {}, platform: 'win32', executablePath, exists: fs.existsSync };
  assert.equal(canAutoUpdate(options), true, 'A genuine NSIS installation must enable updates.');
  fs.unlinkSync(uninstallerPath);
  assert.equal(canAutoUpdate(options), false, 'An unpacked copy must remain excluded.');
});

test('current version starts normally without downloading or installing', async () => {
  const f = fixture();
  assert.equal(await f.run(), 'continue');
  assert.deepEqual(f.calls, ['check']);
  assert.equal(f.view.closed, true);
  assert.equal(f.updater.autoDownload, false);
  assert.equal(f.updater.autoInstallOnAppQuit, false);
  assert.equal(f.updater.allowPrerelease, false);
  assert.equal(f.updater.allowDowngrade, false);
  assert.equal(f.updater.disableWebInstaller, true);
});

test('a running game or uncertain process state defers updates to avoid replacing loaded DLLs', async () => {
  for (const isGameRunning of [async () => true, async () => { throw new Error('tasklist failed'); }]) {
    const f = fixture();
    assert.equal(await runStartupUpdate({ ...f.options, isGameRunning }), 'continue');
    assert.deepEqual(f.calls, []);
  }
});

test('new release is downloaded before a silent install and restart', async () => {
  const f = fixture();
  f.updater.checkForUpdates = async () => { f.calls.push('check'); return f.available; };
  assert.equal(await f.run(), 'installing');
  assert.deepEqual(f.calls, ['check', 'download', ['install', true, true]]);
  assert.ok(f.view.states.some((state) => state[2] === 41));
  assert.equal(f.view.states.at(-1)[0], 'INSTALLING UPDATE');
  assert.equal(f.view.closed, false);
});

test('offline or missing release metadata keeps the launcher usable', async () => {
  for (const reason of ['Network unavailable', 'latest.yml not found']) {
    const f = fixture();
    f.updater.checkForUpdates = async () => { throw new Error(reason); };
    assert.equal(await f.run(), 'continue');
    assert.deepEqual(f.calls, []);
    assert.equal(f.view.closed, true);
    assert.ok(f.logs.some((line) => line.includes(reason)));
  }
});

test('a game started during download prevents installation of the downloaded update', async () => {
  const f = fixture();
  let checks = 0;
  f.options.isGameRunning = async () => ++checks > 1;
  f.updater.checkForUpdates = async () => f.available;
  assert.equal(await f.run(), 'continue');
  assert.deepEqual(f.calls, ['download']);
  assert.equal(f.view.closed, true);
});

test('a timed-out check cannot download or install later', async () => {
  const f = fixture();
  let resolveCheck;
  f.options.checkTimeoutMs = 5;
  f.updater.checkForUpdates = () => new Promise((resolve) => { resolveCheck = resolve; });
  assert.equal(await f.run(), 'continue');
  resolveCheck(f.available);
  await tick();
  f.updater.emit('error', new Error('Late network failure'));
  assert.deepEqual(f.calls, []);
  assert.equal(f.view.closed, true);
});

test('download or checksum failures never launch the installer', async () => {
  const f = fixture();
  f.updater.checkForUpdates = async () => f.available;
  f.updater.downloadUpdate = async () => { throw new Error('sha512 checksum mismatch'); };
  assert.equal(await f.run(), 'continue');
  assert.deepEqual(f.calls, []);
  assert.equal(f.view.closed, true);
});

test('a download timeout cancels the transfer and never installs a late result', async () => {
  const f = fixture();
  let resolveDownload;
  f.options.downloadTimeoutMs = 5;
  f.updater.checkForUpdates = async () => f.available;
  f.updater.downloadUpdate = () => new Promise((resolve) => { resolveDownload = resolve; });
  assert.equal(await f.run(), 'continue');
  assert.equal(f.token.cancelled, true);
  resolveDownload(['late-installer.exe']);
  await tick();
  assert.deepEqual(f.calls, []);
});

test('closing the update window cancels download and quits instead of launching the game UI', async () => {
  const f = fixture();
  let rejectDownload;
  f.updater.checkForUpdates = async () => f.available;
  f.updater.downloadUpdate = () => new Promise((_, reject) => { rejectDownload = reject; });
  const run = f.run();
  await tick();
  f.view.cancel();
  rejectDownload(new Error('cancelled'));
  assert.equal(await run, 'quit');
  assert.equal(f.token.cancelled, true);
  assert.equal(f.app.quitCalls, 1);
  assert.deepEqual(f.calls, []);
});

test('installer launch errors fall back to the current launcher', async () => {
  const f = fixture();
  f.updater.checkForUpdates = async () => f.available;
  f.updater.quitAndInstall = () => f.updater.emit('error', new Error('Installer could not be started'));
  assert.equal(await f.run(), 'continue');
  assert.equal(f.view.closed, true);
});

test('main process starts the API only after the updater permits normal startup', async () => {
  const source = fs.readFileSync(path.join(electronDir, 'main.js'), 'utf8');
  for (const outcome of ['continue', 'installing', 'quit']) {
    let resolveUpdate;
    let launched = 0;
    let ready;
    const handlers = {};
    const app = { isPackaged: true, disableHardwareAcceleration() {}, requestSingleInstanceLock: () => true,
      whenReady: () => ({ then(callback) { ready = callback; } }),
      on: (name, handler) => { handlers[name] = handler; }, getPath: () => 'userdata',
      getVersion: () => '1.0.1', quit() {} };
    const context = { process: { argv: ['launcher'], env: {}, resourcesPath: 'resources', execPath: 'launcher.exe' },
      URL, console: { log() {}, error() {} }, __dirname: electronDir,
      require(name) {
        if (name === 'electron') return { app, ipcMain: { on() {} }, dialog: { showErrorBox() {} } };
        if (name === './startup-updater') return { runStartupUpdate: () => new Promise((resolve) => { resolveUpdate = resolve; }) };
        if (name.startsWith('./')) return require(path.join(electronDir, name));
        return require(name);
      } };
    vm.runInNewContext(source + `
      loadLauncherConfiguration = () => ({frontendOrigin:'https://aimp.dblabs.ca', frontendUrl:'https://aimp.dblabs.ca/sm-link'});
      startApi = () => launched(); waitForApi = async () => {}; createWindow = async () => {};
    `, { ...context, launched: () => { launched++; } });
    const startup = ready();
    await tick();
    assert.equal(launched, 0);
    handlers['window-all-closed'](); // Closing the splash must not quit normal startup.
    resolveUpdate(outcome);
    await startup;
    assert.equal(launched, outcome === 'continue' ? 1 : 0);
  }
});
