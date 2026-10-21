import { existsSync, readFileSync } from 'node:fs';
import path from 'node:path';

import { IpAllowList } from './net.js';

/**
 * Defaults are deliberately safe for a single box: the relay is reachable,
 * but the IP lock and rate limits only tighten once they are configured.
 */
export const DEFAULT_CONFIG = {
  http: { host: '0.0.0.0', port: 8099 },
  // Shown to server owners by /pair. Set it, or the reply cannot tell them
  // where to point RelayUrl.
  publicUrl: '',
  // Empty means "any IP may connect", which is only sensible for testing.
  // Add the Raspi Host egress addresses (IPs or CIDR) to actually lock it.
  allowedIps: [],
  // Set to true only when a reverse proxy you control forwards X-Forwarded-For.
  // Otherwise a client could just claim any IP and bypass the lock.
  trustProxy: false,
  rateLimit: {
    // Per pairing code: game chat. 30 lines per 10s is far above real chat.
    perPairing: { windowSeconds: 10, maxMessages: 30 },
    // Per client IP: everything, mostly long-polls. One server polls every few
    // seconds, so 600/min leaves room for a whole hosting box behind one IP.
    perIp: { windowSeconds: 60, maxRequests: 600 },
  },
  dataFile: 'data/pairings.json',
};

function toPositiveInt(value, fallback, name) {
  if (value === undefined || value === null || value === '') {
    return fallback;
  }
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    throw new Error(`${name} must be a positive number, got ${JSON.stringify(value)}`);
  }
  return Math.floor(parsed);
}

function asBoolean(value, fallback, name) {
  if (value === undefined || value === null || value === '') {
    return fallback;
  }
  if (typeof value === 'boolean') {
    return value;
  }
  const text = String(value).trim().toLowerCase();
  if (['1', 'true', 'yes', 'on'].includes(text)) {
    return true;
  }
  if (['0', 'false', 'no', 'off'].includes(text)) {
    return false;
  }
  throw new Error(`${name} must be true or false, got ${JSON.stringify(value)}`);
}

function asIpList(value, name) {
  if (value === undefined || value === null) {
    return [];
  }
  if (Array.isArray(value)) {
    return value.map((entry) => String(entry).trim()).filter(Boolean);
  }
  // Comma separated is what an env var or a one-line config can offer.
  return String(value)
    .split(',')
    .map((entry) => entry.trim())
    .filter(Boolean);
}

/**
 * Merges the JSON file (if present) with environment overrides. Environment
 * wins so a container can stay on the same bind-mounted config.
 *
 * @param {object} [env] process.env-alike, injectable for tests
 * @param {string} [configPath] explicit path, else relay/config.json next to src/
 */
export function loadConfig(env = process.env, configPath = null) {
  const file = configPath ?? path.join(process.cwd(), 'config.json');
  let fromFile = {};
  if (existsSync(file)) {
    try {
      fromFile = JSON.parse(readFileSync(file, 'utf8'));
    } catch (error) {
      throw new Error(`could not read ${file}: ${error.message}`);
    }
  }

  const perPairing = { ...DEFAULT_CONFIG.rateLimit.perPairing, ...(fromFile.rateLimit?.perPairing ?? {}) };
  const perIp = { ...DEFAULT_CONFIG.rateLimit.perIp, ...(fromFile.rateLimit?.perIp ?? {}) };

  const config = {
    http: { ...DEFAULT_CONFIG.http, ...(fromFile.http ?? {}) },
    publicUrl: env.RELAY_PUBLIC_URL ?? fromFile.publicUrl ?? DEFAULT_CONFIG.publicUrl,
    allowedIps: asIpList(env.RELAY_ALLOWED_IPS, 'RELAY_ALLOWED_IPS').length > 0
      ? asIpList(env.RELAY_ALLOWED_IPS, 'RELAY_ALLOWED_IPS')
      : asIpList(fromFile.allowedIps, 'allowedIps'),
    trustProxy: asBoolean(env.RELAY_TRUST_PROXY, asBoolean(fromFile.trustProxy, DEFAULT_CONFIG.trustProxy, 'trustProxy'), 'RELAY_TRUST_PROXY'),
    rateLimit: {
      perPairing: {
        windowSeconds: toPositiveInt(perPairing.windowSeconds, DEFAULT_CONFIG.rateLimit.perPairing.windowSeconds, 'rateLimit.perPairing.windowSeconds'),
        maxMessages: toPositiveInt(perPairing.maxMessages, DEFAULT_CONFIG.rateLimit.perPairing.maxMessages, 'rateLimit.perPairing.maxMessages'),
      },
      perIp: {
        windowSeconds: toPositiveInt(perIp.windowSeconds, DEFAULT_CONFIG.rateLimit.perIp.windowSeconds, 'rateLimit.perIp.windowSeconds'),
        maxRequests: toPositiveInt(perIp.maxRequests, DEFAULT_CONFIG.rateLimit.perIp.maxRequests, 'rateLimit.perIp.maxRequests'),
      },
    },
    dataFile: env.DATA_FILE ?? fromFile.dataFile ?? DEFAULT_CONFIG.dataFile,
    port: toPositiveInt(env.PORT ?? fromFile.http?.port, DEFAULT_CONFIG.http.port, 'PORT'),
    host: env.HOST ?? fromFile.http?.host ?? DEFAULT_CONFIG.http.host,
    configPath: existsSync(file) ? file : null,
  };

  // Throws on a malformed entry, which is much better than silently allowing
  // nobody (or everybody) because of a typo.
  config.ipLock = new IpAllowList(config.allowedIps);
  return config;
}

/** Human readable summary printed at startup. */
export function describeConfig(config) {
  const lines = [];
  lines.push(`config: ${config.configPath ?? 'defaults (no config.json found)'}`);
  lines.push(config.ipLock.locked
    ? `ip lock: on (${config.allowedIps.join(', ')})`
    : 'ip lock: OFF - any IP may use this relay. Set allowedIps to restrict it.');
  lines.push(`trust proxy headers: ${config.trustProxy ? 'yes' : 'no'}`);
  lines.push(`rate limit: ${config.rateLimit.perPairing.maxMessages} messages/${config.rateLimit.perPairing.windowSeconds}s per pairing, ${config.rateLimit.perIp.maxRequests} requests/${config.rateLimit.perIp.windowSeconds}s per IP`);
  lines.push(config.publicUrl ? `public url: ${config.publicUrl}` : 'public url: not set - /pair will print a placeholder');
  return lines.join('\n');
}
