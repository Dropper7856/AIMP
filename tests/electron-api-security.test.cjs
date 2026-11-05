const assert = require('node:assert/strict');
const path = require('node:path');
const { test } = require('node:test');

const electronDir = process.env.AIMP_TEST_ELECTRON_DIR || path.join(__dirname, '..', 'AIMP.Electron');
const { createApiSecurity, TOKEN_HEADER } = require(path.join(electronDir, 'api-security.js'));
const apiUrl = 'http://127.0.0.1:5127';
const frontendOrigin = 'https://aimp.dblabs.ca';

function installed() {
  const security = createApiSecurity(apiUrl);
  let intercept;
  const mainFrame = { origin: frontendOrigin };
  const contents = {
    id: 17, mainFrame, isDestroyed: () => false,
    session: { webRequest: { onBeforeSendHeaders(filter, listener) {
      assert.deepEqual(filter.urls, ['<all_urls>']);
      intercept = listener;
    } } },
  };
  security.install({ webContents: contents }, frontendOrigin);
  return { security, contents, request(overrides = {}) {
    let result;
    intercept({ url: apiUrl + '/api/v1/filesystem/directory', method: 'GET', resourceType: 'xhr',
      webContentsId: contents.id, frame: mainFrame, requestHeaders: { Origin: frontendOrigin },
      ...overrides }, (response) => { assert.equal(result, undefined); result = response; });
    assert.ok(result);
    return result;
  } };
}

test('each Electron session gets a fresh 256-bit token', () => {
  const first = createApiSecurity(apiUrl).token;
  const second = createApiSecurity(apiUrl).token;
  assert.match(first, /^[a-f0-9]{64}$/);
  assert.notEqual(first, second);
});

test('only the official main-frame API request receives the session token', () => {
  const { security, request } = installed();
  const result = request({ requestHeaders: { origin: frontendOrigin, 'x-aimp-token': 'forged', Accept: 'application/json' } });
  assert.equal(result.requestHeaders[TOKEN_HEADER], security.token);
  assert.equal(result.requestHeaders.Accept, 'application/json');
  assert.equal(Object.keys(result.requestHeaders).filter((name) => name.toLowerCase() === 'x-aimp-token').length, 1);
});

test('iframes, opaque frames, missing frames and other windows cannot borrow authentication', () => {
  const { contents, request } = installed();
  for (const overrides of [
    { frame: null }, { frame: undefined }, { frame: { origin: frontendOrigin } },
    { frame: { origin: 'null' } }, { frame: { origin: 'https://attacker.invalid' } },
    { webContentsId: contents.id + 1 }, { webContentsId: undefined },
    { requestHeaders: { Origin: 'null' } }, { requestHeaders: {} },
    { resourceType: 'mainFrame' }, { resourceType: 'image' }, { resourceType: 'webSocket' },
  ]) assert.equal(request(overrides).cancel, true);
  contents.mainFrame.origin = 'null';
  assert.equal(request().cancel, true);
});

test('destroyed frames fail closed', () => {
  const { contents, request } = installed();
  Object.defineProperty(contents.mainFrame, 'origin', { get() { throw new Error('Frame destroyed'); } });
  assert.equal(request().cancel, true);
});

test('private-network and CORS preflights carry no session token', () => {
  const { request } = installed();
  const result = request({ method: 'OPTIONS', frame: null, requestHeaders: {
    Origin: frontendOrigin, 'Access-Control-Request-Method': 'PUT', 'Access-Control-Request-Private-Network': 'true',
    'X-AIMP-Token': 'forged',
  } });
  assert.equal(result.cancel, undefined);
  assert.equal(result.requestHeaders[TOKEN_HEADER], undefined);
  assert.equal(request({ method: 'OPTIONS', requestHeaders: { Origin: 'null' } }).cancel, true);
});

test('tokens are stripped from external requests and redirect hops', () => {
  const { security, request } = installed();
  const authenticated = request().requestHeaders;
  for (const url of ['https://attacker.invalid/leak', 'http://127.0.0.1:5128/', 'http://localhost:5127/',
    'https://127.0.0.1:5127/', 'http://user:password@127.0.0.1:5127/', 'data:text/html,hello']) {
    const result = request({ url, requestHeaders: { ...authenticated, 'x-aimp-token': security.token } });
    assert.equal(Object.keys(result.requestHeaders).some((name) => name.toLowerCase() === 'x-aimp-token'), false);
  }
});

test('native health and shutdown requests are authenticated without renderer involvement', () => {
  const security = createApiSecurity(apiUrl);
  for (const url of [apiUrl + '/', apiUrl + '/api/v1/application/shutdown']) {
    const options = security.nativeRequestOptions(url, { method: 'POST', headers: { 'x-aimp-token': 'forged', Accept: 'application/json' } });
    assert.equal(options.headers[TOKEN_HEADER], security.token);
    assert.equal(options.headers.Accept, 'application/json');
    assert.equal(options.method, 'POST');
    assert.equal(options.headers['x-aimp-token'], undefined);
  }
  assert.throws(() => security.nativeRequestOptions('https://attacker.invalid/'));
});
