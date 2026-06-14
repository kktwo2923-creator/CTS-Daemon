// 卡密授权服务端：验证/生成/管理 + 网页后台。详见 README。
const path = require("path");
const crypto = require("crypto");
const express = require("express");
const bcrypt = require("bcryptjs");
const jwt = require("jsonwebtoken");
const rateLimit = require("express-rate-limit");
const { run, get, all } = require("./db");

const app = express();
app.set("trust proxy", 1); // Koyeb/Render 在反代后，限流取真实 IP
app.use(express.json());
app.use(express.static(path.join(__dirname, "public")));

const PORT = process.env.PORT || 3000;
const JWT_SECRET = process.env.JWT_SECRET || crypto.randomBytes(32).toString("hex");
// 令牌签名密钥：服务端签发、守护端离线校验时需用同一个值(后续烧进守护)。务必设置并保密。
const SIGNING_SECRET = process.env.LICENSE_SIGNING_SECRET || "CHANGE_ME_license_signing_secret";

const sha256 = (s) => crypto.createHash("sha256").update(s).digest("hex");

// HMAC 令牌：绑定 卡密|设备|到期 三要素，防止本地伪造 license 文件
function signToken(licenseKey, deviceId, expiresAt) {
  return crypto
    .createHmac("sha256", SIGNING_SECRET)
    .update(`${licenseKey}|${deviceId}|${expiresAt || "0"}`)
    .digest("hex");
}

// ---------- 鉴权 ----------
// 校验 X-API-Key：返回该 key 的权限集合；无效返回 null
async function apiKeyPerms(req) {
  const key = req.header("X-API-Key");
  if (!key) return null;
  const row = await get(`SELECT permissions, enabled FROM api_keys WHERE key_hash=?`, [sha256(key)]);
  if (!row || row.enabled !== 1) return null;
  return String(row.permissions || "").split(",").map((s) => s.trim());
}

// 管理鉴权：接受 管理员 JWT(Authorization: Bearer) 或 拥有 manage 权限的 API Key
function adminOrApiKey(perm) {
  return async (req, res, next) => {
    const authz = req.header("Authorization") || "";
    if (authz.startsWith("Bearer ")) {
      try {
        req.admin = jwt.verify(authz.slice(7), JWT_SECRET);
        return next();
      } catch (_) { /* 落到 API Key */ }
    }
    const perms = await apiKeyPerms(req);
    if (perms && (perms.includes(perm) || perms.includes("manage"))) return next();
    return res.status(401).json({ code: 401, success: false, message: "未授权" });
  };
}

// 仅 API Key(用于客户端 verify)
function requireApiKey(perm) {
  return async (req, res, next) => {
    const perms = await apiKeyPerms(req);
    if (perms && perms.includes(perm)) return next();
    return res.status(401).json({ code: 401, success: false, message: "API 密钥无效或权限不足" });
  };
}

// ---------- 限流 ----------
const verifyLimiter = rateLimit({ windowMs: 5 * 60 * 1000, max: 30, standardHeaders: true, legacyHeaders: false });
const apiLimiter = rateLimit({ windowMs: 15 * 60 * 1000, max: 100, standardHeaders: true, legacyHeaders: false });

// ---------- 工具 ----------
function genKey(prefix) {
  const seg = () => crypto.randomBytes(2).toString("hex").toUpperCase();
  return `${prefix}-${seg()}${seg()}-${seg()}${seg()}-${seg()}${seg()}-${seg()}${seg()}`;
}
function nowIso() { return new Date().toISOString(); }
function addDays(days) { return new Date(Date.now() + days * 86400000).toISOString(); }
function remainingDays(expiresAt) {
  if (!expiresAt) return -1; // 永久
  return Math.max(0, Math.ceil((new Date(expiresAt).getTime() - Date.now()) / 86400000));
}

// ---------- 健康检查 ----------
app.get("/api/health", (_req, res) => res.json({ code: 200, success: true, message: "ok", time: nowIso() }));

