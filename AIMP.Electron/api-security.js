const { randomBytes } = require('node:crypto');

const TOKEN_HEADER = 'X-AIMP-Token';

function withoutToken(headers = {}) {
  return Object.fromEntries(Object.entries(headers)
    .filter(([name]) => name.toLowerCase() !== TOKEN_HEADER.toLowerCase()));
}

function createApiSecurity(apiUrl) {
  const apiOrigin = new URL(apiUrl).origin;
  const token = randomBytes(32).toString('hex');

  function isApiUrl(url) {
    try {
      const target = new URL(url);
      return target.origin === apiOrigin && !target.username && !target.password;
    } catch {
      return false;
    }
  }

  function install(window, frontendOrigin) {
    const contents = window.webContents;
    contents.session.webRequest.onBeforeSendHeaders({ urls: ['<all_urls>'] }, (details, callback) => {
      // Strip the token on every hop, including redirects away from the API.
      const requestHeaders = withoutToken(details.requestHeaders);
      if (!isApiUrl(details.url)) return callback({ requestHeaders });
      const origin = Object.entries(requestHeaders).find(([name]) => name.toLowerCase() === 'origin')?.[1];
      const preflight = details.method === 'OPTIONS' && Object.keys(requestHeaders)
        .some((name) => name.toLowerCase() === 'access-control-request-method');
      if (preflight && origin === frontendOrigin) return callback({ requestHeaders });
      try {
        if (contents.isDestroyed() || details.webContentsId !== contents.id
            || details.frame !== contents.mainFrame || details.frame.origin !== frontendOrigin
            || origin !== frontendOrigin || details.resourceType !== 'xhr') {
          return callback({ cancel: true });
        }
      } catch {
        return callback({ cancel: true });
      }
      requestHeaders[TOKEN_HEADER] = token;
      callback({ requestHeaders });
    });
  }

  function nativeRequestOptions(url, options = {}) {
    if (!isApiUrl(url)) throw new Error('Native API requests must target the local API.');
    return { ...options, headers: { ...withoutToken(options.headers), [TOKEN_HEADER]: token } };
  }

  return { token, install, nativeRequestOptions };
}

module.exports = { createApiSecurity, TOKEN_HEADER };
