import test from 'node:test';
import assert from 'node:assert/strict';

import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';

import { RelayCore, generateCode, hashCode } from '../src/relay-core.js';
import { createRelayServer } from '../src/server.js';
import { RateLimiter } from '../src/rate-limit.js';
import { IpAllowList, clientIp, normalizeIp } from '../src/net.js';
import { loadConfig } from '../src/config.js';

const CHANNEL = '123456789012345678';

function newCore(options = {}) {
  return new RelayCore(options);
}

test('generateCode avoids look-alike characters and is long enough', () => {
  for (let i = 0; i < 50; i += 1) {
    const code = generateCode();
    assert.equal(code.length, 10);
    assert.match(code, /^[A-HJ-NP-Z2-9]+$/);
  }
  assert.equal(generateCode(16).length, 16);
});

test('a generated code authenticates and is case insensitive', () => {
  const core = newCore();
  const { code, pairing } = core.createPairing({ guildId: 'g', channelId: CHANNEL, label: 'main' });

  assert.equal(pairing.channelId, CHANNEL);
  assert.equal(core.authenticate(code), pairing);
  assert.equal(core.authenticate(code.toLowerCase()), pairing);
  assert.equal(core.authenticate(` ${code} `), pairing);
});

test('wrong, empty and missing codes are rejected', () => {
  const core = newCore();
  core.createPairing({ channelId: CHANNEL });

  assert.equal(core.authenticate('NOPE'), null);
  assert.equal(core.authenticate(''), null);
  assert.equal(core.authenticate(undefined), null);
  assert.equal(core.authenticate(null), null);
});

test('re-pairing a channel rotates the code and kills the old one', () => {
  const core = newCore();
  const first = core.createPairing({ channelId: CHANNEL });
  const second = core.createPairing({ channelId: CHANNEL });

  assert.notEqual(first.code, second.code);
  assert.equal(core.authenticate(first.code), null);
  assert.notEqual(core.authenticate(second.code), null);
  assert.equal(core.pairings.size, 1);
});

test('revoking by code and by channel both work', () => {
  const core = newCore();
  const a = core.createPairing({ channelId: '111' });
  const b = core.createPairing({ channelId: '222' });

  assert.equal(core.revokeByCode(a.code), true);
  assert.equal(core.authenticate(a.code), null);
  assert.equal(core.pairingForChannel('111'), null);

  assert.equal(core.revokeByChannel('222'), true);
  assert.equal(core.authenticate(b.code), null);
  assert.equal(core.revokeByCode('unknown'), false);
});

test('poll returns buffered messages newer than the cursor', async () => {
  const core = newCore();
  const { code } = core.createPairing({ channelId: CHANNEL });

  core.publish(CHANNEL, { name: 'Alice', message: 'one' });
  core.publish(CHANNEL, { name: 'Bob', message: 'two' });

  const first = await core.poll(code, 0, 0);
  assert.equal(first.messages.length, 2);
  assert.deepEqual(first.messages.map((m) => m.message), ['one', 'two']);
  assert.equal(first.cursor, 2);

  const second = await core.poll(code, first.cursor, 0);
  assert.equal(second.messages.length, 0);
  assert.equal(second.cursor, 2);

  core.publish(CHANNEL, { name: 'Carol', message: 'three' });
  const third = await core.poll(code, first.cursor, 0);
  assert.deepEqual(third.messages.map((m) => m.message), ['three']);
});

test('poll long-polls and is woken by a publish', async () => {
  const core = newCore();
  const { code } = core.createPairing({ channelId: CHANNEL });

  const started = Date.now();
  const pending = core.poll(code, 0, 2000);
  setTimeout(() => core.publish(CHANNEL, { name: 'Dave', message: 'wake up' }), 25);

  const result = await pending;
  assert.deepEqual(result.messages.map((m) => m.message), ['wake up']);
  assert.ok(Date.now() - started < 1000, 'should resolve as soon as the message arrives');
});

test('poll resolves empty when the wait window expires', async () => {
  const core = newCore();
  const { code } = core.createPairing({ channelId: CHANNEL });

  const started = Date.now();
  const result = await core.poll(code, 0, 120);
  assert.deepEqual(result.messages, []);
  assert.ok(Date.now() - started >= 100, 'should actually wait');
});

