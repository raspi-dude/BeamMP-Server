import { createServer } from 'node:http';

const MAX_BODY_BYTES = 8 * 1024;
const MAX_POLL_SECONDS = 20;

function sendJson(res, status, payload) {
  const body = JSON.stringify(payload);
  res.writeHead(status, {
    'content-type': 'application/json',
    'content-length': Buffer.byteLength(body),
    'cache-control': 'no-store',
  });
  res.end(body);
}

function readJsonBody(req) {
  return new Promise((resolve) => {
    let size = 0;
    const chunks = [];
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > MAX_BODY_BYTES) {
        req.destroy();
        resolve(null);
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString('utf8')));
      } catch {
        resolve(null);
      }
    });
    req.on('error', () => resolve(null));
  });
}

/**
 * @param {object} options
 * @param {import('./relay-core.js').RelayCore} options.core
 * @param {(pairing: object, msg: {name: string, message: string}) => Promise<void>} options.sendToDiscord
 */
export function createRelayServer({ core, sendToDiscord, log = () => {} }) {
  return createServer(async (req, res) => {
    try {
      const url = new URL(req.url ?? '/', 'http://localhost');

      if (req.method === 'GET' && url.pathname === '/health') {
        return sendJson(res, 200, { ok: true, pairings: core.pairings.size });
      }
      if (req.method !== 'POST') {
        return sendJson(res, 404, { ok: false, error: 'not found' });
      }

      const body = await readJsonBody(req);
      if (!body || typeof body !== 'object') {
        return sendJson(res, 400, { ok: false, error: 'invalid json body' });
      }

      if (url.pathname === '/v1/relay/poll') {
        if (!core.authenticate(body.code)) {
          return sendJson(res, 401, { ok: false, error: 'invalid pairing code' });
        }
        const waitSeconds = Math.max(0, Math.min(Number(body.wait) || 0, MAX_POLL_SECONDS));
        const cursor = Number.isInteger(body.cursor) ? body.cursor : 0;
        const result = await core.poll(body.code, cursor, waitSeconds * 1000);
        return sendJson(res, 200, { ok: true, cursor: result.cursor, messages: result.messages });
      }

      if (url.pathname === '/v1/relay/send') {
        const pairing = core.authenticate(body.code);
        if (!pairing) {
          return sendJson(res, 401, { ok: false, error: 'invalid pairing code' });
        }
        const name = String(body.name ?? 'Player').replace(/[\r\n\t]+/g, ' ').slice(0, 64).trim() || 'Player';
        const message = String(body.message ?? '').replace(/[\r\n\t]+/g, ' ').trim().slice(0, 400);
        if (!message) {
          return sendJson(res, 400, { ok: false, error: 'empty message' });
        }
        try {
          await sendToDiscord(pairing, { name, message });
          log('relay', `${name}: ${message.length} chars -> channel ${pairing.channelId}`);
          return sendJson(res, 200, { ok: true });
        } catch (error) {
          log('error', `discord send failed: ${error.message}`);
          return sendJson(res, 502, { ok: false, error: 'discord send failed' });
        }
      }

      return sendJson(res, 404, { ok: false, error: 'not found' });
    } catch (error) {
      log('error', `request failed: ${error.message}`);
      if (!res.headersSent) {
        sendJson(res, 500, { ok: false, error: 'internal error' });
      } else {
        res.end();
      }
    }
  });
}
