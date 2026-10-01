'use strict';
/*
 * REMOTE CONSOLE untuk node ESP32 (PT Bekaert Indonesia)
 *
 * Node memanggil server ini lewat HTTPS keluar (tidak perlu port-forward / IP publik di sisi node):
 *   POST /device/sync        -> kirim log serial + status, terima perintah berikutnya
 *   POST /device/result      -> laporan hasil perintah
 *   GET  /device/firmware/ID -> unduh firmware .bin untuk OTA
 * Admin membuka halaman web (login) untuk melihat serial monitor, mengirim perintah, dan
 * mengunggah / men-deploy firmware.
 *
 * Konfigurasi lewat environment variable (lihat README.md):
 *   ADMIN_PASSWORD (min 10 karakter), DEVICE_API_KEY (min 16 karakter), ADMIN_USER, PORT,
 *   BASE_PATH, DATA_DIR, FAST_MS, SLOW_MS
 */
const express = require('express');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

// Muat file .env otomatis jika ada (Node.js 20.6+)
try { if (typeof process.loadEnvFile === 'function') process.loadEnvFile(); } catch (e) {}

const PORT = parseInt(process.env.PORT || '3000', 10);
const BASE_PATH = (process.env.BASE_PATH || '').replace(/\/+$/, '');
const DATA_DIR = path.resolve(process.env.DATA_DIR || path.join(__dirname, 'data'));
const ADMIN_USER = process.env.ADMIN_USER || 'admin';
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD || '';
const DEVICE_API_KEY = process.env.DEVICE_API_KEY || '';
const FAST_MS = parseInt(process.env.FAST_MS || '5000', 10);    // interval sync node saat ada yang menonton
const SLOW_MS = parseInt(process.env.SLOW_MS || '30000', 10);   // interval sync node saat tidak ada yang menonton
const VIEWER_ACTIVE_MS = 60 * 1000;
const MAX_LOGS_PER_NODE = 1500;
const MAX_FIRMWARE_BYTES = 3 * 1024 * 1024;
const MIN_FIRMWARE_BYTES = 100 * 1024;
const SESSION_TTL_MS = 12 * 60 * 60 * 1000;

if (ADMIN_PASSWORD.length < 10) {
  console.error('ADMIN_PASSWORD wajib diisi (minimal 10 karakter).');
  process.exit(1);
}
if (DEVICE_API_KEY.length < 16) {
  console.error('DEVICE_API_KEY wajib diisi (minimal 16 karakter).');
  process.exit(1);
}

const FW_DIR = path.join(DATA_DIR, 'firmware');
fs.mkdirSync(FW_DIR, { recursive: true });
const FW_META_FILE = path.join(DATA_DIR, 'firmware.json');

// ---------------------------------------------------------------- util
function sha(s) { return crypto.createHash('sha256').update(String(s)).digest(); }
function safeEqual(a, b) { return crypto.timingSafeEqual(sha(a), sha(b)); }

// ---------------------------------------------------------------- firmware
let firmware = [];
try { firmware = JSON.parse(fs.readFileSync(FW_META_FILE, 'utf8')); } catch (e) { firmware = []; }
function saveFirmwareMeta() { fs.writeFileSync(FW_META_FILE, JSON.stringify(firmware, null, 2)); }

// ---------------------------------------------------------------- node state (di memori)
const nodes = new Map();
function getNode(id) {
  if (!nodes.has(id)) {
    nodes.set(id, {
      id, lastSeen: 0, intervalMs: SLOW_MS, info: {}, boot: null, lastLineId: 0,
      logs: [], seq: 0, cmds: [], cmdSeq: 0, lastViewer: 0
    });
  }
  return nodes.get(id);
}
function pushLog(node, msg, ms, id) {
  node.seq += 1;
  node.logs.push({ seq: node.seq, id: id || 0, ms: ms || 0, msg: String(msg).slice(0, 300), t: Date.now() });
  if (node.logs.length > MAX_LOGS_PER_NODE) node.logs.splice(0, node.logs.length - MAX_LOGS_PER_NODE);
}
function isOnline(n) { return n.lastSeen && (Date.now() - n.lastSeen) < (n.intervalMs * 2.5 + 5000); }

// ---------------------------------------------------------------- sesi admin
const sessions = new Map();
const loginAttempts = new Map();
setInterval(() => {
  const now = Date.now();
  for (const [t, s] of sessions) if (s.exp < now) sessions.delete(t);
  for (const [ip, a] of loginAttempts) if (a.resetAt < now) loginAttempts.delete(ip);
}, 60 * 1000).unref();

