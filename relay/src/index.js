import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { RelayCore } from './relay-core.js';
import { RateLimiter } from './rate-limit.js';
import { createRelayServer } from './server.js';
import { createBot, createDiscordSender } from './bot.js';
import { describeConfig, loadConfig } from './config.js';

function log(scope, message) {
  console.log(`[${new Date().toISOString()}] [${scope}] ${message}`);
}

/**
 * Token lookup order: DISCORD_TOKEN, then src/token.txt next to this file.
 * The file is git-ignored so a token cannot be committed by accident.
 */
function readToken() {
  if (process.env.DISCORD_TOKEN) {
    return process.env.DISCORD_TOKEN.trim();
  }
  const tokenFile = path.join(path.dirname(fileURLToPath(import.meta.url)), 'token.txt');
  if (existsSync(tokenFile)) {
    return readFileSync(tokenFile, 'utf8').trim();
  }
  return '';
}

const token = readToken();
if (!token) {
  console.error('No bot token: set DISCORD_TOKEN or create relay/src/token.txt (see relay/README.md)');
  process.exit(1);
}

let config;
try {
  config = loadConfig();
} catch (error) {
  console.error(`Bad relay config: ${error.message}`);
  process.exit(1);
}

const port = config.port;
const host = config.host;
const dataFile = config.dataFile.includes(path.sep) || config.dataFile.includes('/')
  ? config.dataFile
  : path.join(process.cwd(), 'data', config.dataFile);

const core = new RelayCore();
if (existsSync(dataFile)) {
  try {
    const loaded = core.loadJSON(JSON.parse(readFileSync(dataFile, 'utf8')));
    log('relay', `loaded ${loaded} pairing(s) from ${dataFile}`);
  } catch (error) {
    log('error', `could not read ${dataFile}: ${error.message}`);
  }
}

let persistTimer = null;
function persistNow() {
  try {
    mkdirSync(path.dirname(dataFile), { recursive: true });
    // Only hashes are written: a leaked data file cannot be replayed as codes.
    writeFileSync(dataFile, JSON.stringify(core.toJSON(), null, 2));
  } catch (error) {
    log('error', `could not write ${dataFile}: ${error.message}`);
  }
}
function schedulePersist() {
  if (persistTimer) {
    return;
  }
  persistTimer = setTimeout(() => {
    persistTimer = null;
    persistNow();
  }, 250);
}

for (const line of describeConfig(config).split('\n')) {
  log('config', line);
}
if (!config.ipLock.locked) {
  log('warn', 'This relay accepts game servers from ANY IP. Set allowedIps in relay/config.json to lock it to your hosting IPs.');
}

const bot = await createBot({
  core,
  token,
  log,
  onChange: schedulePersist,
  publicUrl: config.publicUrl,
  relayLocked: config.ipLock.locked,
});
const server = createRelayServer({
  core,
  sendToDiscord: createDiscordSender({ client: bot.client, log }),
  log,
  ipLock: config.ipLock,
  trustProxy: config.trustProxy,
  perPairingLimiter: new RateLimiter({
    windowSeconds: config.rateLimit.perPairing.windowSeconds,
    max: config.rateLimit.perPairing.maxMessages,
  }),
  perIpLimiter: new RateLimiter({
    windowSeconds: config.rateLimit.perIp.windowSeconds,
    max: config.rateLimit.perIp.maxRequests,
  }),
});

let shuttingDown = false;
async function shutdown(signal) {
  if (shuttingDown) {
    return;
  }
  shuttingDown = true;
  log('relay', `${signal} received, shutting down`);
  persistNow();
  await new Promise((resolve) => server.close(resolve));
  await bot.stop().catch(() => {});
  process.exit(0);
}

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, () => {
    shutdown(signal).catch((error) => {
      log('error', `shutdown failed: ${error.message}`);
      process.exit(1);
    });
  });
}

server.listen(port, host, () => log('relay', `HTTP relay listening on ${host}:${port}`));

try {
  await bot.start();
} catch (error) {
  log('error', `could not log in to Discord: ${error.message}`);
  if (/disallowed intents/i.test(error.message)) {
    log('error', 'Enable MESSAGE CONTENT INTENT at https://discord.com/developers/applications -> your app -> Bot -> Privileged Gateway Intents, then restart.');
  } else if (/invalid token|unauthorized/i.test(error.message)) {
    log('error', 'The token in DISCORD_TOKEN / relay/src/token.txt was rejected. Use Bot -> Reset Token and copy the new one.');
  }
  process.exit(1);
}
