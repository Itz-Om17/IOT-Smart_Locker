import 'dotenv/config';
import express from 'express';
import cookieParser from 'cookie-parser';
import mongoose from 'mongoose';
import auth from './routes/auth.js';
import device from './routes/device.js';
import pins from './routes/pins.js';
import user from './routes/user.js';

const app = express();
app.use(express.json(), cookieParser());
app.use('/api/auth', auth);
app.use('/api/device', device);
app.use('/api/pins', pins);
app.use('/api', user);
app.use((err, req, res, next) => { console.error(err); res.status(500).json({ error: 'Server error' }); });

await mongoose.connect(process.env.MONGO_URI);
app.listen(process.env.PORT || 5000, () => console.log('Locker API on :' + (process.env.PORT || 5000)));