function getSession(req) {
  const m = /(?:^|;\s*)rc_session=([a-f0-9]{64})/.exec(req.headers.cookie || '');
  if (!m) return null;
  const s = sessions.get(m[1]);
  if (!s || s.exp < Date.now()) return null;
  return s;
}
function requireAdmin(req, res, next) {
  const s = getSession(req);
  if (!s) return res.status(401).json({ error: 'Belum login' });
  if (req.method !== 'GET' && req.headers['x-requested-with'] !== 'rc') {
    return res.status(403).json({ error: 'Header X-Requested-With wajib' });
  }
  req.session = s;
  next();
}
function requireDevice(req, res, next) {
  const k = req.headers['x-api-key'];
  if (typeof k !== 'string' || !safeEqual(k, DEVICE_API_KEY)) return res.status(401).json({ error: 'API key salah' });
  next();
}

// ---------------------------------------------------------------- app
const app = express();
app.set('trust proxy', 1);
app.disable('x-powered-by');
const router = express.Router();

router.use((req, res, next) => {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('X-Frame-Options', 'DENY');
  res.setHeader('Referrer-Policy', 'no-referrer');
  res.setHeader('Content-Security-Policy',
    "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'");
  if (req.path.startsWith('/api/') || req.path.startsWith('/device/')) res.setHeader('Cache-Control', 'no-store');
  next();
});

// ---------------- sisi device (node ESP32)
router.post('/device/sync', requireDevice, express.json({ limit: '256kb' }), (req, res) => {
  const b = req.body || {};
  const id = parseInt(b.node_id, 10);
  if (!Number.isInteger(id) || id < 1 || id > 999) return res.status(400).json({ error: 'node_id tidak valid' });
  const node = getNode(id);
  const now = Date.now();

  // Deteksi restart node (boot id berubah) agar dedup id baris log tidak salah
  if (b.boot !== undefined && String(b.boot) !== String(node.boot)) {
    if (node.boot !== null) pushLog(node, '--- [SERVER] node restart terdeteksi (boot baru) ---', 0, 0);
    node.boot = String(b.boot);
    node.lastLineId = 0;
  }

  if (Array.isArray(b.logs)) {
    for (const l of b.logs.slice(0, 100)) {
      if (!l || typeof l.msg !== 'string') continue;
      const lid = parseInt(l.id, 10) || 0;
      if (lid && lid <= node.lastLineId) continue;           // sudah pernah diterima
      pushLog(node, l.msg, parseInt(l.ms, 10) || 0, lid);
      if (lid > node.lastLineId) node.lastLineId = lid;
    }
  }

  node.lastSeen = now;
  node.info = {
    fw: String(b.fw || '').slice(0, 60), ip: String(b.ip || '').slice(0, 40),
    rssi: Number(b.rssi) || 0, uptime_s: Number(b.uptime_s) || 0, heap: Number(b.heap) || 0,
    mpu: !!b.mpu, rtd: !!b.rtd
  };

  // perintah kedaluwarsa (terkirim tapi tidak pernah melapor)
  for (const c of node.cmds) {
    if (c.status === 'sent' && now - c.sentAt > 10 * 60 * 1000) { c.status = 'timeout'; c.message = 'Tidak ada laporan dari node'; }
  }

  const viewerActive = (now - node.lastViewer) < VIEWER_ACTIVE_MS;
  node.intervalMs = viewerActive ? FAST_MS : SLOW_MS;

  let cmd = null;
  const next = node.cmds.find(c => c.status === 'queued');
  if (next) {
    next.status = 'sent';
    next.sentAt = now;
    cmd = { id: next.id, type: next.type };
    if (next.type === 'ota') {
      cmd.fw_id = next.fw_id; cmd.md5 = next.md5; cmd.size = next.size;
    }
  }
  res.json({ ok: true, interval_ms: node.intervalMs, cmd });
});

router.post('/device/result', requireDevice, express.json({ limit: '32kb' }), (req, res) => {
  const b = req.body || {};
  const id = parseInt(b.node_id, 10);
  const node = nodes.get(id);
  if (!node) return res.status(404).json({ error: 'node tidak dikenal' });
  const c = node.cmds.find(x => x.id === parseInt(b.cmd_id, 10));
  if (!c) return res.status(404).json({ error: 'perintah tidak ditemukan' });
  const st = String(b.status);
  if (!['ok', 'error', 'progress'].includes(st)) return res.status(400).json({ error: 'status tidak valid' });
  c.status = st === 'progress' ? 'running' : st;
  c.message = String(b.message || '').slice(0, 300);
  c.doneAt = (st === 'progress') ? null : Date.now();
  res.json({ ok: true });
});

