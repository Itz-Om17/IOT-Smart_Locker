import { createContext, useContext, useEffect, useReducer, useState } from 'react';
import { api, getToken, setToken } from './api';

/* ---------- Auth context ---------- */
const Auth = createContext();
const useAuth = () => useContext(Auth);

function AuthProvider({ children }) {
  const [user, setUser] = useState(undefined);
  useEffect(() => {
    api('/auth/me').then(setUser).catch(() => { setToken(null); setUser(null); });
  }, []);
  const login = async (email, password) => {
    const res = await api('/auth/login', { method: 'POST', body: { email, password } });
    if (res.token) setToken(res.token);
    setUser(res);
    return res;
  };
  const logout = async () => {
    await api('/auth/logout', { method: 'POST' }).catch(() => {});
    setToken(null);
    setUser(null);
  };
  const refreshUser = async () => {
    try {
      const u = await api('/auth/me');
      setUser(u);
    } catch {
      setToken(null);
      setUser(null);
    }
  };
  return <Auth.Provider value={{ user, login, logout, refreshUser }}>{children}</Auth.Provider>;
}

/* ---------- PIN state machine: idle → active → expired | used ---------- */
const Pin = createContext();
const usePin = () => useContext(Pin);

function pinReducer(s, a) {
  switch (a.type) {
    case 'ISSUED': return { status: 'active', pin: a.pin, kind: a.kind, ttlMs: a.ttlMs, deadline: performance.now() + a.ttlMs };
    case 'EXPIRED': return s.status === 'active' ? { status: 'expired' } : s;
    case 'USED':    return { status: 'used' };
    default:        return s;
  }
}

function PinProvider({ children }) {
  const [state, dispatch] = useReducer(pinReducer, { status: 'idle' });
  useEffect(() => {
    const token = getToken();
    const url = '/api/pins/stream' + (token ? '?token=' + encodeURIComponent(token) : '');
    const es = new EventSource(url, { withCredentials: true });
    es.onmessage = e => {
      const d = JSON.parse(e.data);
      if (d.event === 'issued') dispatch({ type: 'ISSUED', pin: d.pin, kind: d.type, ttlMs: d.ttlMs });
      if (d.event === 'used')   dispatch({ type: 'USED' });
    };
    return () => es.close();
  }, []);
  return <Pin.Provider value={{ ...state, dispatch }}>{children}</Pin.Provider>;
}

