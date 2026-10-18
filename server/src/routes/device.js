import { Router } from 'express';
import { Card, Locker, AccessLog, EmergencyRequest } from '../models.js';
import { deviceAuth, h } from '../middleware.js';
import { hub, issuePin, activePin, consumePin } from '../services.js';

const r = Router();
r.use(deviceAuth);
const MAX_FAILS = 5, LOCKOUT_MS = 5 * 60e3;

// ESP32: user presses 'A' and enters 2-digit code on keypad.
// Finds the pending EmergencyRequest for this locker and code.
// If valid: generates 6-digit PIN, pushes to ONLY that user's dashboard via SSE, and starts timer.
r.post('/emergency-verify-code', h(async (req, res) => {
  const L = req.locker;
  if (L.lockedOutUntil > new Date()) return res.status(429).json({ ok: false, reason: 'locked_out' });

  const code = String(req.body.code || '').trim();
  const request = await EmergencyRequest.findOne({
    locker: L._id,
    code,
    status: 'pending',
    expiresAt: { $gt: new Date() },
  }).populate('user');

  if (!request) {
    await AccessLog.create({ locker: L._id, event: 'emergency_code_denied', meta: { code } });
    return res.status(404).json({ ok: false, reason: 'invalid_code' });
  }

  request.status = 'verified';
  await request.save();

  // Issue 6-digit emergency PIN exclusively for this user
  const { ttlMs } = await issuePin({
    user: request.user._id,
    locker: L._id,
    type: 'emergency',
    pinLength: 6,
  });

  // Mark the PIN as verified so it can be replayed on SSE reconnect
  // (in case the browser missed the real-time push while the user was at the locker).
  hub.markEmergencyVerified(String(request.user._id));

  await AccessLog.create({
    user: request.user._id,
    locker: L._id,
    event: 'emergency_code_ok',
    meta: { code },
  });

  res.json({
    ok: true,
    name: request.user.name,
    ttlMs,
    watching: hub.watching(String(request.user._id)),
  });
}));

// ESP32: card tapped. The PIN goes to the browser only; the device gets just the timer.
r.post('/scan', h(async (req, res) => {
  const L = req.locker;
  if (L.lockedOutUntil > new Date()) return res.status(429).json({ ok: false, reason: 'locked_out' });
  const uid = String(req.body.uid || '').toUpperCase().replace(/[^0-9A-F]/g, '');
  const card = await Card.findOne({ uid }).populate('user');
  const deny = async reason => {
    await AccessLog.create({ locker: L._id, user: card?.user?._id, event: 'scan_denied', meta: { uid, reason } });
    res.status(403).json({ ok: false, reason });
  };
  if (!card) return deny('unknown_card');
  if (card.status !== 'active') return deny('card_' + card.status);
  if (!card.user.isActive || !card.user.lockers.some(l => l.equals(L._id))) return deny('no_permission');

  card.lastUsedAt = new Date(); await card.save();
  const { ttlMs } = await issuePin({ user: card.user._id, locker: L._id, card: card._id, type: 'standard' });
  await AccessLog.create({ locker: L._id, user: card.user._id, event: 'scan_ok', meta: { uid } });
  res.json({ ok: true, ttlMs, name: card.user.name, watching: hub.watching(String(card.user._id)) });
}));

// ESP32 polls every 1-2s: drives the LCD countdown and enables the keypad
r.get('/state', h(async (req, res) => {
  const p = await activePin(req.locker._id);
  if (p) {
    return res.json({
      active: true,
      type: p.type,
      remainingMs: Math.max(0, p.expiresAt - Date.now()),
      watching: hub.watching(String(p.user)),
    });
  }

  // Check if there is an active pending 2-digit emergency verification code for this locker
  const pendingEmrg = await EmergencyRequest.findOne({
    locker: req.locker._id,
    status: 'pending',
    expiresAt: { $gt: new Date() },
  });

  if (pendingEmrg) {
    return res.json({
      active: true,
      type: 'emergency_pending',
      hasPendingEmergency: true,
      remainingMs: Math.max(0, pendingEmrg.expiresAt - Date.now()),
      lockedOut: req.locker.lockedOutUntil > new Date(),
    });
  }

  res.json({ active: false, remainingMs: 0, lockedOut: req.locker.lockedOutUntil > new Date() });
}));

r.post('/verify-pin', h(async (req, res) => {
  const L = req.locker;
  if (L.lockedOutUntil > new Date()) return res.status(429).json({ unlock: false, reason: 'locked_out' });
  const pin = String(req.body.pin || '');
  const doc = /^\d{1,12}$/.test(pin) ? await consumePin(L._id, pin) : null;
  if (!doc) {
    const u = await Locker.findByIdAndUpdate(L._id, { $inc: { failCount: 1 } }, { new: true });
    let event = 'pin_fail';
    if (u.failCount >= MAX_FAILS) {
      await Locker.updateOne({ _id: L._id }, { failCount: 0, lockedOutUntil: new Date(Date.now() + LOCKOUT_MS) });
      event = 'lockout';
    }
    await AccessLog.create({ locker: L._id, event });
    return res.status(401).json({ unlock: false, reason: event === 'lockout' ? 'locked_out' : 'invalid' });
  }
  await Locker.updateOne({ _id: L._id }, { failCount: 0 });
  const uid = String(doc.user);
  hub.clear(uid); hub.send(uid, { event: 'used' });
  await AccessLog.create({ user: doc.user, locker: L._id, event: 'pin_ok', meta: { type: doc.type } });
  res.json({ unlock: true, holdMs: 4000 });
}));
export default r;
