import net from 'node:net';

/**
 * IPv4-mapped IPv6 addresses (what a dual-stack listener reports for IPv4
 * clients) and IPv6 loopback are folded onto their IPv4 form so a single
 * `127.0.0.1` or `1.2.3.4` entry in the config matches either way.
 */
export function normalizeIp(raw) {
  if (raw === undefined || raw === null) {
    return '';
  }
  let text = String(raw).trim();
  if (text.startsWith('[') && text.endsWith(']')) {
    text = text.slice(1, -1);
  }
  if (text.startsWith('::ffff:') && net.isIPv4(text.slice(7))) {
    return text.slice(7);
  }
  if (text === '::1') {
    return '127.0.0.1';
  }
  return text;
}

function parseEntry(entry) {
  const text = String(entry).trim();
  if (!text) {
    throw new Error('allowedIps contains an empty entry');
  }
  const slash = text.indexOf('/');
  const address = slash === -1 ? text : text.slice(0, slash);
  const family = net.isIP(address);
  if (!family) {
    throw new Error(`allowedIps entry "${entry}" is not an IP address or CIDR block`);
  }
  if (slash === -1) {
    return { address, family, prefix: null };
  }
  const prefixText = text.slice(slash + 1);
  const prefix = Number(prefixText);
  const maxPrefix = family === 4 ? 32 : 128;
  if (!Number.isInteger(prefix) || prefix < 0 || prefix > maxPrefix) {
    throw new Error(`allowedIps entry "${entry}" has an invalid prefix length (0-${maxPrefix})`);
  }
  return { address, family, prefix };
}

/**
 * Allowed IPs / CIDR blocks for game servers. An empty list means "allow all"
 * and is reported through `locked` so the relay can warn about it loudly.
 */
export class IpAllowList {
  constructor(entries = []) {
    /** @type {string[]} */
    this.entries = [];
    this.blockList = new net.BlockList();
    for (const entry of entries) {
      const parsed = parseEntry(entry);
      const type = parsed.family === 4 ? 'ipv4' : 'ipv6';
      if (parsed.prefix === null) {
        this.blockList.addAddress(parsed.address, type);
      } else {
        this.blockList.addSubnet(parsed.address, parsed.prefix, type);
      }
      this.entries.push(String(entry).trim());
    }
    this.locked = this.entries.length > 0;
  }

  allows(ip) {
    if (!this.locked) {
      return true;
    }
    const normalized = normalizeIp(ip);
    if (!normalized || !net.isIP(normalized)) {
      return false;
    }
    return this.blockList.check(normalized, net.isIPv4(normalized) ? 'ipv4' : 'ipv6');
  }
}

/**
 * The address to apply the allowlist and rate limits to.
 *
 * X-Forwarded-For / X-Real-IP are only honoured when `trustProxy` is on: a
 * proxy in front of the relay means every request arrives from 127.0.0.1, but
 * trusting the header without a proxy lets any client spoof an allowed IP.
 */
export function clientIp(req, { trustProxy = false } = {}) {
  if (trustProxy) {
    const forwarded = req.headers['x-forwarded-for'];
    const rawForwarded = Array.isArray(forwarded) ? forwarded[0] : forwarded;
    const first = String(rawForwarded ?? '').split(',')[0].trim();
    const candidate = normalizeIp(first);
    if (candidate && net.isIP(candidate)) {
      return candidate;
    }
    const realIp = normalizeIp(req.headers['x-real-ip']);
    if (realIp && net.isIP(realIp)) {
      return realIp;
    }
  }
  return normalizeIp(req.socket?.remoteAddress ?? '');
}
