// 数据库封装：libSQL 客户端，同一套 API 同时支持
//  - 本地文件 SQLite：DATABASE_URL=file:local.db （开发/自测）
//  - Turso 云端持久库：DATABASE_URL=libsql://xxx.turso.io + DATABASE_AUTH_TOKEN （生产）
// Koyeb 等容器文件系统是临时的，生产必须用 Turso，否则重新部署会丢数据。
const { createClient } = require("@libsql/client");

const url = process.env.DATABASE_URL || "file:local.db";
const authToken = process.env.DATABASE_AUTH_TOKEN || undefined;

const db = createClient({ url, authToken });

// 小工具：execute 返回 rows；带参数用 { sql, args }
async function run(sql, args = []) {
  return db.execute({ sql, args });
}
async function get(sql, args = []) {
  const r = await db.execute({ sql, args });
  return r.rows[0] || null;
}
async function all(sql, args = []) {
  const r = await db.execute({ sql, args });
  return r.rows;
}

module.exports = { db, run, get, all, DB_URL: url };
