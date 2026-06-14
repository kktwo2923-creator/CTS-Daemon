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
    user_note     TEXT,                       -- 用户激活时填写的备注,供后台查看
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

  // 套餐(售卖的价格/时长，后台可改)
  await run(`CREATE TABLE IF NOT EXISTS plans (
    code        TEXT PRIMARY KEY,           -- 套餐代码
    name        TEXT NOT NULL,              -- 显示名(月卡/季卡/永久…)
    days        INTEGER NOT NULL DEFAULT 30,-- 时长天数，0=永久
    price       TEXT NOT NULL DEFAULT '0.00',-- 售价(元)
    enabled     INTEGER NOT NULL DEFAULT 1, -- 是否上架
    sort        INTEGER NOT NULL DEFAULT 0,
    prefix      TEXT DEFAULT 'VPRO'
  )`);

  // 订单(易支付自动发卡)
  await run(`CREATE TABLE IF NOT EXISTS orders (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    out_trade_no TEXT UNIQUE NOT NULL,      -- 本系统订单号
    plan_code    TEXT,
    days         INTEGER DEFAULT 30,
    money        TEXT,                       -- 金额(元)
    pay_type     TEXT,                       -- alipay / wxpay
    trade_no     TEXT,                       -- 支付平台流水号
    status       INTEGER DEFAULT 0,          -- 0待支付/待确认 1已支付已发卡
    license_key  TEXT,                       -- 发出的卡密
    contact      TEXT,                        -- 买家联系方式(半手动模式)
    note         TEXT,                        -- 买家提交订单时填写的备注,供后台查看
    created_at    TEXT DEFAULT (datetime('now')),
    paid_at      TEXT
  )`);
  await run(`CREATE INDEX IF NOT EXISTS idx_ord_status ON orders(status)`);
  try { await run(`ALTER TABLE orders ADD COLUMN contact TEXT`); } catch (_) { /* 旧库补列，已存在则忽略 */ }
  try { await run(`ALTER TABLE orders ADD COLUMN note TEXT`); } catch (_) { /* 旧库补列，已存在则忽略 */ }
  try { await run(`ALTER TABLE license_keys ADD COLUMN user_note TEXT`); } catch (_) { /* 旧库补列，已存在则忽略 */ }

  // 默认产品
  const prod = await get(`SELECT product_id FROM products WHERE product_id='PROD001'`);
  if (!prod) {
    await run(
      `INSERT INTO products(product_id, product_name, version) VALUES('PROD001','OnePlus V Pro','1.0.0')`
    );
  }

  // 默认套餐（价格仅为示例，请在后台“套餐价格”里改成你的实际售价）
  const anyPlan = await get(`SELECT code FROM plans LIMIT 1`);
  if (!anyPlan) {
    const seed = [
      ["yue", "月卡(30天)", 30, "9.90", 1],
      ["ji", "季卡(90天)", 90, "25.00", 2],
      ["nian", "年卡(365天)", 365, "88.00", 3],
      ["forever", "永久", 0, "168.00", 4],
    ];
    for (const [code, name, days, price, sort] of seed) {
      await run(`INSERT INTO plans(code, name, days, price, enabled, sort) VALUES(?,?,?,?,1,?)`, [code, name, days, price, sort]);
    }
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
