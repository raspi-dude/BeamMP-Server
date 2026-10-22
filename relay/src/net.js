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
 *
 * The LAST entry is used, not the first. Nginx (and so Nginx Proxy Manager)
 * appends the peer address with `$proxy_add_x_forwarded_for`, which keeps
 * whatever the client sent to its left:
 *
 *   client sends:  X-Forwarded-For: 203.0.113.7      (an allowlisted IP)
 *   proxy sends:   X-Forwarded-For: 203.0.113.7, 198.51.100.9
 *
 * The rightmost entry is the address our proxy actually saw. Reading the first
 * one would let anyone walk past `allowedIps` by forging a header, and would
 * let one caller reset another's rate limit.
 *
 * `trustedProxies` narrows the trust to the peers that may send that header,
 * like nginx's `set_real_ip_from`. A direct connection to the relay port then
 * cannot forge anything, even with `trustProxy` on. Setting the list implies
 * trust for those peers; the plain `trustProxy` flag stays as "trust whoever
 * connects", which is what a network-isolated deployment needs.
 *
 * @param {object} req
 * @param {{trustProxy?: boolean, trustedProxies?: IpAllowList|null}} [options]
 */
export function clientIp(req, { trustProxy = false, trustedProxies = null } = {}) {
  const peer = normalizeIp(req.socket?.remoteAddress ?? '');
  const proxyList = trustedProxies?.locked ? trustedProxies : null;
  if (!(proxyList ? proxyList.allows(peer) : trustProxy)) {
    return peer;
  }

  const forwarded = req.headers['x-forwarded-for'];
  const rawForwarded = Array.isArray(forwarded) ? forwarded.join(',') : forwarded;
  const entries = String(rawForwarded ?? '')
    .split(',')
    .map((entry) => normalizeIp(entry.trim()))
    .filter((entry) => entry && net.isIP(entry));

  // Walk right to left past our own proxies: the first hop that is not one of
  // them is the furthest address we can vouch for.
  for (let i = entries.length - 1; i >= 0; --i) {
    if (proxyList && proxyList.allows(entries[i])) {
      continue;
    }
    return entries[i];
  }
  if (entries.length > 0) {
    return entries[0];
  }

  const realIp = normalizeIp(req.headers['x-real-ip']);
  if (realIp && net.isIP(realIp)) {
    return realIp;
  }
  return peer;
}
