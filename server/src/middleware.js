import crypto from 'crypto';
import jwt from 'jsonwebtoken';
import { User, Locker } from './models.js';

export const h = fn => (req, res, next) => fn(req, res, next).catch(next);

export const requireAuth = h(async (req, res, next) => {
  try {
    const authHeader = req.get('Authorization') || '';
    const bearer = authHeader.startsWith('Bearer ') ? authHeader.slice(7) : null;
    const token = bearer || req.cookies?.token;
    const { id } = jwt.verify(token, process.env.JWT_SECRET);
    req.user = await User.findById(id);
  } catch { /* fall through */ }
  if (!req.user?.isActive) return res.status(401).json({ error: 'Please sign in' });
  next();
});

export const deviceAuth = h(async (req, res, next) => {
  const locker = await Locker.findOne({ deviceId: req.get('x-device-id') });
  const a = Buffer.from(crypto.createHash('sha256').update(req.get('x-device-key') || '').digest('hex'));
  const b = Buffer.from(locker?.deviceKeyHash || '');
  if (!locker || a.length !== b.length || !crypto.timingSafeEqual(a, b))
    return res.status(401).json({ error: 'Unknown device' });
  req.locker = locker;
  next();
});