test('poll rejects an unknown code', async () => {
  const core = newCore();
  await assert.rejects(() => core.poll('BOGUS', 0, 0), /invalid pairing code/);
});

test('publish sanitizes text, ignores blanks and is bounded', () => {
  const core = newCore({ maxBuffered: 3 });
  const { code } = core.createPairing({ channelId: CHANNEL });

  core.publish(CHANNEL, { name: 'Alice', message: 'line one\nline two\tend' });
  core.publish(CHANNEL, { name: 'Alice', message: '   ' });
  core.publish(CHANNEL, { name: 'Alice', message: 'x'.repeat(900) });

  return core.poll(code, 0, 0).then((result) => {
    assert.equal(result.messages.length, 2, 'blank message must not be buffered');
    assert.equal(result.messages[0].message, 'line one line two end');
    assert.equal(result.messages[1].message.length, 400, 'long messages are truncated');
  });
});

test('buffer keeps only the newest entries', async () => {
  const core = newCore({ maxBuffered: 3 });
  const { code } = core.createPairing({ channelId: CHANNEL });
  for (let i = 1; i <= 6; i += 1) {
    core.publish(CHANNEL, { name: 'Alice', message: `m${i}` });
  }
  const result = await core.poll(code, 0, 0);
  assert.deepEqual(result.messages.map((m) => m.message), ['m4', 'm5', 'm6']);
});

test('publish to an unpaired channel is ignored', () => {
  const core = newCore();
  assert.equal(core.publish('999', { name: 'X', message: 'y' }), 0);
});

test('pairings survive a save/load round trip without the plaintext code', async () => {
  const core = newCore();
  const { code } = core.createPairing({ guildId: 'g1', channelId: CHANNEL, label: 'bridge' });
  const snapshot = JSON.parse(JSON.stringify(core.toJSON()));

  assert.equal(snapshot.pairings[0].codeHash, hashCode(code));
  assert.equal(JSON.stringify(snapshot).includes(code), false, 'plaintext code must never be stored');

  const restored = newCore();
  assert.equal(restored.loadJSON(snapshot), 1);
  assert.notEqual(restored.authenticate(code), null);
  assert.equal(restored.pairingForChannel(CHANNEL)?.label, 'bridge');
});

// ---- HTTP layer ----

