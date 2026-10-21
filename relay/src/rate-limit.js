/**
 * Sliding-window limiter keyed by pairing code or client IP.
 *
 * Sliding window (rather than a fixed bucket) so a client cannot burst twice
 * the limit by straddling a window boundary. Memory stays bounded: expired
 * timestamps are pruned on access and the key map is capped, which matters
 * because the keys include client IPs.
 */
export class RateLimiter {
  constructor({ windowSeconds, max, maxKeys = 10000, now = () => Date.now() } = {}) {
    if (!(windowSeconds > 0)) {
      throw new Error('windowSeconds must be positive');
    }
    if (!(max > 0)) {
      throw new Error('max must be positive');
    }
    this.windowMs = windowSeconds * 1000;
    this.max = max;
    this.maxKeys = maxKeys;
    this.now = now;
    /** @type {Map<string, number[]>} */
    this.hits = new Map();
  }

  /** @returns {{allowed: boolean, remaining: number, retryAfter: number}} */
  take(key) {
    const now = this.now();
    const cutoff = now - this.windowMs;
    let timestamps = this.hits.get(key);
    if (timestamps) {
      // Drop everything outside the window before deciding.
      let keepFrom = 0;
      while (keepFrom < timestamps.length && timestamps[keepFrom] <= cutoff) {
        keepFrom += 1;
      }
      timestamps = keepFrom > 0 ? timestamps.slice(keepFrom) : timestamps;
    } else {
      timestamps = [];
    }

    if (timestamps.length >= this.max) {
      const oldest = timestamps[0] ?? now;
      this.hits.set(key, timestamps);
      return {
        allowed: false,
        remaining: 0,
        retryAfter: Math.max(1, Math.ceil((oldest + this.windowMs - now) / 1000)),
      };
    }

    timestamps.push(now);
    this.hits.set(key, timestamps);
    this.evictIfNeeded();
    return { allowed: true, remaining: this.max - timestamps.length, retryAfter: 0 };
  }

  evictIfNeeded() {
    if (this.hits.size <= this.maxKeys) {
      return;
    }
    const cutoff = this.now() - this.windowMs;
    for (const [key, timestamps] of this.hits) {
      if (timestamps.length === 0 || timestamps[timestamps.length - 1] <= cutoff) {
        this.hits.delete(key);
      }
      if (this.hits.size <= this.maxKeys) {
        return;
      }
    }
    // Still too many live keys: drop the oldest ones (Map keeps insertion order).
    for (const key of this.hits.keys()) {
      this.hits.delete(key);
      if (this.hits.size <= this.maxKeys) {
        return;
      }
    }
  }
}
