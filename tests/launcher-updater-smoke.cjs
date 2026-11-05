// Run with Electron. Uses a local feed and intercepts installer execution;
// no GitHub release is published and no installed application is replaced.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { createHash } = require('node:crypto');
const { EventEmitter } = require('node:events');
const { app, BrowserWindow } = require('electron');
const root = path.resolve(__dirname, '..');
const electronDir = process.env.AIMP_TEST_ELECTRON_DIR || path.join(root, 'AIMP.Electron');
const { NsisUpdater } = require(path.join(electronDir, 'node_modules/electron-updater'));
const { ElectronHttpExecutor } = require(path.join(electronDir, 'node_modules/electron-updater/out/electronHttpExecutor'));
const { runStartupUpdate, createUpdateWindow } = require(path.join(electronDir, 'startup-updater.js'));
const runDir = path.join(root, 'analysis/updater-repro', String(Date.now()));
fs.mkdirSync(runDir, { recursive: true });
const fixture = Buffer.from('Harmless synthetic updater fixture. Never execute this file.');
const digest = createHash('sha512').update(fixture).digest('base64');
const scenarios = [
  { name: 'current', version: '1.0.1', outcome: 'continue', downloads: 0, installs: 0 },
  { name: 'older', version: '1.0.0', outcome: 'continue', downloads: 0, installs: 0 },
  { name: 'newer', version: '1.0.2', outcome: 'installing', downloads: 1, installs: 1 },
  { name: 'corrupted', version: '1.0.2', corrupt: true, outcome: 'continue', downloads: 1, installs: 0 },
  { name: 'missing-manifest', missing: true, outcome: 'continue', downloads: 0, installs: 0 },
];
const results = [];
app.disableHardwareAcceleration();
app.on('window-all-closed', () => {});
setTimeout(() => { console.error('Updater runtime test timed out.'); app.exit(1); }, 45000).unref();

app.whenReady().then(async () => {
  let server;
  let activeView;
  try {
    for (const scenario of scenarios) {
      const caseDir = path.join(runDir, scenario.name);
      fs.mkdirSync(caseDir);
      const executablePath = path.join(caseDir, 'AIMP.Launcher.Desktop.exe');
      fs.writeFileSync(executablePath, 'Synthetic file; do not execute.');
      fs.writeFileSync(path.join(caseDir, 'Uninstall AIMP.Launcher.Desktop.exe'), 'Synthetic file; do not execute.');
      let downloads = 0;
      let installs = 0;
      const logs = [];
      server = http.createServer((request, response) => {
        const pathname = new URL(request.url, 'http://localhost').pathname;
        if (pathname === '/latest.yml' && !scenario.missing) {
          response.end(`version: ${scenario.version}\nfiles:\n  - url: setup-${scenario.version}.exe\n    sha512: ${digest}\n    size: ${fixture.length}\npath: setup-${scenario.version}.exe\nsha512: ${digest}\n`);
        } else if (pathname.endsWith('.exe')) {
          downloads++;
          response.setHeader('Content-Length', fixture.length);
          response.end(scenario.corrupt ? Buffer.alloc(fixture.length, 65) : fixture);
        } else response.writeHead(404).end();
      });
      await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
      const feed = 'http://127.0.0.1:' + server.address().port;
      const configPath = path.join(caseDir, 'app-update.yml');
      fs.writeFileSync(configPath, `provider: generic\nurl: ${feed}\nupdaterCacheDirName: updater-cache\n`);
      const lifecycle = new EventEmitter();
      lifecycle.isPackaged = true;
      lifecycle.quit = () => lifecycle.emit('before-quit');
      const adapter = { version: '1.0.1', name: 'AI-MP updater test', isPackaged: true,
        appUpdateConfigPath: configPath, userDataPath: caseDir, baseCachePath: caseDir,
        whenReady: () => app.whenReady(), onQuit() {}, quit: () => lifecycle.quit(), relaunch() {} };
      class TestUpdater extends NsisUpdater {
        doInstall(options) {
          assert.equal(options.isSilent, true);
          assert.equal(options.isForceRunAfter, true);
          assert.deepEqual(fs.readFileSync(this.installerPath), fixture);
          installs++;
          return true;
        }
      }
      const updater = new TestUpdater(null, adapter);
      updater.httpExecutor = new ElectronHttpExecutor();
      updater.disableDifferentialDownload = true;
      updater.setFeedURL({ provider: 'generic', url: feed }); // Local test only; production uses app-update.yml.
      let activeWindow;
      function HiddenWindow(options) {
        const window = new BrowserWindow({ ...options, show: false });
        window.show = () => {}; // Exercise the actual local HTML without opening a desktop window.
        activeWindow = window;
        return window;
      }
      const outcome = await runStartupUpdate({ app: lifecycle, log: (line) => logs.push(line),
        env: {}, platform: 'win32', executablePath, exists: fs.existsSync,
        isGameRunning: async () => false,
        getUpdater: () => updater, createView: (cancel) => {
          activeView = createUpdateWindow(HiddenWindow, cancel);
          return activeView;
        }, checkTimeoutMs: 5000, downloadTimeoutMs: 5000 });
      await new Promise(setImmediate);
      assert.equal(outcome, scenario.outcome, scenario.name);
      assert.equal(downloads, scenario.downloads, scenario.name);
      assert.equal(installs, scenario.installs, scenario.name);
      if (outcome === 'installing') {
        const ui = await activeWindow.webContents.executeJavaScript(`({
          status: document.getElementById('status').textContent,
          progress: document.getElementById('progress').value
        })`);
        assert.equal(ui.status, 'INSTALLING UPDATE');
        assert.equal(ui.progress, 100);
      }
      if (scenario.corrupt) assert.ok(logs.some((line) => /checksum|sha512/i.test(line)), 'Checksum failure must be reported.');
      activeView.close();
      activeView = null;
      await new Promise((resolve) => server.close(resolve));
      server = null;
      results.push({ scenario: scenario.name, outcome, downloads, installs, passed: true });
    }
    fs.writeFileSync(path.join(runDir, 'result.json'), JSON.stringify(results, null, 2));
    console.log(JSON.stringify(results, null, 2));
    console.log('PASS real electron-updater runtime and local update window. Installer execution intercepted in tests.');
    app.exit(0);
  } catch (error) {
    console.error(error);
    app.exit(1);
  } finally { activeView?.close(); server?.close(); }
});
