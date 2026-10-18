import 'dotenv/config';
import mongoose from 'mongoose';
import bcrypt from 'bcryptjs';
import crypto from 'crypto';
import { User, Card, Locker, AccessPin, AccessLog } from './models.js';

await mongoose.connect(process.env.MONGO_URI);

console.log('Clearing existing collections...');
await Promise.all([User, Card, Locker, AccessPin, AccessLog].map(m => m.deleteMany({})));

// Standard device key from credentials.txt to prevent breaking ESP32 firmware config
const deviceKey = process.env.DEVICE_KEY || '6eabb61628ffe61985e23b3ecd42f709';
const deviceKeyHash = crypto.createHash('sha256').update(deviceKey).digest('hex');

const locker = await Locker.create({
  deviceId: 'LOCKER-01',
  label: 'Main Smart Locker',
  deviceKeyHash,
  failCount: 0,
});

console.log(`Locker created: ${locker.deviceId} (${locker.label})`);

const passwordHash = await bcrypt.hash('password123', 10);

// Define 5 distinct users and their assigned RFID cards (plus an Admin user)
const USERS_SEED_DATA = [
  {
    name: 'Alice Johnson',
    email: 'alice@locker.dev',
    role: 'user',
    cardUid: '53373FFA',
    cardLabel: 'Card #1 (53 37 3F FA)',
  },
  {
    name: 'Bob Smith',
    email: 'bob@locker.dev',
    role: 'user',
    cardUid: '13C075F7',
    cardLabel: 'Card #2 (13 C0 75 F7)',
  },
  {
    name: 'Charlie Davis',
    email: 'charlie@locker.dev',
    role: 'user',
    cardUid: 'C3746935',
    cardLabel: 'Card #3 (C3 74 69 35)',
  },
  {
    name: 'Diana Prince',
    email: 'diana@locker.dev',
    role: 'user',
    cardUid: '73D11330',
    cardLabel: 'Card #4 (73 D1 13 30)',
  },
  {
    name: 'Evan Wright',
    email: 'evan@locker.dev',
    role: 'user',
    cardUid: '39624407',
    cardLabel: 'Card #5 (39 62 44 07)',
  },
  {
    name: 'Demo Admin',
    email: 'demo@locker.dev',
    role: 'admin',
    cardUid: 'AB12CD34',
    cardLabel: 'Master Admin Card (AB 12 CD 34)',
  },
];

console.log('\n=============================================================');
console.log('       SEEDING USERS AND ASSIGNING RFID CARDS');
console.log('=============================================================');

for (const item of USERS_SEED_DATA) {
  const user = await User.create({
    name: item.name,
    email: item.email,
    passwordHash,
    role: item.role,
    lockers: [locker._id],
    isActive: true,
  });

  const card = await Card.create({
    uid: item.cardUid,
    label: item.cardLabel,
    user: user._id,
    status: 'active',
  });

  console.log(`User: ${user.name.padEnd(16)} | Email: ${user.email.padEnd(18)} | Card UID: ${card.uid} (${card.label})`);
}

console.log('=============================================================');
console.log(`Locker Device ID: ${locker.deviceId}`);
console.log(`Locker Device Key: ${deviceKey}`);
console.log('Default Password for all seeded users: password123');
console.log('=============================================================\n');

await mongoose.disconnect();
console.log('Database seeded successfully and disconnected.');
