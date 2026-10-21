// Execute the menu's AS3 method bodies with display/socket doubles (no game or build).
// Type annotations are erased; this does not replace GFx runtime validation.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
const base = process.argv[2] ? pathToFileURL(resolve(process.argv[2]) + '/')
  : new URL('../AIMP.Launcher/src/AIMP.Launcher/Resources/UI/Source/NEWFRONTENDMENU/frontend/', import.meta.url);
const menuSource = readFileSync(new URL('NewFrontendMenu.as', base), 'utf8');
const bridgeSource = readFileSync(new URL('MultiplayerBridge.as', base), 'utf8');
function method(source, name) {
  const signature = new RegExp(`function ${name}\\(([^)]*)\\)`).exec(source);
  assert.ok(signature, `Missing ${name}`);
  const start = source.indexOf('{', signature.index + signature[0].length);
  let depth = 1, end = start + 1, quote = '';
  for (; depth; end++) {
    const c = source[end];
    if (quote) { if (c === '\\') end++; else if (c === quote) quote = ''; }
    else if (c === '"' || c === "'") quote = c;
    else if (c === '{') depth++;
    else if (c === '}') depth--;
    assert.ok(end < source.length, `Unclosed ${name}`);
  }
  const args = signature[1].split(',').filter(Boolean).map(s => s.trim().split(':')[0]);
  const body = source.slice(start + 1, end - 1)
    .replace(/(\bvar\s+\w+|catch\(\w+):[\w.*]+/g, '$1')
    .replace('super.onPress()', 'this.superPress()');
  return new Function(...args, body);
}
globalThis.NewFrontendMenuState = { MENU: { equals: s => s === 'menu' } };
globalThis.MenuPageId = { ROOT: { value: 0 } };
globalThis.PROTOCOL_VERSION = '1';
globalThis.int = value => Number(value) | 0;
let now = 100;
globalThis.getTimer = () => now;
globalThis.trim = method(bridgeSource, 'trim');
globalThis.decodeValue = method(bridgeSource, 'decodeValue');
globalThis.encodeValue = method(bridgeSource, 'encodeValue');
function newMenu() {
  const state = { _mpLauncherReady: false, _mpPendingCommand: '', _mpLobbyRole: '', _mpCanStart: false, _mpLocalReady: false, _state: 'menu' };
  for (const name of menuSource.matchAll(/private var (_\w+Item):/g)) state[name[1]] = {};
  for (const name of ['mpUpdateControls', 'mpBeginCommand', 'mpReadinessHandler', 'mpBridgeStatusHandler',
    'mpBridgeErrorHandler', 'mpBridgeLobbyStateHandler', 'mpBridgeLobbyOpenedHandler', 'mpSessionClosedHandler',
    'joinDirectIpHandler', 'startServerHandler', 'mpResumeLobbyHandler']) state[name] = method(menuSource, name);
  state.notifications = [];
  state.pages = [];
  state.requests = [];
  state.refreshNetworkMenuRenderers = () => {};
  state.show_network_notification = text => state.notifications.push(text);
  state.mpOpenLobbyPage = role => state.pages.push(role);
  state.mpBridgeLobbyReturnHandler = role => state.pages.push(role);
  state.mpStopLobbyReturn = () => {};
  state.menu = { setCurrentPage: id => state.pages.push(id) };
  state.mpGetDirectJoinAddressLabel = () => '127.0.0.1:28015';
  state.getSelectedSurvivorMapId = () => 'ChallengeMap16';
  state.getSelectedSurvivorMapName = () => 'The Basement';
  state._mpBridge = { joinDirectIp: () => state.requests.push('join'), createLobby: () => state.requests.push('create') };
  return state;
}
function snapshot(role, canStart = '0', localReady = '0') {
  return { role, status: 'Waiting for players', p1: 'Host', p2: 'Client', p3: '', p4: '', canStart, localReady, hasRemotePlayer: '1' };
}
const menu = newMenu();
menu.mpUpdateControls();
assert.equal(menu._multiplayerItem.enabled, false);
menu.mpBridgeStatusHandler('Bridge connected');
assert.equal(menu._multiplayerItem.enabled, false, 'TCP connection is not Launcher Ready');
menu.joinDirectIpHandler(null);
assert.deepEqual(menu.requests, []);
menu.mpReadinessHandler(true);
assert.equal(menu._multiplayerItem.enabled, true);
menu.joinDirectIpHandler(null);
menu.joinDirectIpHandler(null);
assert.deepEqual(menu.requests, ['join'], 'No duplicate connection attempts');
assert.deepEqual(menu.pages, [], 'No navigation before successful join');
menu.mpBridgeErrorHandler('Connection refused');
assert.deepEqual(menu.pages, [], 'A failed join stays on the original page');
assert.equal(menu._directIpJoinItem.enabled, true);
menu.joinDirectIpHandler(null);
menu.mpBridgeLobbyStateHandler(snapshot('client'));
assert.deepEqual(menu.pages, [], 'An early snapshot is not a completed connection');
menu.mpBridgeLobbyOpenedHandler('client');
assert.deepEqual(menu.pages, ['client']);
assert.equal(menu._clientLobbyReadyItem.selected, false, 'Ready reflects the server');
assert.equal(menu._directIpJoinItem.enabled, false, 'Cannot join another lobby while connected');
menu.mpBridgeLobbyStateHandler(snapshot('client', '0', '1'));
assert.deepEqual(menu.pages, ['client'], 'Routine lobby updates do not navigate');
assert.equal(menu._clientLobbyReadyItem.selected, true);
menu._clientLobbyReadyItem.selected = false;
menu.mpBridgeErrorHandler('Ready update rejected');
assert.equal(menu._clientLobbyReadyItem.selected, true, 'Rejected Ready changes restore the authoritative value');
menu.mpSessionClosedHandler('Host disconnected');
assert.deepEqual(menu.pages, ['client', 0]);
assert.equal(menu._mpResumeLobbyItem.visible, false);
assert.equal(menu._directIpJoinItem.enabled, true);
const host = newMenu();
host.mpReadinessHandler(true);
host.startServerHandler(null);
assert.deepEqual(host.pages, []);
host.mpBridgeErrorHandler('Port in use');
assert.equal(host._serverStartItem.enabled, true);
host.startServerHandler(null);
host.mpBridgeLobbyStateHandler(snapshot('host'));
assert.deepEqual(host.pages, []);
host.mpBridgeLobbyOpenedHandler('host');
assert.deepEqual(host.pages, ['host']);
assert.equal(host._hostLobbyStartGameItem.enabled, false);
host.mpBridgeLobbyStateHandler(snapshot('host', '1'));
assert.equal(host._hostLobbyStartGameItem.enabled, true);
host.mpReadinessHandler(false);
assert.equal(host._hostLobbyStartGameItem.enabled, false);
assert.equal(host._multiplayerItem.enabled, false);
const failedHost = newMenu();
failedHost.mpReadinessHandler(true);
failedHost.startServerHandler(null);
failedHost.mpBridgeLobbyStateHandler(snapshot('host'));
failedHost.mpSessionClosedHandler('Unable to create lobby');
failedHost.mpBridgeErrorHandler('Native setup failed');
assert.deepEqual(failedHost.pages, [], 'Failure after an early snapshot preserves the create page');
assert.equal(failedHost._serverStartItem.enabled, true);
const bridge = { _launcherReady: false, _lastReady: 0, _active: true, _nextPoll: 0, _connected: true,
  setReadyState: method(bridgeSource, 'setReadyState'), handleLine: method(bridgeSource, 'handleLine'),
  update: method(bridgeSource, 'update'), send: () => {}, invokeStatus: () => {}, invokeError: () => {} };
bridge.onReadiness = ready => menu.mpReadinessHandler(ready);
bridge.handleLine('STATUS|message=Launcher ready');
assert.equal(bridge._launcherReady, false, 'Status text never grants access');
bridge.handleLine('FRONTEND_STATE|ready=1');
assert.equal(bridge._launcherReady, true);
now += 10001;
bridge.update();
assert.equal(bridge._launcherReady, false, 'Missing launcher heartbeat revokes access');
bridge.handleLine('FRONTEND_STATE|ready=1');
assert.equal(bridge._launcherReady, true, 'Late initialization/recovery restores access');
bridge._active = false;
let sends = 0;
bridge.send = () => sends++;
bridge.update();
assert.equal(sends, 0, 'An unloaded movie must not compete with the gameplay bridge');
const disconnected = { _connected: false, _socket: null, errors: [], invokeError(text) { this.errors.push(text); } };
method(bridgeSource, 'send').call(disconnected, 'JOIN_DIRECT_IP', {});
assert.equal(disconnected.errors.length, 1, 'Disconnected actions are rejected instead of replayed later');
const expandable = readFileSync(new URL('menu/MenuItemExpandable.as', base), 'utf8');
const item = { autoExpand: false, _subPage: {}, superPress: () => {}, onPress: method(expandable, 'onPress') };
let expansions = 0;
globalThis._menu = { expand: () => expansions++ };
item.onPress();
assert.equal(expansions, 0);
item.autoExpand = true;
item.onPress();
assert.equal(expansions, 1, 'Normal expandable menu entries retain their behavior');
assert.ok(!menuSource.includes('_loc1_.addItem(this._playGameItem)'));
console.log(`Menu behavior checks passed (${fileURLToPath(base)}).`);

// Developer options stay hidden by default and reset campaign choices when disabled.
const dev = {
  _developerMode: false, _campaignLevelIds: ['M01_TEST'],
  _survivorMapIds: ['ChallengeMap16'], _survivorMapLabels: ['The Basement'],
  _survivorMapDescriptions: ['Survival'],
  _serverModeItem: { selectedItem: 'survival' },
  _serverMapItem: { setItems(ids) { this.ids = ids; } },
  refreshNetworkMenuRenderers() {},
  updateServerMapVisibility: method(menuSource, 'updateServerMapVisibility'),
  refreshSurvivorMapOptions: method(menuSource, 'refreshSurvivorMapOptions')
};
dev.refreshSurvivorMapOptions();
assert.equal(dev._serverModeItem.visible, false);
assert.equal(dev._serverMapItem.visible, false);
dev._developerMode = true;
dev._serverModeItem.selectedItem = 'campaign';
dev.refreshSurvivorMapOptions();
assert.equal(dev._serverModeItem.visible, true);
assert.equal(dev._serverMapItem.visible, true);
assert.deepEqual(dev._serverMapItem.ids, ['M01_TEST']);
dev._serverMapItem.selectedItem = 'M01_TEST';
assert.equal(method(menuSource, 'getSelectedSurvivorMapId').call(dev), 'M01_TEST');
dev._developerMode = false;
dev._serverModeItem.selectedItem = 'survival';
dev.refreshSurvivorMapOptions();
assert.equal(dev._serverMapItem.visible, false);
assert.deepEqual(dev._serverMapItem.ids, ['ChallengeMap16']);
console.log('Developer mode menu checks passed.');