// ---------- 管理员登录 ----------
app.post("/api/admin/login", apiLimiter, async (req, res) => {
  const { username, password } = req.body || {};
  const row = await get(`SELECT username, password_hash FROM admins WHERE username=?`, [username || ""]);
  if (!row || !bcrypt.compareSync(password || "", row.password_hash)) {
    return res.status(401).json({ code: 401, success: false, message: "账号或密码错误" });
  }
  const token = jwt.sign({ username: row.username }, JWT_SECRET, { expiresIn: "7d" });
  res.json({ code: 200, success: true, token });
});

// 修改管理员密码
app.post("/api/admin/password", apiLimiter, adminOrApiKey("manage"), async (req, res) => {
  const { username, new_password } = req.body || {};
  const u = username || (req.admin && req.admin.username);
  if (!u || !new_password) return res.status(400).json({ code: 400, success: false, message: "缺少参数" });
  await run(`UPDATE admins SET password_hash=? WHERE username=?`, [bcrypt.hashSync(new_password, 10), u]);
  res.json({ code: 200, success: true, message: "已修改" });
});

// ---------- 卡密验证(客户端) ----------
app.post("/api/verify", verifyLimiter, requireApiKey("verify"), async (req, res) => {
  const { license_key, device_id, device_info } = req.body || {};
  const ip = req.ip;
  const log = (result) => run(
    `INSERT INTO verify_logs(license_key, device_id, ip, result) VALUES(?,?,?,?)`,
    [license_key || "", device_id || "", ip || "", result]
  ).catch(() => {});

  if (!license_key || !device_id) {
    await log("缺少参数");
    return res.status(400).json({ code: 400, success: false, message: "缺少 license_key 或 device_id" });
  }

  const row = await get(`SELECT * FROM license_keys WHERE license_key=?`, [license_key]);
  if (!row) { await log("卡密不存在"); return res.json({ code: 404, success: false, message: "卡密不存在" }); }
  if (row.status === 3) { await log("已禁用"); return res.json({ code: 403, success: false, message: "卡密已被禁用" }); }

  let { status, device_id: boundDev, expires_at, duration_days } = row;

  // 首次激活：绑定设备 + 计算到期
  if (status === 0) {
    expires_at = Number(duration_days) === 0 ? null : addDays(Number(duration_days));
    await run(
      `UPDATE license_keys SET status=1, device_id=?, device_info=?, activated_at=?, expires_at=? WHERE id=?`,
      [device_id, device_info || "", nowIso(), expires_at, row.id]
    );
    boundDev = device_id; status = 1;
  } else if (status === 1) {
    if (boundDev && boundDev !== device_id) {
      await log("设备不匹配");
      return res.json({ code: 409, success: false, message: "卡密已绑定其它设备，请联系管理员解绑" });
    }
    if (expires_at && Date.now() > new Date(expires_at).getTime()) {
      await run(`UPDATE license_keys SET status=2 WHERE id=?`, [row.id]);
      await log("已过期");
      return res.json({ code: 410, success: false, message: "卡密已过期" });
    }
  } else if (status === 2) {
    await log("已过期");
    return res.json({ code: 410, success: false, message: "卡密已过期" });
  }

  await log("成功");
  const prod = await get(`SELECT product_name, version FROM products WHERE product_id=?`, [row.product_id]);
  res.json({
    code: 200, success: true, message: "验证成功",
    data: {
      license_key,
      device_id: boundDev,
      product_name: prod ? prod.product_name : "OnePlus V Pro",
      product_version: prod ? prod.version : "1.0.0",
      activated_at: row.activated_at || nowIso(),
      expires_at: expires_at || null,
      remaining_days: remainingDays(expires_at),
      token: signToken(license_key, boundDev, expires_at), // 守护端离线校验用
    },
  });
});

// ---------- 生成卡密 ----------
app.post("/api/license/generate", apiLimiter, adminOrApiKey("generate"), async (req, res) => {
  let { count, duration_days, product_id, prefix, note } = req.body || {};
  count = Math.min(Math.max(parseInt(count) || 1, 1), 1000);
  duration_days = parseInt(duration_days); if (isNaN(duration_days)) duration_days = 30;
  product_id = product_id || "PROD001";
  prefix = (prefix || "KEY").replace(/[^A-Za-z0-9]/g, "").slice(0, 8) || "KEY";

  const keys = [];
  for (let i = 0; i < count; i++) {
    const k = genKey(prefix);
    await run(
      `INSERT INTO license_keys(license_key, product_id, status, duration_days, note) VALUES(?,?,0,?,?)`,
      [k, product_id, duration_days, note || ""]
    );
    keys.push(k);
  }
  res.json({ code: 200, success: true, message: `已生成 ${keys.length} 个`, data: { keys, duration_days, product_id } });
});

