// Vercel serverless 入口：把 Express 应用作为函数处理器导出。
// 运行所需的密钥/数据库连接通过环境变量提供(Vercel 项目设置或部署时注入)。
module.exports = require("../server.js");
