const { app, BrowserWindow, dialog, ipcMain, shell } = require('electron');
const { execFile, spawn } = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { createApiSecurity } = require('./api-security');
const { runStartupUpdate } = require('./startup-updater');

// Split-screen: several launchers may run at once, each controlling its own
// game. Slot 1 is the usual launcher; slots 2+ get their own user data
// (profile, logs) and API port. "--new-instance" skips the confirmation.
const MAX_INSTANCES = 4;
const NEW_INSTANCE_ARGUMENT = '--new-instance';
const DEFAULT_USER_DATA = app.getPath('userData');

function userDataForSlot(slot) {
  return slot === 1 ? DEFAULT_USER_DATA : `${DEFAULT_USER_DATA}-instance-${slot}`;
}

function acquireInstanceSlot() {
  for (let slot = 1; slot <= MAX_INSTANCES; slot += 1) {
    app.setPath('userData', userDataForSlot(slot));
    if (app.requestSingleInstanceLock({ probe: true })) return slot;
  }
  app.setPath('userData', userDataForSlot(1));
  return 0;
}

const instanceSlot = acquireInstanceSlot();
const API_URL = `http://127.0.0.1:${5127 + Math.max(instanceSlot, 1) - 1}`;
const API_HEALTH_URL = `${API_URL}/`;
const API_SHUTDOWN_URL = `${API_URL}/api/v1/application/shutdown`;
const API_EXECUTABLE = 'AIMP.Launcher.exe';
const apiSecurity = createApiSecurity(API_URL);

// The launcher UI is CSS-only. Software rendering avoids black Chromium surfaces
// on clean VMs and machines with old or missing GPU drivers.
app.disableHardwareAcceleration();

let mainWindow = null;
let apiProcess = null;
let apiLog = null;
let quitting = false;
let shutdownComplete = false;
let frontendUrlForRetry = null;
let startupInProgress = true;

function resourcesPath(...segments) {
  return path.join(app.isPackaged ? process.resourcesPath : __dirname, ...segments);
}

function loadLauncherConfiguration() {
  const configPath = resourcesPath('launcher.config.json');
  const config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
  const frontendUrl = process.env.AIMP_FRONTEND_URL || config.frontendUrl;
  if (typeof frontendUrl !== 'string' || frontendUrl.trim().length === 0) {
    throw new Error(`frontendUrl is missing from ${configPath}.`);
  }

  const parsed = new URL(frontendUrl);
  if (!['http:', 'https:'].includes(parsed.protocol)) {
    throw new Error('The launcher frontend URL must use HTTP or HTTPS.');
  }

  return { frontendUrl: parsed.toString(), frontendOrigin: parsed.origin };
}

function apiDirectory() {
  return resourcesPath('api');
}

function apiLogPath() {
  return path.join(app.getPath('userData'), 'logs', 'aimp-launcher-api.log');
}

function electronLogPath() {
  return path.join(app.getPath('userData'), 'logs', 'aimp-electron.log');
}

function logElectron(message) {
  const line = `[${new Date().toISOString()}] ${message}\n`;
  try {
    fs.mkdirSync(path.dirname(electronLogPath()), { recursive: true });
    fs.appendFileSync(electronLogPath(), line, 'utf8');
  } catch (error) {
    console.error('Unable to write Electron log:', error);
  }
  console.log(message);
}

function electronExecutableDirectory() {
  return process.env.PORTABLE_EXECUTABLE_DIR || path.dirname(process.execPath);
}

