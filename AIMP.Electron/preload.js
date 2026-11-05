const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('AIMP_API_BASE_URL', 'http://127.0.0.1:5127');
contextBridge.exposeInMainWorld('aimpWindow', {
  minimize: () => ipcRenderer.send('window:minimize'),
  maximize: () => ipcRenderer.send('window:toggle-maximize'),
  close: () => ipcRenderer.send('window:close'),
});

window.addEventListener('error', (event) => {
  ipcRenderer.send(
    'renderer:log',
    `${event.message} at ${event.filename || 'unknown'}:${event.lineno || 0}:${event.colno || 0}`,
  );
});

window.addEventListener('unhandledrejection', (event) => {
  const reason = event.reason instanceof Error ? event.reason.stack : String(event.reason);
  ipcRenderer.send('renderer:log', `Unhandled promise rejection: ${reason}`);
});
