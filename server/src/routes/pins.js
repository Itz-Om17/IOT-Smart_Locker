import { Router } from 'express';
import crypto from 'crypto';
import bcrypt from 'bcryptjs';
import jwt from 'jsonwebtoken';
import rateLimit from 'express-rate-limit';
import { User, AccessLog, EmergencyRequest } from '../models.js';
import { requireAuth, h } from '../middleware.js';
import { hub, issuePin } from '../services.js';

const r = Router();

// Server-Sent Events stream: browser connects here to receive real-time PIN events.
// Supports token query parameter for absolute per-tab session isolation!
r.get('/stream', h(async (req, res) => {
  let token = req.query.token || req.cookies?.token;
  if (!token && req.headers.authorization?.startsWith('Bearer ')) {
    token = req.headers.authorization.slice(7);
  }

  let user = null;
  if (token) {
    try {
      const decoded = jwt.verify(token, process.env.JWT_SECRET);
      user = await User.findById(decoded.id);
    } catch {}
  }

  if (!user || !user.isActive) {
    return res.status(401).json({ error: 'Please sign in' });
  }

  res.set({ 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive' });
  res.flushHeaders();
  const id = String(user._id);
  hub.add(id, res);
  const ping = setInterval(() => res.write(': ping\n\n'), 25000);
  req.on('close', () => { clearInterval(ping); hub.remove(id, res); });
}));

// Apply authentication middleware to remaining routes
r.use(requireAuth);

// Emergency access: lost card → re-enter password → get a unique 2-digit code
// 2-minute cooldown enforced per user to prevent abuse
const EMERGENCY_COOLDOWN_MS = 2 * 60 * 1000;
r.post('/emergency', rateLimit({ windowMs: 15 * 60e3, max: 5 }), h(async (req, res) => {
  const { lockerId, password } = req.body;

  if (!req.user.lockers.some(l => String(l) === String(lockerId)))
    return res.status(403).json({ error: 'You do not have access to that locker' });

  if (!(await bcrypt.compare(String(password || ''), req.user.passwordHash)))
    return res.status(401).json({ error: 'Wrong password' });

  // Enforce 2-minute cooldown between emergency requests
  if (req.user.lastEmergencyAt) {
    const elapsed = Date.now() - new Date(req.user.lastEmergencyAt).getTime();
    if (elapsed < EMERGENCY_COOLDOWN_MS) {
      const waitSec = Math.ceil((EMERGENCY_COOLDOWN_MS - elapsed) / 1000);
      return res.status(429).json({
        error: `Emergency access is on cooldown. Please wait ${waitSec} more second${waitSec !== 1 ? 's' : ''} before requesting again.`,
        cooldownSec: waitSec,
      });
    }
  }

  // Find any active pending codes on this locker to prevent collision
  const existing = await EmergencyRequest.find({
    locker: lockerId,
    status: 'pending',
    expiresAt: { $gt: new Date() },
  }).select('code');
  const usedCodes = new Set(existing.map(e => e.code));

  // Invalidate any previous pending emergency request for this user
  await EmergencyRequest.updateMany(
    { user: req.user._id, locker: lockerId, status: 'pending' },
    { status: 'expired' }
  );

  // Pick a unique 2-digit code (10 to 99)
  let code = '';
  for (let attempt = 0; attempt < 50; attempt++) {
    const candidate = String(crypto.randomInt(10, 100));
    if (!usedCodes.has(candidate)) {
      code = candidate;
      break;
    }
  }
  if (!code) code = String(crypto.randomInt(10, 100));

  // Valid for 5 minutes (time for user to enter code at keypad)
  const expiresAt = new Date(Date.now() + 5 * 60 * 1000);

  await EmergencyRequest.create({
    user: req.user._id,
    locker: lockerId,
    code,
    status: 'pending',
    expiresAt,
  });

  // Record cooldown timestamp
  await User.updateOne({ _id: req.user._id }, { lastEmergencyAt: new Date() });
  await AccessLog.create({ user: req.user._id, locker: lockerId, event: 'emergency_code_issued', meta: { code } });

  res.json({ ok: true, code, ttlSec: 300 });
}));

export default r;