async function withServer(run, { sendImpl, ...options } = {}) {
  const core = newCore();
  const sent = [];
  const server = createRelayServer({
    core,
    sendToDiscord: sendImpl ?? (async (pairing, msg) => {
      sent.push({ channelId: pairing.channelId, ...msg });
    }),
    ...options,
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();
  const base = `http://127.0.0.1:${port}`;
  try {
    await run({ core, base, sent, server });
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
}

test('HTTP: health reports pairing count', async () => {
  await withServer(async ({ core, base }) => {
    core.createPairing({ channelId: CHANNEL });
    const response = await fetch(`${base}/health`);
    assert.equal(response.status, 200);
    assert.deepEqual(await response.json(), { ok: true, pairings: 1, ipLock: 'off' });
  });
});

test('HTTP: send reaches Discord and poll returns it to the game server', async () => {
  await withServer(async ({ core, base, sent }) => {
    const { code } = core.createPairing({ channelId: CHANNEL });

    const sendResponse = await fetch(`${base}/v1/relay/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code, name: 'Player1', message: 'hello from the game' }),
    });
    assert.equal(sendResponse.status, 200);
    assert.deepEqual(sent, [{ channelId: CHANNEL, name: 'Player1', message: 'hello from the game', system: false }]);

    // Discord side replies, then the game polls it back.
    core.publish(CHANNEL, { name: 'DiscordUser', message: 'hello from discord' });
    const pollResponse = await fetch(`${base}/v1/relay/poll`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code, cursor: 0, wait: 1 }),
    });
    assert.equal(pollResponse.status, 200);
    const payload = await pollResponse.json();
    assert.equal(payload.ok, true);
    assert.deepEqual(payload.messages.map((m) => m.message), ['hello from discord']);
  });
});

test('HTTP: unknown code is 401 on both endpoints', async () => {
  await withServer(async ({ base }) => {
    for (const path of ['/v1/relay/send', '/v1/relay/poll']) {
      const response = await fetch(`${base}${path}`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ code: 'NOPE', name: 'x', message: 'y', wait: 0 }),
      });
      assert.equal(response.status, 401, path);
    }
  });
});

test('HTTP: empty message is rejected, Discord failures are reported as 502', async () => {
  await withServer(async ({ core, base }) => {
    const { code } = core.createPairing({ channelId: CHANNEL });
    const empty = await fetch(`${base}/v1/relay/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code, name: 'a', message: '   ' }),
    });
    assert.equal(empty.status, 400);
  });

  await withServer(async ({ core, base }) => {
    const { code } = core.createPairing({ channelId: CHANNEL });
    const failed = await fetch(`${base}/v1/relay/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code, name: 'a', message: 'b' }),
    });
    assert.equal(failed.status, 502);
  }, { sendImpl: async () => { throw new Error('discord down'); } });
});

test('HTTP: malformed body is 400, wrong method is 404', async () => {
  await withServer(async ({ base }) => {
    const bad = await fetch(`${base}/v1/relay/poll`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: '{not json',
    });
    assert.equal(bad.status, 400);

    const wrongMethod = await fetch(`${base}/v1/relay/poll`);
    assert.equal(wrongMethod.status, 404);
  });
});

// ---- IP lock, rate limits and config ----

test('IpAllowList matches exact IPs, CIDR blocks and IPv4-mapped addresses', () => {
  const list = new IpAllowList(['203.0.113.7', '10.0.0.0/8', '2001:db8::/32']);
  assert.equal(list.locked, true);
  assert.equal(list.allows('203.0.113.7'), true);
  assert.equal(list.allows('10.1.2.3'), true);
  assert.equal(list.allows('2001:db8::5'), true);
  assert.equal(list.allows('203.0.113.8'), false);
  assert.equal(list.allows('11.0.0.1'), false);
  assert.equal(list.allows('not-an-ip'), false);
  // A dual-stack listener reports IPv4 clients as ::ffff:1.2.3.4.
  assert.equal(list.allows('::ffff:203.0.113.7'), true);
});

test('an empty allowlist allows everything and reports itself unlocked', () => {
  const list = new IpAllowList([]);
  assert.equal(list.locked, false);
  assert.equal(list.allows('203.0.113.7'), true);
  assert.equal(normalizeIp('::ffff:10.0.0.1'), '10.0.0.1');
  assert.equal(normalizeIp('[2001:db8::1]'), '2001:db8::1');
});

test('a malformed allowedIps entry is rejected instead of silently ignored', () => {
  assert.throws(() => new IpAllowList(['10.0.0.0/64']), /prefix length/);
  assert.throws(() => new IpAllowList(['example.com']), /not an IP address/);
});

test('clientIp only honours forwarding headers when the proxy is trusted', () => {
  const req = {
    headers: { 'x-forwarded-for': '203.0.113.9, 10.0.0.1' },
    socket: { remoteAddress: '::ffff:127.0.0.1' },
  };
  assert.equal(clientIp(req, { trustProxy: false }), '127.0.0.1');
  // Rightmost: the address the nearest proxy appended, not what the client wrote.
  assert.equal(clientIp(req, { trustProxy: true }), '10.0.0.1');

  const realIp = { headers: { 'x-real-ip': '198.51.100.4' }, socket: { remoteAddress: '127.0.0.1' } };
  assert.equal(clientIp(realIp, { trustProxy: true }), '198.51.100.4');
});

test('clientIp only believes forwarding headers from a configured proxy peer', () => {
  const forwarded = (peer, header) => ({
    headers: { 'x-forwarded-for': header },
    socket: { remoteAddress: peer },
  });
  const nginxNextDoor = new IpAllowList(['127.0.0.1']);

  // The proxy we listed appended the real client: trust it.
  assert.equal(
    clientIp(forwarded('127.0.0.1', '203.0.113.7, 198.51.100.9'), { trustedProxies: nginxNextDoor }),
    '198.51.100.9',
  );
  // Same header, but the connection did not come from the proxy: ignore it.
  assert.equal(
    clientIp(forwarded('198.51.100.9', '203.0.113.7'), { trustedProxies: nginxNextDoor }),
    '198.51.100.9',
  );
  // A chain of two proxies we own skips past both to reach the player.
  assert.equal(
    clientIp(forwarded('127.0.0.1', '203.0.113.7, 10.0.0.2'), { trustedProxies: new IpAllowList(['127.0.0.1', '10.0.0.0/8']) }),
    '203.0.113.7',
  );
  // Setting the list is enough; the blunt flag does not have to be on as well.
  assert.equal(
    clientIp(forwarded('127.0.0.1', '198.51.100.9'), { trustProxy: false, trustedProxies: nginxNextDoor }),
    '198.51.100.9',
  );
});

test('HTTP: a forged X-Forwarded-For cannot claim an allowlisted IP', async () => {
  const ipLock = new IpAllowList(['203.0.113.7']);
  await withServer(async ({ core, base }) => {
    const { code } = core.createPairing({ channelId: CHANNEL });
    const send = (forwardedFor) => fetch(`${base}/v1/relay/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json', 'x-forwarded-for': forwardedFor },
      body: JSON.stringify({ code, name: 'a', message: 'b' }),
    });

    // nginx appends the real peer, so the client's claim stays on the left.
    const spoofed = await send('203.0.113.7, 198.51.100.9');
    assert.equal(spoofed.status, 403);
    assert.match((await spoofed.json()).message, /198\.51\.100\.9/);

    // A genuine allowlisted client works with the same header layout.
    assert.equal((await send('198.51.100.9, 203.0.113.7')).status, 200);
  }, { ipLock, trustProxy: true });
});

