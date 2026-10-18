import { Router } from 'express';
import bcrypt from 'bcryptjs';
import { Card, Locker, AccessLog, User } from '../models.js';
import { requireAuth, h } from '../middleware.js';

const r = Router();
r.use(requireAuth);

r.get('/lockers', h(async (req, res) =>
  res.json(await Locker.find({ _id: { $in: req.user.lockers } }).select('label deviceId'))));

r.get('/cards', h(async (req, res) =>
  res.json(await Card.find({ user: req.user._id }).select('uid label status lastUsedAt'))));

r.get('/logs', h(async (req, res) =>
  res.json(await AccessLog.find({ user: req.user._id }).sort({ createdAt: -1 }).limit(30).populate('locker', 'label'))));

r.post('/cards/:id/report-lost', h(async (req, res) => {
  const c = await Card.findOneAndUpdate({ _id: req.params.id, user: req.user._id }, { status: 'lost' }, { new: true });
  if (!c) return res.status(404).json({ error: 'Card not found' });
  await AccessLog.create({ user: req.user._id, event: 'card_lost', meta: { uid: c.uid } });
  res.json(c);
}));

// -----------------------------------------------------------------------
// Profile update: change email and/or password
// Always requires the current password for verification before any change.
// -----------------------------------------------------------------------
r.put('/user/profile', h(async (req, res) => {
  const { currentPassword, newEmail, newPassword } = req.body;

  if (!currentPassword)
    return res.status(400).json({ error: 'Current password is required to make any changes.' });

  if (!(await bcrypt.compare(String(currentPassword), req.user.passwordHash)))
    return res.status(401).json({ error: 'Current password is incorrect.' });

  const updates = {};

  if (newEmail && newEmail.trim().toLowerCase() !== req.user.email) {
    const taken = await User.findOne({ email: newEmail.trim().toLowerCase() });
    if (taken) return res.status(409).json({ error: 'That email address is already in use.' });
    updates.email = newEmail.trim().toLowerCase();
  }

  if (newPassword) {
    if (String(newPassword).length < 6)
      return res.status(400).json({ error: 'New password must be at least 6 characters.' });
    updates.passwordHash = await bcrypt.hash(String(newPassword), 10);
  }

  if (Object.keys(updates).length === 0)
    return res.status(400).json({ error: 'No changes were provided.' });

  const updated = await User.findByIdAndUpdate(req.user._id, updates, { new: true });
  res.json({ id: updated._id, name: updated.name, email: updated.email });
}));

export default r;
