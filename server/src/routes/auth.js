import { Router } from 'express';
import bcrypt from 'bcryptjs';
import jwt from 'jsonwebtoken';
import rateLimit from 'express-rate-limit';
import { User } from '../models.js';
import { h, requireAuth } from '../middleware.js';

const r = Router();
const pub = u => ({ id: u._id, name: u.name, email: u.email });

r.post('/login', rateLimit({ windowMs: 15 * 60e3, max: 20 }), h(async (req, res) => {
  const u = await User.findOne({ email: String(req.body.email || '').toLowerCase() });
  if (!u || !(await bcrypt.compare(String(req.body.password || ''), u.passwordHash)))
    return res.status(401).json({ error: 'Wrong email or password' });
  const token = jwt.sign({ id: u._id }, process.env.JWT_SECRET, { expiresIn: '8h' });
  res.cookie('token', token, { httpOnly: true, sameSite: 'strict', secure: process.env.NODE_ENV === 'production', maxAge: 8 * 3600e3 });
  res.json({ ...pub(u), token });
}));
r.post('/logout', (req, res) => res.clearCookie('token').json({ ok: true }));
r.get('/me', requireAuth, (req, res) => res.json(pub(req.user)));
export default r;