router.get('/device/firmware/:id', requireDevice, (req, res) => {
  const f = firmware.find(x => x.id === req.params.id);
  if (!f) return res.status(404).json({ error: 'firmware tidak ada' });
  const file = path.join(FW_DIR, f.id + '.bin');
  if (!fs.existsSync(file)) return res.status(404).json({ error: 'file hilang' });
  res.setHeader('Content-Type', 'application/octet-stream');
  res.setHeader('Content-Length', String(f.size));
  res.setHeader('X-MD5', f.md5);
  fs.createReadStream(file).pipe(res);
});

// ---------------- sisi admin
router.post('/api/login', express.json({ limit: '4kb' }), (req, res) => {
  const ip = req.ip || 'x';
  const now = Date.now();
  const a = loginAttempts.get(ip) || { count: 0, resetAt: now + 15 * 60 * 1000 };
  if (a.resetAt < now) { a.count = 0; a.resetAt = now + 15 * 60 * 1000; }
  if (a.count >= 5) return res.status(429).json({ error: 'Terlalu banyak percobaan. Coba lagi nanti.' });

  const { user, password } = req.body || {};
  const ok = typeof user === 'string' && typeof password === 'string' &&
             safeEqual(user, ADMIN_USER) && safeEqual(password, ADMIN_PASSWORD);
  if (!ok) {
    a.count += 1; loginAttempts.set(ip, a);
    return res.status(401).json({ error: 'User atau password salah' });
  }
  loginAttempts.delete(ip);
  const token = crypto.randomBytes(32).toString('hex');
  sessions.set(token, { exp: now + SESSION_TTL_MS, user: ADMIN_USER });
  const secure = req.secure ? '; Secure' : '';
  res.setHeader('Set-Cookie', `rc_session=${token}; HttpOnly; SameSite=Strict; Path=/; Max-Age=${SESSION_TTL_MS / 1000}${secure}`);
  res.json({ ok: true });
});

router.post('/api/logout', requireAdmin, (req, res) => {
  const m = /(?:^|;\s*)rc_session=([a-f0-9]{64})/.exec(req.headers.cookie || '');
  if (m) sessions.delete(m[1]);
  res.setHeader('Set-Cookie', 'rc_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0');
  res.json({ ok: true });
});

router.get('/api/me', requireAdmin, (req, res) => res.json({ user: req.session.user, fast_ms: FAST_MS, slow_ms: SLOW_MS }));

router.get('/api/nodes', requireAdmin, (req, res) => {
  const list = [...nodes.values()].sort((a, b) => a.id - b.id).map(n => ({
    id: n.id, online: !!isOnline(n), last_seen: n.lastSeen, interval_ms: n.intervalMs, info: n.info,
    pending: n.cmds.filter(c => c.status === 'queued' || c.status === 'sent' || c.status === 'running').length
  }));
  res.json({ nodes: list, server_time: Date.now() });
});

router.get('/api/logs', requireAdmin, (req, res) => {
  const node = nodes.get(parseInt(req.query.node_id, 10));
  if (!node) return res.json({ lines: [], last_seq: 0 });
  node.lastViewer = Date.now();                       // menandakan ada yang menonton -> node sync cepat
  const since = parseInt(req.query.since, 10) || 0;
  let lines = node.logs.filter(l => l.seq > since);
  if (since === 0) lines = lines.slice(-300);
  else lines = lines.slice(0, 500);
  res.json({ lines, last_seq: node.seq });
});

router.get('/api/commands', requireAdmin, (req, res) => {
  const node = nodes.get(parseInt(req.query.node_id, 10));
  if (!node) return res.json({ commands: [] });
  node.lastViewer = Date.now();
  res.json({ commands: node.cmds.slice(-20).reverse() });
});