function startApi(frontendOrigin) {
  const directory = apiDirectory();
  const executable = path.join(directory, API_EXECUTABLE);
  if (!fs.existsSync(executable)) {
    throw new Error(`Embedded launcher API not found: ${executable}`);
  }

  fs.mkdirSync(path.dirname(apiLogPath()), { recursive: true });
  apiLog = fs.createWriteStream(apiLogPath(), { flags: 'a' });
  apiLog.write(`\n[${new Date().toISOString()}] Starting ${executable}\n`);
  logElectron(`Starting embedded API: ${executable}`);

  const apiArguments = process.argv.some((argument) => argument.toLowerCase() === '-dev')
    ? ['-dev']
    : [];
  apiProcess = spawn(executable, apiArguments, {
    cwd: directory,
    windowsHide: true,
    detached: false,
    stdio: ['ignore', 'pipe', 'pipe'],
    env: {
      ...process.env,
      ASPNETCORE_ENVIRONMENT: 'Production',
      ASPNETCORE_URLS: API_URL,
      AIMP_ELECTRON_ROOT: electronExecutableDirectory(),
      AIMP_FRONTEND_ORIGIN: frontendOrigin,
      AIMP_API_TOKEN: apiSecurity.token,
      AIMP_INSTANCE: String(instanceSlot),
    },
  });

  apiProcess.stdout.pipe(apiLog);
  apiProcess.stderr.pipe(apiLog);
  apiProcess.once('error', (error) => {
    apiLog?.write(`[${new Date().toISOString()}] API start error: ${error.stack}\n`);
  });
  apiProcess.once('exit', (code, signal) => {
    apiLog?.end(
      `[${new Date().toISOString()}] API stopped. Code=${code}, signal=${signal}\n`,
    );
    apiLog = null;
    apiProcess = null;

    if (!quitting) {
      dialog.showErrorBox(
        'AI-MP Launcher API stopped',
        `The local backend stopped unexpectedly.\n\nLog: ${apiLogPath()}`,
      );
      app.quit();
    }
  });
}

function requestJson(url, options = {}) {
  return new Promise((resolve, reject) => {
    const request = http.request(url, apiSecurity.nativeRequestOptions(url, options), (response) => {
      const chunks = [];
      response.on('data', (chunk) => chunks.push(chunk));
      response.on('end', () => {
        const body = Buffer.concat(chunks).toString('utf8');
        if ((response.statusCode || 500) >= 400) {
          reject(new Error(`HTTP ${response.statusCode}: ${body}`));
          return;
        }
        try {
          resolve(body.length > 0 ? JSON.parse(body) : null);
        } catch (error) {
          reject(error);
        }
      });
    });
    request.setTimeout(1_000, () => request.destroy(new Error('Request timed out')));
    request.once('error', reject);
    request.end();
  });
}

