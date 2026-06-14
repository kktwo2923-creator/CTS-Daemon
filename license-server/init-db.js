// 初始化数据库：建表 + 写入默认管理员、API 密钥、默认产品。可重复执行（幂等）。
const crypto = require("crypto");
const bcrypt = require("bcryptjs");
const { run, get } = require("./db");

function sha256(s) {
  return crypto.createHash("sha256").update(s).digest("hex");
}

async function main() {
  await run(`CREATE TABLE IF NOT EXISTS products (
    product_id   TEXT PRIMARY KEY,
    product_name TEXT NOT NULL,
    version      TEXT DEFAULT '1.0.0',
    created_at    TEXT DEFAULT (datetime('now'))
  )`);

  await run(`CREATE TABLE IF NOT EXISTS license_keys (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    license_key   TEXT UNIQUE NOT NULL,
    product_id    TEXT DEFAULT 'PROD001',
    status        INTEGER DEFAULT 0,          -- 0未使用 1已激活 2已过期 3已禁用
    duration_days INTEGER DEFAULT 30,         -- 0 表示永久
    device_id     TEXT,                       -- 绑定的设备(ro.serialno)
    device_info   TEXT,
    activated_at  TEXT,
    expires_at    TEXT,
    note          TEXT,
    created_at    TEXT DEFAULT (datetime('now'))
  )`);

  await run(`CREATE TABLE IF NOT EXISTS api_keys (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    key_hash    TEXT UNIQUE NOT NULL,
    name        TEXT,
    permissions TEXT DEFAULT 'verify,generate,manage',
    enabled     INTEGER DEFAULT 1,
    created_at   TEXT DEFAULT (datetime('now'))
  )`);

  await run(`CREATE TABLE IF NOT EXISTS admins (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    created_at     TEXT DEFAULT (datetime('now'))
  )`);

  await run(`CREATE TABLE IF NOT EXISTS verify_logs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    license_key TEXT,
    device_id   TEXT,
    ip          TEXT,
    result      TEXT,
    created_at   TEXT DEFAULT (datetime('now'))
  )`);

  await run(`CREATE INDEX IF NOT EXISTS idx_lk_status ON license_keys(status)`);
  await run(`CREATE INDEX IF NOT EXISTS idx_lk_device ON license_keys(device_id)`);

  // 默认产品
  const prod = await get(`SELECT product_id FROM products WHERE product_id='PROD001'`);
  if (!prod) {
    await run(
      `INSERT INTO products(product_id, product_name, version) VALUES('PROD001','OnePlus V Pro','1.0.0')`
    );
  }

  // 默认管理员（用户名/密码可用环境变量覆盖；首登后请改密码）
  const adminUser = process.env.ADMIN_USER || "admin";
  const adminPass = process.env.ADMIN_PASSWORD || "admin123";
  const existAdmin = await get(`SELECT id FROM admins WHERE username=?`, [adminUser]);
  if (!existAdmin) {
    const hash = bcrypt.hashSync(adminPass, 10);
    await run(`INSERT INTO admins(username, password_hash) VALUES(?,?)`, [adminUser, hash]);
  }

  // API 密钥：环境变量 API_KEY 优先，否则随机生成；DB 只存哈希，明文仅本次打印
  let apiKeyPlain = process.env.API_KEY || null;
  const anyKey = await get(`SELECT id FROM api_keys LIMIT 1`);
  if (!anyKey) {
    if (!apiKeyPlain) apiKeyPlain = "SK_" + crypto.randomBytes(24).toString("hex");
    await run(`INSERT INTO api_keys(key_hash, name) VALUES(?,?)`, [sha256(apiKeyPlain), "default"]);
  }

  return { adminUser, adminPass, apiKeyPlain };
}

// 幂等初始化，供 serverless(Vercel) 冷启动按需调用
module.exports = { ensureInit: main };

// 直接 `node init-db.js` 运行时(Docker/CLI)：执行并打印凭据
if (require.main === module) {
  main().then(({ adminUser, adminPass, apiKeyPlain }) => {
    console.log("✅ 数据库初始化完成");
    console.log("📋 管理员账号: " + adminUser + " / " + adminPass + "  (请尽快修改)");
    if (apiKeyPlain) {
      console.log("🔑 API 密钥(仅显示一次，请保存): " + apiKeyPlain);
    } else {
      console.log("🔑 API 密钥已存在(哈希存储)。如忘记可重设环境变量 API_KEY 并清空 api_keys 表重建。");
    }
    process.exit(0);
  }).catch((e) => {
    console.error("初始化失败:", e);
    process.exit(1);
  });
}