router.post('/api/command', requireAdmin, express.json({ limit: '4kb' }), (req, res) => {
  const { node_id, type, fw_id } = req.body || {};
  const id = parseInt(node_id, 10);
  const node = nodes.get(id);
  if (!node) return res.status(404).json({ error: 'Node belum pernah terhubung' });
  if (!['restart', 'send_now', 'ota'].includes(type)) return res.status(400).json({ error: 'Tipe perintah tidak dikenal' });
  if (node.cmds.filter(c => c.status === 'queued').length >= 5) return res.status(429).json({ error: 'Antrian perintah penuh' });

  const cmd = { id: ++node.cmdSeq, type, status: 'queued', createdAt: Date.now(), message: '' };
  if (type === 'ota') {
    const f = firmware.find(x => x.id === fw_id);
    if (!f) return res.status(404).json({ error: 'Firmware tidak ditemukan' });
    if (node.cmds.some(c => c.type === 'ota' && ['queued', 'sent', 'running'].includes(c.status))) {
      return res.status(409).json({ error: 'Masih ada OTA yang berjalan / menunggu untuk node ini' });
    }
    cmd.fw_id = f.id; cmd.md5 = f.md5; cmd.size = f.size; cmd.fw_name = f.name;
  }
  node.cmds.push(cmd);
  if (node.cmds.length > 100) node.cmds.splice(0, node.cmds.length - 100);
  pushLog(node, `--- [SERVER] perintah "${type}" diantrikan (#${cmd.id}) ---`, 0, 0);
  res.json({ ok: true, command: cmd });
});

router.delete('/api/command/:node/:id', requireAdmin, (req, res) => {
  const node = nodes.get(parseInt(req.params.node, 10));
  const c = node && node.cmds.find(x => x.id === parseInt(req.params.id, 10));
  if (!c) return res.status(404).json({ error: 'Tidak ditemukan' });
  if (c.status !== 'queued') return res.status(409).json({ error: 'Hanya perintah yang masih antre yang bisa dibatalkan' });
  c.status = 'cancelled'; c.message = 'Dibatalkan admin';
  res.json({ ok: true });
});

router.get('/api/firmware', requireAdmin, (req, res) => res.json({ firmware: [...firmware].reverse() }));

router.post('/api/firmware', requireAdmin, express.raw({ type: () => true, limit: MAX_FIRMWARE_BYTES }), (req, res) => {
  const buf = req.body;
  if (!Buffer.isBuffer(buf) || buf.length < MIN_FIRMWARE_BYTES) return res.status(400).json({ error: 'File terlalu kecil / kosong' });
  if (buf.length > MAX_FIRMWARE_BYTES) return res.status(413).json({ error: 'File terlalu besar' });
  if (buf[0] !== 0xE9) return res.status(400).json({ error: 'Bukan image firmware ESP32 (byte pertama harus 0xE9). Gunakan file .bin hasil Export Compiled Binary yang TANPA "bootloader"/"partitions".' });

  const md5 = crypto.createHash('md5').update(buf).digest('hex');
  const id = md5.slice(0, 12);
  const name = String(req.query.name || 'firmware.bin').replace(/[^\w.\- ]/g, '_').slice(0, 80);
  let f = firmware.find(x => x.id === id);
  if (!f) {
    fs.writeFileSync(path.join(FW_DIR, id + '.bin'), buf);
    f = { id, name, size: buf.length, md5, uploadedAt: Date.now() };
    firmware.push(f);
    if (firmware.length > 20) {                          // simpan maksimal 20 firmware terakhir
      const old = firmware.shift();
      try { fs.unlinkSync(path.join(FW_DIR, old.id + '.bin')); } catch (e) { /* abaikan */ }
    }
    saveFirmwareMeta();
  }
  res.json({ ok: true, firmware: f });
});

router.delete('/api/firmware/:id', requireAdmin, (req, res) => {
  const i = firmware.findIndex(x => x.id === req.params.id);
  if (i < 0) return res.status(404).json({ error: 'Tidak ditemukan' });
  const [f] = firmware.splice(i, 1);
  try { fs.unlinkSync(path.join(FW_DIR, f.id + '.bin')); } catch (e) { /* abaikan */ }
  saveFirmwareMeta();
  res.json({ ok: true });
});

router.use(express.static(path.join(__dirname, 'public'), { index: 'index.html' }));
router.get('/', (req, res) => {
  const pub = path.join(__dirname, 'public', 'index.html');
  if (fs.existsSync(pub)) return res.sendFile(pub);
  res.sendFile(path.join(__dirname, 'index.html'));
});

// penanganan error (mis. JSON rusak / body terlalu besar)
router.use((err, req, res, next) => {
  const status = err.status || 500;
  res.status(status).json({ error: status === 413 ? 'Data terlalu besar' : (status < 500 ? 'Permintaan tidak valid' : 'Kesalahan server') });
});

if (BASE_PATH) app.get(BASE_PATH, (req, res) => res.redirect(BASE_PATH + '/'));
app.use(BASE_PATH || '/', router);

app.listen(PORT, () => {
  console.log(`Remote console berjalan di port ${PORT}${BASE_PATH ? ' (BASE_PATH=' + BASE_PATH + ')' : ''}`);
});