/* ---------- Helpers ---------- */
const mmss = ms => {
  const s = Math.ceil(ms / 1000);
  return `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
};

/* ====================================================================
   LOGIN PAGE
   ==================================================================== */
function Login() {
  const { login } = useAuth();
  const [email, setEmail] = useState(''), [password, setPassword] = useState(''), [err, setErr] = useState('');
  const submit = async e => {
    e.preventDefault(); setErr('');
    try { await login(email, password); } catch (x) { setErr(x.message); }
  };
  return (
    <main className="narrow">
      <h1>Smart Locker</h1>
      <form onSubmit={submit} className="stack">
        <label>Email<input type="email" value={email} onChange={e => setEmail(e.target.value)} required /></label>
        <label>Password<input type="password" value={password} onChange={e => setPassword(e.target.value)} required /></label>
        {err && <p className="err" role="alert">{err}</p>}
        <button>Sign in</button>
      </form>
    </main>
  );
}

/* ====================================================================
   PIN PANEL  – shows the dynamic OTP and a live countdown ring
   ==================================================================== */
function PinPanel() {
  const { status, pin, kind, ttlMs, deadline, dispatch } = usePin();
  const [left, setLeft] = useState(0);

  useEffect(() => {
    if (status !== 'active') return;
    const tick = () => {
      const l = Math.max(0, deadline - performance.now());
      setLeft(l);
      if (l === 0) dispatch({ type: 'EXPIRED' });
    };
    tick();
    const t = setInterval(tick, 200);
    return () => clearInterval(t);
  }, [status, deadline]);

  const C = 2 * Math.PI * 54;
  const isEmergency = kind === 'emergency' && status === 'active';

  const idleMsg = {
    idle:    'Tap your card on the locker to get a PIN.',
    expired: 'PIN expired. Tap your card again.',
    used:    'PIN used — locker opened successfully.',
  };

  return (
    <section className={'lcd ' + (isEmergency ? 'emergency' : '')} aria-live="polite">
      {status === 'active' ? (
        <>
          <p className="lcd-label">
            {isEmergency ? '🚨 Emergency PIN' : 'Your PIN'} — type it on the keypad.
          </p>
          <div className="lcd-row">
            <output className="digits" aria-label={'PIN ' + pin.split('').join(' ')}>{pin}</output>
            <svg viewBox="0 0 120 120" width="96" height="96" aria-hidden="true">
              <circle cx="60" cy="60" r="54" className="ring-bg" />
              <circle cx="60" cy="60" r="54" className="ring" strokeDasharray={C}
                strokeDashoffset={C * (1 - left / ttlMs)} transform="rotate(-90 60 60)" />
              <text x="60" y="66" textAnchor="middle" className="ring-t">{mmss(left)}</text>
            </svg>
          </div>
          {isEmergency && (
            <p className="lcd-tip">
              At the locker: enter this 6-digit PIN on the keypad → press <kbd>#</kbd> to unlock
            </p>
          )}
        </>
      ) : (
        <p className="lcd-idle">{idleMsg[status] || idleMsg.idle}</p>
      )}
    </section>
  );
}

/* ====================================================================
   EMERGENCY ACCESS  – issues a 6-digit PIN without RFID card
   ==================================================================== */
function Emergency() {
  const [lockers, setLockers]   = useState([]);
  const [lockerId, setLockerId] = useState('');
  const [password, setPassword] = useState('');
  const [code, setCode]         = useState('');
  const [codeExpiry, setCodeExpiry] = useState(0);
  const [codeRemaining, setCodeRemaining] = useState(0);
  const [err, setErr]           = useState('');
  const [busy, setBusy]         = useState(false);
  const { status, kind }        = usePin();

  useEffect(() => {
    api('/lockers').then(l => { setLockers(l); setLockerId(l[0]?._id || ''); });
  }, []);

  // When a 6-digit emergency PIN is activated, clear the 2-digit pending banner
  useEffect(() => {
    if (status === 'active' && kind === 'emergency') {
      setCode('');
    }
  }, [status, kind]);

  // 5-minute countdown for 2-digit verification code
  useEffect(() => {
    if (!code || !codeExpiry) return;
    const tick = () => {
      const rem = Math.max(0, codeExpiry - performance.now());
      setCodeRemaining(rem);
      if (rem === 0) setCode('');
    };
    tick();
    const t = setInterval(tick, 500);
    return () => clearInterval(t);
  }, [code, codeExpiry]);

  const submit = async e => {
    e.preventDefault(); setErr(''); setBusy(true);
    try {
      const res = await api('/pins/emergency', { method: 'POST', body: { lockerId, password } });
      setPassword('');
      if (res.code) {
        setCode(res.code);
        setCodeExpiry(performance.now() + (res.ttlSec || 300) * 1000);
      }
    } catch (x) { setErr(x.message); }
    finally { setBusy(false); }
  };

  return (
    <section>
      <h2>Emergency Access</h2>
      <p className="hint">Lost your RFID card? Authorize emergency access to unlock the locker.</p>

      {code ? (
        <div className="emergency-code-box">
          <div className="code-header">
            <span className="badge">STEP 1 OF 2</span>
            <span className="timer-badge">⏱ {mmss(codeRemaining)}</span>
          </div>
          <p className="code-title">Your 2-Digit Verification Code:</p>
          <div className="code-display">{code}</div>
          <ol className="code-steps">
            <li>Go to the locker keypad and press <kbd>A</kbd></li>
            <li>Enter your 2-digit code <kbd>{code}</kbd> and press <kbd>#</kbd></li>
            <li>Once verified, your <strong>6-digit emergency PIN</strong> will appear here!</li>
          </ol>
          <button type="button" className="link" onClick={() => setCode('')} style={{ marginTop: '10px' }}>
            Cancel Request
          </button>
        </div>
      ) : (
        <form onSubmit={submit} className="stack">
          <p className="hint emrg-how">
            ⚠️ Press <kbd>A</kbd> at the locker and enter your unique 2-digit code to generate a 6-digit PIN.
            A 2-minute cooldown applies after each request.
          </p>
          <label>Locker
            <select value={lockerId} onChange={e => setLockerId(e.target.value)}>
              {lockers.map(l => <option key={l._id} value={l._id}>{l.label}</option>)}
            </select>
          </label>
          <label>Your Password
            <input type="password" value={password} onChange={e => setPassword(e.target.value)} required />
          </label>
          {err && <p className="err" role="alert">{err}</p>}
          <button className="danger" disabled={busy || !lockerId}>
            {busy ? 'Requesting…' : 'Generate 2-Digit Emergency Code'}
          </button>
        </form>
      )}
    </section>
  );
}

/* ====================================================================
   MY CARDS
   ==================================================================== */
function Cards() {
  const [cards, setCards] = useState([]);
  const load = () => api('/cards').then(setCards);
  useEffect(() => { load(); }, []);

  const lost = async c => {
    if (confirm(`Block card ${c.uid}? It will stop working immediately.`)) {
      await api(`/cards/${c._id}/report-lost`, { method: 'POST' });
      load();
    }
  };

  return (
    <section>
      <h2>My Cards</h2>
      {cards.length === 0 && <p className="hint">No cards are linked to your account.</p>}
      <ul className="rows">
        {cards.map(c => (
          <li key={c._id}>
            <code>{c.uid}</code>
            {c.label && <span className="hint">{c.label}</span>}
            <span className={'tag ' + c.status}>{c.status}</span>
            {c.status === 'active' && (
              <button className="link" onClick={() => lost(c)}>Report lost</button>
            )}
          </li>
        ))}
      </ul>
    </section>
  );
}

/* ====================================================================
   RECENT ACTIVITY LOG
   ==================================================================== */
function Logs() {
  const [logs, setLogs] = useState([]);
  const { status } = usePin();
  useEffect(() => { api('/logs').then(setLogs); }, [status]);

  const label = {
    scan_ok:          'Card accepted',
    scan_denied:      'Card refused',
    pin_ok:           'Locker opened',
    pin_fail:         'Wrong PIN entered',
    lockout:          'Keypad locked out',
    emergency_issued: '🚨 Emergency PIN issued',
    card_lost:        'Card reported lost',
  };

  return (
    <section>
      <h2>Recent Activity</h2>
      {logs.length === 0 && <p className="hint">Nothing yet.</p>}
      <ul className="rows">
        {logs.map(l => (
          <li key={l._id}>
            <span>{label[l.event] || l.event}</span>
            <span className="hint">{l.locker?.label}</span>
            <time className="hint">{new Date(l.createdAt).toLocaleString()}</time>
          </li>
        ))}
      </ul>
    </section>
  );
}

/* ====================================================================
   PROFILE SETTINGS  – update email and/or password
   ==================================================================== */
function ProfileSettings() {
  const { user, refreshUser } = useAuth();
  const [open, setOpen] = useState(false);

  const [currentPassword,  setCurrentPassword]  = useState('');
  const [newEmail,         setNewEmail]          = useState('');
  const [newPassword,      setNewPassword]       = useState('');
  const [confirmPassword,  setConfirmPassword]   = useState('');
  const [err,  setErr]  = useState('');
  const [msg,  setMsg]  = useState('');
  const [busy, setBusy] = useState(false);

  const reset = () => {
    setCurrentPassword(''); setNewEmail(''); setNewPassword(''); setConfirmPassword('');
    setErr(''); setMsg('');
  };

  const submit = async e => {
    e.preventDefault(); setErr(''); setMsg('');
    if (newPassword && newPassword !== confirmPassword) {
      setErr('New passwords do not match.'); return;
    }
    if (!newEmail && !newPassword) {
      setErr('Please provide a new email or a new password.'); return;
    }
    setBusy(true);
    try {
      await api('/user/profile', {
        method: 'PUT',
        body: {
          currentPassword,
          newEmail:    newEmail    || undefined,
          newPassword: newPassword || undefined,
        },
      });
      setMsg('Profile updated successfully! Please log in again if you changed your email.');
      await refreshUser();
      reset();
    } catch (x) {
      setErr(x.message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <section>
      <div className="section-toggle" onClick={() => { setOpen(o => !o); reset(); }}>
        <h2>Profile Settings {open ? '▲' : '▼'}</h2>
        <span className="hint">Change your email or password</span>
      </div>

      {open && (
        <form onSubmit={submit} className="stack" style={{ marginTop: '12px' }}>
          <p className="hint">Current account: <strong>{user?.email}</strong></p>

          <label>Current Password <span className="hint">(required)</span>
            <input type="password" value={currentPassword} onChange={e => setCurrentPassword(e.target.value)} required />
          </label>

          <label>New Email <span className="hint">(leave blank to keep current)</span>
            <input type="email" value={newEmail} onChange={e => setNewEmail(e.target.value)}
              placeholder={user?.email} autoComplete="off" />
          </label>

          <label>New Password <span className="hint">(min 6 characters, leave blank to keep current)</span>
            <input type="password" value={newPassword} onChange={e => setNewPassword(e.target.value)}
              placeholder="New password..." autoComplete="new-password" />
          </label>

          <label>Confirm New Password
            <input type="password" value={confirmPassword} onChange={e => setConfirmPassword(e.target.value)}
              placeholder="Confirm new password..." autoComplete="new-password" />
          </label>

          {err && <p className="err" role="alert">{err}</p>}
          {msg && <p className="ok"  role="status">{msg}</p>}

          <div style={{ display: 'flex', gap: '10px' }}>
            <button type="submit" disabled={busy || !currentPassword}>
              {busy ? 'Saving…' : 'Save Changes'}
            </button>
            <button type="button" className="link" onClick={() => { setOpen(false); reset(); }}>
              Cancel
            </button>
          </div>
        </form>
      )}
    </section>
  );
}

/* ====================================================================
   DASHBOARD
   ==================================================================== */
function Dashboard() {
  const { user, logout } = useAuth();
  return (
    <PinProvider>
      <main>
        <header>
          <h1>Smart Locker</h1>
          <span>{user.name} <button className="link" onClick={logout}>Sign out</button></span>
        </header>

        <PinPanel />

        <div className="cols">
          <Emergency />
          <Cards />
        </div>

        <ProfileSettings />
        <Logs />
      </main>
    </PinProvider>
  );
}

/* ====================================================================
   GATE — decides login vs dashboard
   ==================================================================== */
function Gate() {
  const { user } = useAuth();
  if (user === undefined) return null;
  return user ? <Dashboard /> : <Login />;
}

export default function App() {
  return (
    <AuthProvider>
      <Gate />
    </AuthProvider>
  );
}
