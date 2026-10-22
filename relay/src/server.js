import { createServer } from 'node:http';

import { clientIp } from './net.js';

const MAX_BODY_BYTES = 8 * 1024;
const MAX_POLL_SECONDS = 20;

function sendJson(res, status, payload, headers = {}) {
  const body = JSON.stringify(payload);
  res.writeHead(status, {
    'content-type': 'application/json',
    'content-length': Buffer.byteLength(body),
    'cache-control': 'no-store',
    ...headers,
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
 * @param {(pairing: object, msg: {name?: string, message: string, system?: boolean}) => Promise<void>} options.sendToDiscord
 * @param {import('./net.js').IpAllowList} [options.ipLock] refuse anything not on this list
 * @param {boolean} [options.trustProxy] honour X-Forwarded-For for the client IP
 * @param {import('./net.js').IpAllowList} [options.trustedProxies] only these peers may send that header
 * @param {import('./rate-limit.js').RateLimiter} [options.perPairingLimiter] limits game -> Discord messages
 * @param {import('./rate-limit.js').RateLimiter} [options.perIpLimiter] limits all relay traffic per IP
 */
export function createRelayServer({
  core,
  sendToDiscord,
  log = () => {},
  ipLock = null,
  trustProxy = false,
  trustedProxies = null,
  perPairingLimiter = null,
  perIpLimiter = null,
}) {
  return createServer(async (req, res) => {
    try {
      const url = new URL(req.url ?? '/', 'http://localhost');

      // Monitoring is not part of the relay protocol, so it stays reachable.
      if (req.method === 'GET' && url.pathname === '/health') {
        return sendJson(res, 200, {
          ok: true,
          pairings: core.pairings.size,
          ipLock: ipLock?.locked ? 'on' : 'off',
        });
      }
      if (req.method !== 'POST') {
        return sendJson(res, 404, { ok: false, error: 'not found' });
      }

      const ip = clientIp(req, { trustProxy, trustedProxies });

      if (ipLock && !ipLock.allows(ip)) {
        log('warn', `refused ${req.url} from ${ip || 'unknown IP'}: not in allowedIps`);
        return sendJson(res, 403, {
          ok: false,
          error: 'ip_not_allowed',
          message: 'This relay only accepts connections from Raspi Host game servers. '
            + `Your IP (${ip || 'unknown'}) is not on the allowlist; the relay owner can add it in allowedIps.`,
        });
      }

      if (perIpLimiter) {
        const limit = perIpLimiter.take(ip || 'unknown');
        if (!limit.allowed) {
          log('warn', `rate limited ${ip || 'unknown IP'} for ${req.url} (retry in ${limit.retryAfter}s)`);
          return sendJson(res, 429, {
            ok: false,
            error: 'rate_limited',
            message: `Too many requests from this IP (${ip || 'unknown'}); retry in ${limit.retryAfter} second(s).`,
            retryAfter: limit.retryAfter,
          }, { 'retry-after': String(limit.retryAfter) });
        }
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

        const message = String(body.message ?? '').replace(/[\r\n\t]+/g, ' ').trim().slice(0, 400);
        if (!message) {
          return sendJson(res, 400, { ok: false, error: 'empty message' });
        }

        if (perPairingLimiter) {
          const limit = perPairingLimiter.take(pairing.codeHash);
          if (!limit.allowed) {
            log('warn', `rate limited pairing for channel ${pairing.channelId} (retry in ${limit.retryAfter}s)`);
            return sendJson(res, 429, {
              ok: false,
              error: 'rate_limited',
              message: `This server is sending messages too quickly; retry in ${limit.retryAfter} second(s).`,
              retryAfter: limit.retryAfter,
            }, { 'retry-after': String(limit.retryAfter) });
          }
        }

        const system = body.system === true;
        const name = system
          ? null
          : (String(body.name ?? 'Player').replace(/[\r\n\t]+/g, ' ').slice(0, 64).trim() || 'Player');

        try {
          await sendToDiscord(pairing, { name, message, system });
          log('relay', system
            ? `${message.length} char system line -> channel ${pairing.channelId}`
            : `${name}: ${message.length} chars -> channel ${pairing.channelId}`);
          return sendJson(res, 200, { ok: true });
        } catch (error) {
          log('error', `discord send failed: ${error.message}`);
          return sendJson(res, 502, { ok: false, error: 'discord_send_failed', message: error.message });
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