test('HTTP: a direct connection cannot use the forwarding header', async () => {
  const ipLock = new IpAllowList(['203.0.113.7']);
  // The proxy lives at 10.0.0.1, so the test client at 127.0.0.1 is not it.
  const trustedProxies = new IpAllowList(['10.0.0.1']);
  await withServer(async ({ core, base }) => {
    const { code } = core.createPairing({ channelId: CHANNEL });
    const response = await fetch(`${base}/v1/relay/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json', 'x-forwarded-for': '203.0.113.7' },
      body: JSON.stringify({ code, name: 'a', message: 'b' }),
    });
    assert.equal(response.status, 403);
    assert.match((await response.json()).message, /Your IP \(127\.0\.0\.1\)/);
  }, { ipLock, trustProxy: true, trustedProxies });
});

test('RateLimiter blocks over the limit and frees up after the window', () => {
  let now = 1_000_000;
  const limiter = new RateLimiter({ windowSeconds: 10, max: 2, now: () => now });

  assert.equal(limiter.take('code').allowed, true);
  assert.equal(limiter.take('code').allowed, true);
  const blocked = limiter.take('code');
  assert.equal(blocked.allowed, false);
  assert.ok(blocked.retryAfter >= 1 && blocked.retryAfter <= 10);
  // Other keys are unaffected.
  assert.equal(limiter.take('other').allowed, true);

  now += 11_000;
  assert.equal(limiter.take('code').allowed, true);
});

test('loadConfig applies defaults, file values and env overrides', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'relay-config-'));
  const file = path.join(dir, 'config.json');
  writeFileSync(file, JSON.stringify({
    publicUrl: 'https://relay.raspi.host',
    allowedIps: ['10.0.0.0/8'],
    trustProxy: true,
    trustedProxies: ['127.0.0.1'],
    rateLimit: { perPairing: { windowSeconds: 5, maxMessages: 4 } },
  }));

  const fromFile = loadConfig({}, file);
  assert.equal(fromFile.publicUrl, 'https://relay.raspi.host');
  assert.equal(fromFile.ipLock.locked, true);
  assert.equal(fromFile.ipLock.allows('10.9.9.9'), true);
  assert.equal(fromFile.trustProxy, true);
  assert.equal(fromFile.proxyLock.locked, true);
  assert.equal(fromFile.proxyLock.allows('127.0.0.1'), true);
  assert.equal(fromFile.proxyLock.allows('127.0.0.2'), false);
  assert.equal(fromFile.rateLimit.perPairing.maxMessages, 4);
  // Untouched values keep their documented defaults.
  assert.equal(fromFile.rateLimit.perIp.maxRequests, 600);
  assert.equal(fromFile.port, 8099);

  const fromEnv = loadConfig({ RELAY_ALLOWED_IPS: '203.0.113.7, 192.0.2.0/24', RELAY_PUBLIC_URL: 'https://x.test', RELAY_TRUSTED_PROXIES: '172.17.0.0/16' }, file);
  assert.equal(fromEnv.ipLock.allows('192.0.2.5'), true);
  assert.equal(fromEnv.ipLock.allows('10.9.9.9'), false);
  assert.equal(fromEnv.publicUrl, 'https://x.test');
  assert.equal(fromEnv.proxyLock.allows('172.17.0.8'), true);
  // The env list replaces the file list rather than merging with it.
  assert.equal(fromEnv.proxyLock.allows('127.0.0.1'), false);

  // Unset means "any peer may send the header", and no lock is reported.
  assert.equal(loadConfig({}, path.join(dir, 'missing.json')).proxyLock.locked, false);

  writeFileSync(file, JSON.stringify({ allowedIps: ['10.0.0.0/99'] }));
  assert.throws(() => loadConfig({}, file), /prefix length/);

  writeFileSync(file, JSON.stringify({ trustedProxies: ['proxy.internal'] }));
  assert.throws(() => loadConfig({}, file), /not an IP address/);
});

test('HTTP: a client outside allowedIps gets 403 with an explanation', async () => {
  const ipLock = new IpAllowList(['203.0.113.7']);
  await withServer(async ({ core, base }) => {
    const { code } = core.createPairing({ channelId: CHANNEL });
    const response = await fetch(`${base}/v1/relay/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code, name: 'a', message: 'b' }),
    });
    assert.equal(response.status, 403);
    const payload = await response.json();
    assert.equal(payload.error, 'ip_not_allowed');
    assert.match(payload.message, /Raspi Host/);
    assert.match(payload.message, /allowedIps/);
    // The same lock must protect polling.
    const poll = await fetch(`${base}/v1/relay/poll`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code, cursor: 0, wait: 0 }),
    });
    assert.equal(poll.status, 403);
    // Health is for monitoring and stays reachable.
    assert.equal((await fetch(`${base}/health`)).status, 200);
  }, { ipLock });
});

