import mongoose from 'mongoose';
const { Schema, model } = mongoose;
const ref = r => ({ type: Schema.Types.ObjectId, ref: r });

export const User = model('User', new Schema({
  name: { type: String, required: true },
  email: { type: String, required: true, unique: true, lowercase: true },
  passwordHash: { type: String, required: true },
  role: { type: String, enum: ['user', 'admin'], default: 'user' },
  lockers: [ref('Locker')],
  isActive: { type: Boolean, default: true },
  lastEmergencyAt: Date,              // for 2-minute emergency access cooldown
}, { timestamps: true }));

export const Card = model('Card', new Schema({           // RFID data
  uid: {
    type: String,
    required: true,
    unique: true,
    uppercase: true,
    trim: true,
    set: v => (v ? String(v).toUpperCase().replace(/[^0-9A-F]/g, '') : v),
  },
  label: { type: String, default: 'RFID Card' },
  user: { ...ref('User'), required: true },
  status: { type: String, enum: ['active', 'blocked', 'lost'], default: 'active' },
  lastUsedAt: Date,
}, { timestamps: true }));

export const Locker = model('Locker', new Schema({       // one per ESP32
  deviceId: { type: String, required: true, unique: true },
  label: String,
  deviceKeyHash: { type: String, required: true },       // sha256 of the ESP32's key
  failCount: { type: Number, default: 0 },
  lockedOutUntil: Date,
}));

const pinSchema = new Schema({
  user: { ...ref('User'), required: true },
  locker: { ...ref('Locker'), required: true },
  card: ref('Card'),                                     // empty for emergency
  type: { type: String, enum: ['standard', 'emergency'], required: true },
  pinHash: { type: String, required: true },             // HMAC, never plaintext
  usedAt: Date,
  expiresAt: { type: Date, required: true },
}, { timestamps: true });
pinSchema.index({ expiresAt: 1 }, { expireAfterSeconds: 3600 });
pinSchema.index({ locker: 1, usedAt: 1, expiresAt: 1 });
export const AccessPin = model('AccessPin', pinSchema);

export const EmergencyRequest = model('EmergencyRequest', new Schema({
  user: { ...ref('User'), required: true },
  locker: { ...ref('Locker'), required: true },
  code: { type: String, required: true }, // unique 2-digit number (e.g. "47")
  status: { type: String, enum: ['pending', 'verified', 'expired'], default: 'pending' },
  expiresAt: { type: Date, required: true },
}, { timestamps: true }));

export const AccessLog = model('AccessLog', new Schema({
  user: ref('User'), locker: ref('Locker'),
  event: String,  // scan_ok scan_denied pin_ok pin_fail emergency_issued lockout card_lost emergency_code_issued emergency_code_ok emergency_code_denied
  meta: Object,
}, { timestamps: true }));

