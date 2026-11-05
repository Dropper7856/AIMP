const fs = require('node:fs');
const path = require('node:path');
const { execFile } = require('node:child_process');

function hasRunningGame() {
  const tasklist = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tasklist.exe');
  return new Promise((resolve, reject) => {
    execFile(tasklist, ['/FI', 'IMAGENAME eq AI.exe', '/FO', 'CSV', '/NH'],
      { windowsHide: true, timeout: 2000 }, (error, stdout) => {
        if (error) reject(error);
        else resolve(/^"AI\.exe",/im.test(stdout));
      });
  });
}

function uninstallerPathFor(executablePath) {
  // NSIS uses electron-builder's productFilename, which follows the custom
  // executableName. The displayed product name can be different.
  return path.join(path.dirname(executablePath), `Uninstall ${path.basename(executablePath)}`);
}

function canAutoUpdate({ app, env, platform, executablePath, exists }) {
  return app.isPackaged && platform === 'win32'
    && !env.PORTABLE_EXECUTABLE_FILE && !env.PORTABLE_EXECUTABLE_DIR
    && exists(uninstallerPathFor(executablePath));
}

function withTimeout(operation, milliseconds, onTimeout = () => {}) {
  let timer;
  return Promise.race([operation, new Promise((_, reject) => {
    timer = setTimeout(() => {
      onTimeout();
      reject(new Error('Update operation timed out.'));
    }, milliseconds);
  })]).finally(() => clearTimeout(timer));
}

function createUpdateWindow(BrowserWindow, onCancel) {
  let closing = false;
  const window = new BrowserWindow({
    width: 580, height: 340, resizable: false, maximizable: false, show: false,
    title: 'AI-MP Launcher', autoHideMenuBar: true, backgroundColor: '#010a07',
    webPreferences: { contextIsolation: true, nodeIntegration: false, sandbox: true,
      partition: 'aimp-updater' },
  });
  window.on('closed', () => { if (!closing) onCancel(); });
  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  window.webContents.on('will-navigate', (event) => event.preventDefault());
  return {
    ready: window.loadFile(path.join(__dirname, 'update.html')).then(() => window.show()),
    update(title, message, percent = null) {
      if (window.isDestroyed()) return;
      window.setProgressBar(percent === null ? 2 : percent / 100);
      const state = JSON.stringify({ title, message, percent });
      void window.webContents.executeJavaScript(`(() => {
        const state = ${state};
        document.getElementById('status').textContent = state.title;
        document.getElementById('message').textContent = state.message;
        const progress = document.getElementById('progress');
        if (state.percent === null) progress.removeAttribute('value');
        else progress.value = state.percent;
      })()`).catch(() => {});
    },
    close() { closing = true; if (!window.isDestroyed()) window.destroy(); },
  };
}

async function runStartupUpdate({ app, BrowserWindow, log, env = process.env,
  platform = process.platform, executablePath = process.execPath, exists = fs.existsSync,
  isGameRunning = hasRunningGame,
  getUpdater = () => require('electron-updater').autoUpdater,
  createView = (cancel) => createUpdateWindow(BrowserWindow, cancel),
  checkTimeoutMs = 10000, downloadTimeoutMs = 5 * 60 * 1000 }) {
  if (!canAutoUpdate({ app, env, platform, executablePath, exists })) {
    log(`Automatic update skipped: packaged=${app.isPackaged}; platform=${platform}; `
      + `portable=${Boolean(env.PORTABLE_EXECUTABLE_FILE || env.PORTABLE_EXECUTABLE_DIR)}; `
      + `expected uninstaller=${uninstallerPathFor(executablePath)}`);
    return 'continue';
  }
  try {
    if (await isGameRunning()) {
      log('Automatic update deferred while Alien: Isolation is running.');
      return 'continue';
    }
  } catch (error) {
    log('Automatic update deferred: could not check whether the game is running.');
    return 'continue';
  }
  let view;
  let updater;
  let token;
  let aborted = false;
  let outcome = 'continue';
  let lastError;
  const cancel = () => { aborted = true; token?.cancel(); };
  const onProgress = (progress) => {
    if (!aborted && Number.isFinite(progress.percent)) {
      const percent = Math.max(0, Math.min(100, Math.floor(progress.percent)));
      view?.update('DOWNLOADING UPDATE', `${percent}% — The launcher will restart automatically.`, percent);
    }
  };
  app.once('before-quit', cancel);
  try {
    updater = getUpdater();
    updater.autoDownload = false;
    updater.autoInstallOnAppQuit = false;
    updater.autoRunAppAfterInstall = true;
    updater.allowPrerelease = false;
    updater.allowDowngrade = false;
    updater.disableWebInstaller = true;
    const writeLog = (...args) => log('[Updater] ' + args.map((arg) => String(arg)).join(' '));
    updater.logger = { info: writeLog, warn: writeLog, error: writeLog, debug: writeLog };
    // Keep an error listener even after a timed-out check settles in the
    // background. Late events must not trigger a download or crash the app.
    updater.on('error', (error) => { lastError = error; writeLog(error.message || error); });
    updater.on('download-progress', onProgress);
    view = createView(() => { cancel(); app.quit(); });
    await view.ready;
    if (aborted) return 'quit';
    view.update('CHECKING FOR UPDATES', 'Checking for the latest AI-MP Launcher version.');
    const result = await withTimeout(updater.checkForUpdates(), checkTimeoutMs);
    if (aborted) return 'quit';
    if (!result?.isUpdateAvailable) return 'continue';
    token = result.cancellationToken;
    view.update('UPDATE AVAILABLE', `Downloading version ${result.updateInfo.version}.`);
    const files = await withTimeout(updater.downloadUpdate(token), downloadTimeoutMs, () => token?.cancel());
    if (aborted) return 'quit';
    if (!files?.length) throw new Error('The update did not produce an installer.');
    if (await isGameRunning()) {
      log('Update downloaded; installation deferred because the game was started.');
      return 'continue';
    }
    if (aborted) return 'quit';
    view.update('INSTALLING UPDATE', 'AI-MP Launcher will restart in a moment.', 100);
    lastError = null;
    // This runs before the API or game bridge has started, so the installer
    // can replace the complete package without locking the backend binaries.
    updater.quitAndInstall(true, true);
    if (lastError) throw lastError;
    outcome = 'installing';
    return outcome;
  } catch (error) {
    log('Update unavailable; keeping the current version: ' + (error.message || error));
    return aborted ? 'quit' : 'continue';
  } finally {
    app.removeListener('before-quit', cancel);
    updater?.removeListener('download-progress', onProgress);
    if (outcome !== 'installing') view?.close();
  }
}

module.exports = { runStartupUpdate, canAutoUpdate, createUpdateWindow };