test('HTTP: an allowlisted local client passes the lock', async () => {
  const ipLock = new IpAllowList(['127.0.0.1']);
  await withServer(async ({ core, base }) => {
    const { code } = core.createPairing({ channelId: CHANNEL });
    const response = await fetch(`${base}/v1/relay/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code, name: 'a', message: 'b' }),
    });
    assert.equal(response.status, 200);
  }, { ipLock });
});

test('HTTP: per pairing rate limit returns 429 with a retry hint', async () => {
  const perPairingLimiter = new RateLimiter({ windowSeconds: 60, max: 2 });
  await withServer(async ({ core, base }) => {
    const { code } = core.createPairing({ channelId: CHANNEL });
    const send = () => fetch(`${base}/v1/relay/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code, name: 'a', message: 'spam' }),
    });
    assert.equal((await send()).status, 200);
    assert.equal((await send()).status, 200);
    const limited = await send();
    assert.equal(limited.status, 429);
    assert.ok(Number(limited.headers.get('retry-after')) >= 1);
    const payload = await limited.json();
    assert.equal(payload.error, 'rate_limited');
    assert.match(payload.message, /too quickly/);
  }, { perPairingLimiter });
});

test('HTTP: per IP rate limit applies before authentication', async () => {
  const perIpLimiter = new RateLimiter({ windowSeconds: 60, max: 1 });
  await withServer(async ({ base }) => {
    const first = await fetch(`${base}/v1/relay/poll`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code: 'NOPE', wait: 0 }),
    });
    assert.equal(first.status, 401);
    const second = await fetch(`${base}/v1/relay/poll`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code: 'NOPE', wait: 0 }),
    });
    assert.equal(second.status, 429);
  }, { perIpLimiter });
});

test('HTTP: system messages are passed through without a player name', async () => {
  await withServer(async ({ core, base, sent }) => {
    const { code } = core.createPairing({ channelId: CHANNEL });
    const response = await fetch(`${base}/v1/relay/send`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ code, system: true, message: '**Server started**' }),
    });
    assert.equal(response.status, 200);
    assert.deepEqual(sent, [{ channelId: CHANNEL, name: null, message: '**Server started**', system: true }]);
  });
});
