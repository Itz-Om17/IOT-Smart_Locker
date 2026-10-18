import crypto from 'crypto';
import { AccessPin } from './models.js';

export const PIN_TTL_MS = 2 * 60 * 1000;
const LEN = Number(process.env.PIN_LENGTH) || 4;
const hash = pin => crypto.createHmac('sha256', process.env.PIN_SECRET).update(pin).digest('hex');
const sse = m => `data: ${JSON.stringify(m)}\n\n`;

// userId -> open SSE responses, plus the latest live PIN so a reconnecting browser gets it again
const clients = new Map(), latest = new Map();
export const hub = {
  add(id, res) {
    if (!clients.has(id)) clients.set(id, new Set());
    clients.get(id).add(res);
    const l = latest.get(id);
    if (l && l.expiresAt > Date.now()) {
      // Standard PINs: always replay on reconnect.
      // Emergency PINs: only replay AFTER the keypad has verified the 2-digit
      // code (l.verified = true). This prevents showing the PIN before physical
      // verification while still recovering a missed SSE event after verification.
      if (l.type !== 'emergency' || l.verified)
        res.write(sse({ event: 'issued', pin: l.pin, type: l.type, ttlMs: l.expiresAt - Date.now() }));
    }
  },
  remove: (id, res) => clients.get(id)?.delete(res),
  watching: id => (clients.get(id)?.size || 0) > 0,
  send: (id, msg) => clients.get(id)?.forEach(r => r.write(sse(msg))),
  issued(id, pin, type, ttlMs) {
    latest.set(id, { pin, type, expiresAt: Date.now() + ttlMs, verified: false });
    this.send(id, { event: 'issued', pin, type, ttlMs });
  },
  // Called by device.js after the ESP32 successfully verifies the 2-digit code.
  // Marks the cached emergency PIN as safe to replay on SSE reconnect.
  markEmergencyVerified(id) {
    const l = latest.get(id);
    if (l && l.type === 'emergency') {
      l.verified = true;
      latest.set(id, l);
    }
  },
  clear: id => latest.delete(id),
};

export const activePin = lockerId =>
  AccessPin.findOne({ locker: lockerId, usedAt: null, expiresAt: { $gt: new Date() } }).sort({ createdAt: -1 });

export async function issuePin({ user, locker, card, type, pinLength }) {
  const now = new Date();
  // one live PIN per locker: expire any earlier one
  await AccessPin.updateMany({ locker, usedAt: null, expiresAt: { $gt: now } }, { expiresAt: now });
  const len = pinLength || LEN;
  const pin = String(crypto.randomInt(0, 10 ** len)).padStart(len, '0');
  await AccessPin.create({ user, locker, card, type, pinHash: hash(pin), expiresAt: new Date(+now + PIN_TTL_MS) });
  hub.issued(String(user), pin, type, PIN_TTL_MS);
  return { ttlMs: PIN_TTL_MS };
}

// atomic: succeeds once, only if unexpired
export const consumePin = (locker, pin) =>
  AccessPin.findOneAndUpdate(
    { locker, pinHash: hash(pin), usedAt: null, expiresAt: { $gt: new Date() } },
    { usedAt: new Date() }, { new: true });