// ---------- 卡密列表 ----------
app.get("/api/license/list", apiLimiter, adminOrApiKey("manage"), async (req, res) => {
  const status = req.query.status;
  const page = Math.max(parseInt(req.query.page) || 1, 1);
  const limit = Math.min(Math.max(parseInt(req.query.limit) || 50, 1), 500);
  const offset = (page - 1) * limit;
  let where = ""; const args = [];
  if (status !== undefined && status !== "") { where = "WHERE status=?"; args.push(parseInt(status)); }
  const rows = await all(
    `SELECT id, license_key, status, duration_days, device_id, device_info, activated_at, expires_at, note, created_at
     FROM license_keys ${where} ORDER BY id DESC LIMIT ? OFFSET ?`, [...args, limit, offset]
  );
  const total = await get(`SELECT COUNT(*) AS c FROM license_keys ${where}`, args);
  res.json({ code: 200, success: true, data: { list: rows, total: total ? total.c : 0, page, limit } });
});

// ---------- 禁用 / 启用 / 解绑 / 删除 ----------
app.post("/api/license/disable", apiLimiter, adminOrApiKey("manage"), async (req, res) => {
  const { license_key } = req.body || {};
  await run(`UPDATE license_keys SET status=3 WHERE license_key=?`, [license_key || ""]);
  res.json({ code: 200, success: true, message: "已禁用" });
});
app.post("/api/license/enable", apiLimiter, adminOrApiKey("manage"), async (req, res) => {
  // 启用：已绑定过的回到 1，未绑定的回到 0
  const { license_key } = req.body || {};
  const row = await get(`SELECT device_id FROM license_keys WHERE license_key=?`, [license_key || ""]);
  if (!row) return res.json({ code: 404, success: false, message: "不存在" });
  await run(`UPDATE license_keys SET status=? WHERE license_key=?`, [row.device_id ? 1 : 0, license_key]);
  res.json({ code: 200, success: true, message: "已启用" });
});
app.post("/api/license/unbind", apiLimiter, adminOrApiKey("manage"), async (req, res) => {
  const { license_key } = req.body || {};
  await run(`UPDATE license_keys SET device_id=NULL, device_info=NULL, status=0, activated_at=NULL, expires_at=NULL WHERE license_key=?`, [license_key || ""]);
  res.json({ code: 200, success: true, message: "已解绑(回到未使用，可重新激活绑定新设备)" });
});
app.post("/api/license/delete", apiLimiter, adminOrApiKey("manage"), async (req, res) => {
  const { license_key } = req.body || {};
  await run(`DELETE FROM license_keys WHERE license_key=?`, [license_key || ""]);
  res.json({ code: 200, success: true, message: "已删除" });
});

// ---------- 统计 ----------
app.get("/api/stats", apiLimiter, adminOrApiKey("manage"), async (_req, res) => {
  const s = await all(`SELECT status, COUNT(*) AS c FROM license_keys GROUP BY status`);
  const map = { unused: 0, active: 0, expired: 0, disabled: 0 };
  for (const r of s) {
    if (r.status === 0) map.unused = r.c;
    else if (r.status === 1) map.active = r.c;
    else if (r.status === 2) map.expired = r.c;
    else if (r.status === 3) map.disabled = r.c;
  }
  const total = await get(`SELECT COUNT(*) AS c FROM license_keys`);
  const recent = await all(`SELECT license_key, device_id, ip, result, created_at FROM verify_logs ORDER BY id DESC LIMIT 20`);
  res.json({ code: 200, success: true, data: { total: total ? total.c : 0, ...map, recent } });
});

app.listen(PORT, () => console.log(`卡密服务已启动 :${PORT}  DB=${require("./db").DB_URL}`));