async function waitForApi(timeoutMs = 30_000) {
  const deadline = Date.now() + timeoutMs;
  let lastError = null;
  while (Date.now() < deadline) {
    if (apiProcess?.exitCode !== null) {
      throw new Error(`The embedded API exited with code ${apiProcess.exitCode}.`);
    }
    try {
      const info = await requestJson(API_HEALTH_URL);
      if (info?.name === 'AIMP Launcher API') {
        logElectron(`Embedded API ready: version=${info.version || 'unknown'}`);
        return;
      }
      lastError = new Error('Another application is using the launcher API port.');
    } catch (error) {
      lastError = error;
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  throw new Error(`The embedded API did not become ready: ${lastError?.message || 'timeout'}`);
}

function waitForApiExit(timeoutMs) {
  if (!apiProcess) return Promise.resolve(true);
  return new Promise((resolve) => {
    const processReference = apiProcess;
    const timeout = setTimeout(() => resolve(false), timeoutMs);
    processReference.once('exit', () => {
      clearTimeout(timeout);
      resolve(true);
    });
  });
}

async function stopApi() {
  if (!apiProcess?.pid) return;
  const pid = apiProcess.pid;
  const exitPromise = waitForApiExit(5_000);
  try {
    await requestJson(API_SHUTDOWN_URL, { method: 'POST' });
  } catch (error) {
    apiLog?.write(`[${new Date().toISOString()}] Graceful shutdown failed: ${error.message}\n`);
  }

  if (await exitPromise) return;
  if (process.platform === 'win32') {
    await new Promise((resolve) => execFile('taskkill', ['/pid', String(pid), '/T', '/F'], resolve));
  } else {
    apiProcess?.kill('SIGTERM');
  }
}

function errorPage(title, details, retryUrl) {
  const escape = (value) =>
    String(value)
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;');
  const retryScript = JSON.stringify(retryUrl || '').replaceAll('<', '\\u003c');
  return `<!doctype html>
<html><head><meta charset="utf-8"><title>${escape(title)}</title>
<style>
html,body{height:100%;margin:0;background:#010806;color:#8dffc4;font:15px Consolas,monospace}
body{display:grid;place-items:center;padding:32px;box-sizing:border-box}
main{width:min(820px,100%);border:1px solid #367f5d;padding:24px;background:#03130d}
h1{font-size:20px;font-weight:400;letter-spacing:.08em}pre{white-space:pre-wrap;color:#55e59c}
p{color:#8bbda2}button{padding:10px 16px;border:1px solid #8dffc4;background:#010806;color:#8dffc4;cursor:pointer}
</style></head><body><main><h1>${escape(title)}</h1><pre>${escape(details)}</pre>
<p>Electron log: ${escape(electronLogPath())}</p>
<button id="retry">RETRY</button></main>
<script>document.getElementById('retry').addEventListener('click',()=>{location.href=${retryScript}})</script>
</body></html>`;
}

async function showRendererError(title, details) {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  const page = `data:text/html;charset=utf-8,${encodeURIComponent(
    errorPage(title, details, frontendUrlForRetry),
  )}`;
  await mainWindow.loadURL(page);
  mainWindow.show();
  mainWindow.focus();
}

async function waitForLauncherRenderer(timeoutMs = 15_000) {
  const deadline = Date.now() + timeoutMs;
  let lastState = null;
  while (Date.now() < deadline) {
    if (!mainWindow || mainWindow.isDestroyed()) {
      throw new Error('The launcher window was closed during frontend initialization.');
    }
    try {
      lastState = await mainWindow.webContents.executeJavaScript(`({
        readyState: document.readyState,
        launcherMounted: document.querySelector('app-sevastopol-terminal') !== null,
        appRootChildren: document.querySelector('app-root')?.childElementCount ?? 0,
        bodyText: document.body?.innerText?.slice(0, 300) ?? ''
      })`);
      if (lastState.launcherMounted) {
        logElectron('Angular launcher renderer mounted successfully.');
        return;
      }
    } catch (error) {
      lastState = { evaluationError: error.message };
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  throw new Error(
    `Angular did not mount the /sm-link launcher within ${timeoutMs} ms. ` +
      `Last renderer state: ${JSON.stringify(lastState)}`,
  );
}

async function createWindow(frontendUrl) {
  const frontendOrigin = new URL(frontendUrl).origin;
  frontendUrlForRetry = frontendUrl;
  logElectron(`Creating launcher window for ${frontendUrl}`);
  mainWindow = new BrowserWindow({
    width: 1360,
    height: 860,
    minWidth: 960,
    minHeight: 640,
    show: false,
    frame: false,
    autoHideMenuBar: true,
    backgroundColor: '#010a07',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      devTools: false,
      // "persist:" keeps the frontend's localStorage/cookies (onboarding,
      // preferences) on disk; a bare partition name is in-memory only.
      partition: 'persist:aimp-launcher',
    },
  });

  apiSecurity.install(mainWindow, frontendOrigin);
  mainWindow.once('ready-to-show', () => {
    logElectron('Electron window is ready to display.');
  });
  mainWindow.webContents.on('did-finish-load', () => {
    logElectron(`Renderer loaded: ${mainWindow?.webContents.getURL() || 'unknown URL'}`);
  });
  mainWindow.webContents.on(
    'did-fail-load',
    (_event, errorCode, errorDescription, validatedURL, isMainFrame) => {
      logElectron(
        `Renderer load failure: code=${errorCode} description=${errorDescription} ` +
          `mainFrame=${isMainFrame} url=${validatedURL}`,
      );
    },
  );
  mainWindow.webContents.on('preload-error', (_event, preloadPath, error) => {
    logElectron(`Preload failure (${preloadPath}): ${error.stack || error.message}`);
  });
  mainWindow.webContents.on('render-process-gone', (_event, details) => {
    const message = `Renderer process terminated: reason=${details.reason}, exitCode=${details.exitCode}`;
    logElectron(message);
    void showRendererError('AI-MP RENDERER STOPPED', message);
  });
  mainWindow.webContents.on('console-message', (_event, ...args) => {
    const details = args.length === 1 && typeof args[0] === 'object' ? args[0] : null;
    const level = details?.level ?? args[0] ?? 'log';
    const message = details?.message ?? args[1] ?? '';
    const source = details?.sourceId ?? args[3] ?? 'renderer';
    const line = details?.lineNumber ?? args[2] ?? 0;
    logElectron(`Renderer console [${level}] ${message} (${source}:${line})`);
  });
  mainWindow.on('unresponsive', () => logElectron('Launcher window became unresponsive.'));
  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    const target = new URL(url);
    if (['http:', 'https:'].includes(target.protocol)) void shell.openExternal(url);
    return { action: 'deny' };
  });
  const guardNavigation = (event, url) => {
    const target = new URL(url);
    if (target.origin !== frontendOrigin) {
      event.preventDefault();
      if (['http:', 'https:'].includes(target.protocol)) void shell.openExternal(url);
    }
  };
  mainWindow.webContents.on('will-navigate', guardNavigation);
  mainWindow.webContents.on('will-redirect', guardNavigation);
  mainWindow.on('closed', () => {
    mainWindow = null;
  });
  try {
    await mainWindow.loadURL(frontendUrl);
    await waitForLauncherRenderer();
    mainWindow.show();
    mainWindow.focus();
  } catch (error) {
    const details = `Unable to load ${frontendUrl}\n\n${error.stack || error.message}`;
    logElectron(details);
    await showRendererError('AI-MP FRONTEND UNAVAILABLE', details);
  }
}

function focusRunningLauncher() {
  // Notify slot 1 the way a plain second launch always did.
  app.releaseSingleInstanceLock();
  app.setPath('userData', userDataForSlot(1));
  app.requestSingleInstanceLock({ focus: true });
}

// Split-screen: a new launcher starts with the default launcher's frontend
// storage (onboarding, preferences). Locked or cache files are skipped.
function seedInstanceUserData() {
  const partition = path.join('Partitions', 'aimp-launcher');
  const source = path.join(DEFAULT_USER_DATA, partition);
  const target = path.join(app.getPath('userData'), partition);
  if (instanceSlot <= 1 || fs.existsSync(target) || !fs.existsSync(source)) return;
  const skipped = new Set(['LOCK', 'lockfile', 'Cache', 'Code Cache', 'GPUCache', 'DawnCache', 'DawnGraphiteCache', 'DawnWebGPUCache']);
  const copy = (from, to) => {
    fs.mkdirSync(to, { recursive: true });
    for (const entry of fs.readdirSync(from, { withFileTypes: true })) {
      if (skipped.has(entry.name)) continue;
      try {
        if (entry.isDirectory()) copy(path.join(from, entry.name), path.join(to, entry.name));
        else if (entry.isFile()) fs.copyFileSync(path.join(from, entry.name), path.join(to, entry.name));
      } catch (error) {
        logElectron(`Instance ${instanceSlot}: could not copy ${entry.name} from the default launcher: ${error.message}`);
      }
    }
  };
  try {
    copy(source, target);
  } catch (error) {
    logElectron(`Instance ${instanceSlot}: frontend storage was not copied: ${error.message}`);
  }
}

// The launcher API registers each running instance (LauncherInstances.cs).
function otherInstancesRunning() {
  const directory = path.join(process.env.LOCALAPPDATA || '', 'AIMP', 'Instances');
  for (let slot = 2; slot <= MAX_INSTANCES; slot += 1) {
    try {
      const entry = JSON.parse(fs.readFileSync(path.join(directory, `slot-${slot}.json`), 'utf8'));
      process.kill(entry.LauncherProcessId, 0);
      return true;
    } catch {
      // Missing entry or exited launcher.
    }
  }
  return false;
}

// True when this launcher should keep running.
async function confirmAdditionalInstance() {
  if (instanceSlot === 1) return true;
  if (instanceSlot === 0) {
    await dialog.showMessageBox({
      type: 'info',
      title: 'AI-MP Launcher',
      message: `${MAX_INSTANCES} AI-MP Launchers are already running.`,
      detail: 'Close one of them to start another game.',
      buttons: ['OK'],
    });
    focusRunningLauncher();
    return false;
  }
  if (process.argv.some((argument) => argument.toLowerCase() === NEW_INSTANCE_ARGUMENT)) return true;
  const { response } = await dialog.showMessageBox({
    type: 'question',
    title: 'AI-MP Launcher',
    message: 'AI-MP Launcher is already running.',
    detail: 'Open another launcher to play split-screen? Each launcher starts and controls its own game. '
      + 'Join the first player\'s lobby by direct IP (127.0.0.1).',
    buttons: ['Open another launcher', 'Show the running launcher'],
    defaultId: 0,
    cancelId: 1,
    noLink: true,
  });
  if (response === 0) return true;
  focusRunningLauncher();
  return false;
}

if (instanceSlot > 1) seedInstanceUserData();

app.on('second-instance', (_event, _argv, _workingDirectory, additionalData) => {
  // Another launcher probing for a free slot must not steal the focus.
  if (additionalData?.probe) return;
  if (mainWindow) {
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.show();
    mainWindow.focus();
  }
});

app.whenReady().then(async () => {
  try {
    if (!(await confirmAdditionalInstance())) {
      shutdownComplete = true;
      app.quit();
      return;
    }
    logElectron(`Desktop launcher starting: version=${app.getVersion()}; instance=${instanceSlot}; `
      + `api=${API_URL}; executable=${process.execPath}`);
    // Updating replaces files the other running launchers use.
    const updateOutcome = instanceSlot === 1 && !otherInstancesRunning()
      ? await runStartupUpdate({ app, BrowserWindow, log: logElectron })
      : 'continue';
    if (updateOutcome === 'continue' && instanceSlot !== 1) logElectron('Automatic update skipped in an additional launcher.');
    if (updateOutcome !== 'continue') return;
    const config = loadLauncherConfiguration();
    startApi(config.frontendOrigin);
    await waitForApi();
    await createWindow(config.frontendUrl);
  } catch (error) {
    dialog.showErrorBox(
      'Unable to start AI-MP Launcher',
      `${error.stack || error.message}\n\nAPI log: ${apiLogPath()}`,
    );
    quitting = true;
    await stopApi();
    shutdownComplete = true;
    app.quit();
  } finally {
    startupInProgress = false;
  }
});

ipcMain.on('aimp:api-base-url', (event) => {
  event.returnValue = API_URL;
});

ipcMain.on('window:minimize', () => mainWindow?.minimize());
ipcMain.on('window:toggle-maximize', () => {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  if (mainWindow.isMaximized()) {
    mainWindow.unmaximize();
  } else {
    mainWindow.maximize();
  }
});
ipcMain.on('window:close', () => mainWindow?.close());
ipcMain.on('renderer:log', (_event, message) => logElectron(`Renderer error: ${message}`));

app.on('before-quit', (event) => {
  if (shutdownComplete || !apiProcess) return;
  event.preventDefault();
  if (quitting) return;
  quitting = true;
  void stopApi().finally(() => {
    shutdownComplete = true;
    app.quit();
  });
});

app.on('window-all-closed', () => {
  if (!startupInProgress) app.quit();
});
