export function getToken() {
  return sessionStorage.getItem('smart_locker_token');
}

export function setToken(token) {
  if (token) sessionStorage.setItem('smart_locker_token', token);
  else sessionStorage.removeItem('smart_locker_token');
}

export async function api(path, { method = 'GET', body } = {}) {
  const token = getToken();
  const headers = { 'Content-Type': 'application/json' };
  if (token) headers['Authorization'] = `Bearer ${token}`;

  const r = await fetch('/api' + path, {
    method, credentials: 'include',
    headers,
    body: body ? JSON.stringify(body) : undefined,
  });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.error || 'Request failed');
  return data;
}

