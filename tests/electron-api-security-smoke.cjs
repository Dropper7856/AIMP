// Run with AIMP.Electron/node_modules/electron/dist/electron.exe.
// Hidden windows and ephemeral HTTP servers exercise Electron's actual request hooks.
const assert = require('node:assert/strict');
const http = require('node:http');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const { createApiSecurity, TOKEN_HEADER } = require(path.join(
  process.env.AIMP_TEST_ELECTRON_DIR || path.join(__dirname, '..', 'AIMP.Electron'), 'api-security.js'));

const servers = [];
async function listen(handler) {
  const server = http.createServer(handler);
  servers.push(server);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return `http://127.0.0.1:${server.address().port}`;
}

app.disableHardwareAcceleration();
app.on('window-all-closed', () => {});
app.whenReady().then(async () => {
  let window;
  let attacker;
  try {
    let redirectRequests = 0;
    let preflights = 0;
    let frontendOrigin;
    let security;
    const externalOrigin = await listen((request, response) => {
      redirectRequests++;
      assert.equal(request.headers[TOKEN_HEADER.toLowerCase()], undefined, 'Token leaked on redirect.');
      response.setHeader('Access-Control-Allow-Origin', '*');
      response.end('external');
    });
    const apiOrigin = await listen((request, response) => {
      response.setHeader('Access-Control-Allow-Origin', frontendOrigin);
      response.setHeader('Access-Control-Allow-Methods', 'GET, PUT');
      response.setHeader('Access-Control-Allow-Headers', 'Content-Type');
      response.setHeader('Access-Control-Allow-Private-Network', 'true');
      if (request.method === 'OPTIONS') {
        preflights++;
        assert.equal(request.headers[TOKEN_HEADER.toLowerCase()], undefined, 'Preflight must not expose the token.');
        response.writeHead(204).end();
        return;
      }
      const authenticated = request.headers[TOKEN_HEADER.toLowerCase()] === security.token;
      if (!authenticated) { response.writeHead(401).end(); return; }
      if (request.url === '/redirect') {
        response.writeHead(302, { Location: externalOrigin + '/receive' }).end();
        return;
      }
      response.setHeader('Content-Type', 'application/json');
      response.end(JSON.stringify({ authenticated }));
    });
    frontendOrigin = await listen((_request, response) => {
      response.setHeader('Content-Type', 'text/html');
      response.end('<!doctype html><title>API security smoke test</title>');
    });
    security = createApiSecurity(apiOrigin);
    const webPreferences = { contextIsolation: true, nodeIntegration: false, sandbox: true,
      partition: 'aimp-security-smoke' };
    window = new BrowserWindow({ show: false, webPreferences });
    security.install(window, frontendOrigin);
    await window.loadURL(frontendOrigin);
    const result = await window.webContents.executeJavaScript(`(async () => {
      const api = ${JSON.stringify(apiOrigin)};
      const get = await (await fetch(api + '/read')).json();
      const put = await (await fetch(api + '/write', {
        method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: '{}'
      })).json();
      const xhr = await new Promise((resolve, reject) => {
        const request = new XMLHttpRequest();
        request.open('GET', api + '/xhr');
        request.onload = () => resolve(JSON.parse(request.responseText));
        request.onerror = reject;
        request.send();
      });
      const frames = [];
      for (const sandboxed of [true, false]) {
        frames.push(await new Promise((resolve, reject) => {
          const frame = document.createElement('iframe');
          if (sandboxed) frame.sandbox = 'allow-scripts';
          const timeout = setTimeout(() => reject(new Error('iframe timeout')), 5000);
          function message(event) {
            if (event.source !== frame.contentWindow) return;
            clearTimeout(timeout); window.removeEventListener('message', message);
            frame.remove(); resolve(event.data);
          }
          window.addEventListener('message', message);
          frame.srcdoc = '<script>fetch(' + JSON.stringify(api + '/iframe')
            + ').then(() => parent.postMessage("allowed", "*"))'
            + '.catch(() => parent.postMessage("denied", "*"));<' + '/script>';
          document.body.appendChild(frame);
        }));
      }
      try { await fetch(api + '/redirect'); } catch {}
      return { get, put, xhr, frames };
    })()`);
    assert.equal(result.get.authenticated, true);
    assert.equal(result.put.authenticated, true);
    assert.equal(result.xhr.authenticated, true);
    assert.deepEqual(result.frames, ['denied', 'denied']);
    assert.ok(preflights >= 1, 'JSON PUT must exercise CORS preflight.');
    assert.equal(redirectRequests, 1, 'Redirect destination was not exercised.');
    attacker = new BrowserWindow({ show: false, webPreferences });
    await attacker.loadURL(externalOrigin);
    const blocked = await attacker.webContents.executeJavaScript(
      `fetch(${JSON.stringify(apiOrigin + '/other-window')}).then(() => false).catch(() => true)`);
    assert.equal(blocked, true);
    console.log('PASS Electron runtime: fetch, XHR and JSON PUT authenticated; preflight token-free; sandbox/iframe/other-window blocked; redirect token stripped.');
    app.exit(0);
  } catch (error) {
    console.error(error);
    app.exit(1);
  } finally {
    attacker?.destroy();
    window?.destroy();
    for (const server of servers) server.close();
  }
});
