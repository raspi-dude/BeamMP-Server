import { randomBytes, createHash } from 'node:crypto';

// Codes avoid look-alike characters (I, O, 0, 1) so they can be read aloud or
// copy-pasted from Discord without ambiguity.
const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

export function generateCode(length = 10) {
  const bytes = randomBytes(length);
  let code = '';
  for (let i = 0; i < length; i += 1) {
    code += CODE_ALPHABET[bytes[i] % CODE_ALPHABET.length];
  }
  return code;
}

export function hashCode(code) {
  return createHash('sha256').update(String(code).trim().toUpperCase()).digest('hex');
}

function relayError(status, message) {
  const error = new Error(message);
  error.status = status;
  return error;
}

/**
 * Channel-agnostic hub between Discord and paired game servers.
 *
 * Holds no Discord dependency on purpose: the bot feeds `publish()` and the
 * HTTP layer calls `poll()` / `authenticate()`, which keeps the whole thing
 * unit-testable without a token or network.
 */
export class RelayCore {
  constructor({ maxBuffered = 200, now = () => Date.now() } = {}) {
    this.maxBuffered = maxBuffered;
    this.now = now;
    /** @type {Map<string, object>} hash(code) -> pairing */
    this.pairings = new Map();
    /** @type {Map<string, object>} channelId -> pairing */
    this.byChannel = new Map();
  }

  createPairing({ guildId, channelId, label = '', code = null }) {
    if (!channelId) {
      throw relayError(400, 'channelId is required');
    }
    // One pairing per channel: re-pairing replaces the old code, which also
    // gives operators a clean way to rotate a leaked code.
    const existing = this.byChannel.get(channelId);
    if (existing) {
      this.revokeHash(existing.codeHash);
    }

    const plainCode = code ?? generateCode();
    const pairing = {
      codeHash: hashCode(plainCode),
      guildId: guildId ?? null,
      channelId,
      label: String(label).slice(0, 64),
      createdAt: this.now(),
      nextId: 1,
      messages: [],
      waiters: new Set(),
    };
    this.pairings.set(pairing.codeHash, pairing);
    this.byChannel.set(channelId, pairing);
    return { code: plainCode, pairing };
  }

  authenticate(code) {
    if (!code) {
      return null;
    }
    return this.pairings.get(hashCode(code)) ?? null;
  }

  revokeHash(codeHash) {
    const pairing = this.pairings.get(codeHash);
    if (!pairing) {
      return false;
    }
    this.pairings.delete(codeHash);
    if (this.byChannel.get(pairing.channelId) === pairing) {
      this.byChannel.delete(pairing.channelId);
    }
    for (const waiter of pairing.waiters) {
      waiter.resolve([]);
    }
    pairing.waiters.clear();
    return true;
  }

  revokeByCode(code) {
    const pairing = this.authenticate(code);
    return pairing ? this.revokeHash(pairing.codeHash) : false;
  }

  revokeByChannel(channelId) {
    const pairing = this.byChannel.get(channelId);
    return pairing ? this.revokeHash(pairing.codeHash) : false;
  }

  pairingForChannel(channelId) {
    return this.byChannel.get(channelId) ?? null;
  }

  /**
   * Discord -> game. Called for every message in a paired channel; returns the
   * number of long-poll clients that were woken.
   */
  publish(channelId, { name, message, authorId = null }) {
    const pairing = this.byChannel.get(channelId);
    if (!pairing) {
      return 0;
    }
    const cleanMessage = String(message ?? '').replace(/[\r\n\t]+/g, ' ').trim().slice(0, 400);
    if (!cleanMessage) {
      return 0;
    }
    const entry = {
      id: pairing.nextId,
      name: String(name ?? 'Discord').slice(0, 64),
      message: cleanMessage,
      authorId,
      at: this.now(),
    };
    pairing.nextId += 1;
    pairing.messages.push(entry);
    if (pairing.messages.length > this.maxBuffered) {
      pairing.messages.shift();
    }

    let woken = 0;
    for (const waiter of [...pairing.waiters]) {
      const pending = this.takeSince(pairing, waiter.cursor);
      if (pending.length > 0) {
        pairing.waiters.delete(waiter);
        waiter.resolve(pending);
        woken += 1;
      }
    }
    return woken;
  }

  takeSince(pairing, cursor) {
    return pairing.messages
      .filter((entry) => entry.id > cursor)
      .map((entry) => ({ id: entry.id, name: entry.name, message: entry.message }));
  }

  /**
   * Game -> relay. Resolves immediately when messages after `cursor` exist,
   * otherwise waits up to `waitMs`. HTTP callers turn this into a long-poll so
   * an idle server costs no traffic.
   */
  poll(code, cursor, waitMs) {
    const pairing = this.authenticate(code);
    if (!pairing) {
      return Promise.reject(relayError(401, 'invalid pairing code'));
    }
    const since = Number.isInteger(cursor) && cursor > 0 ? cursor : 0;
    const pending = this.takeSince(pairing, since);
    if (pending.length > 0) {
      return Promise.resolve({ cursor: pairing.nextId - 1, messages: pending });
    }
    const cappedWait = Math.max(0, Math.min(Number(waitMs) || 0, 30000));
    return new Promise((resolve) => {
      const waiter = {
        cursor: since,
        resolve: (messages) => {
          clearTimeout(timer);
          resolve({ cursor: pairing.nextId - 1, messages });
        },
      };
      const timer = setTimeout(() => {
        pairing.waiters.delete(waiter);
        resolve({ cursor: pairing.nextId - 1, messages: [] });
      }, cappedWait);
      timer.unref?.();
      pairing.waiters.add(waiter);
    });
  }

  toJSON() {
    return {
      pairings: [...this.pairings.values()].map((pairing) => ({
        codeHash: pairing.codeHash,
        guildId: pairing.guildId,
        channelId: pairing.channelId,
        label: pairing.label,
        createdAt: pairing.createdAt,
      })),
    };
  }

  loadJSON(data) {
    for (const stored of data?.pairings ?? []) {
      if (!stored?.codeHash || !stored?.channelId) {
        continue;
      }
      const pairing = {
        codeHash: stored.codeHash,
        guildId: stored.guildId ?? null,
        channelId: stored.channelId,
        label: stored.label ?? '',
        createdAt: stored.createdAt ?? this.now(),
        nextId: 1,
        messages: [],
        waiters: new Set(),
      };
      this.pairings.set(pairing.codeHash, pairing);
      this.byChannel.set(pairing.channelId, pairing);
    }
    return this.pairings.size;
  }
}
