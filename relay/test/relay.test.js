import test from 'node:test';
import assert from 'node:assert/strict';

import { RelayCore, generateCode, hashCode } from '../src/relay-core.js';
import { createRelayServer } from '../src/server.js';

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

async function withServer(run, { sendImpl } = {}) {
  const core = newCore();
  const sent = [];
  const server = createRelayServer({
    core,
    sendToDiscord: sendImpl ?? (async (pairing, msg) => {
      sent.push({ channelId: pairing.channelId, ...msg });
    }),
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
    assert.deepEqual(await response.json(), { ok: true, pairings: 1 });
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
    assert.deepEqual(sent, [{ channelId: CHANNEL, name: 'Player1', message: 'hello from the game' }]);

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
