FROM node:20-alpine
WORKDIR /app
COPY package*.json ./
RUN npm install --omit=dev
COPY . .
ENV NODE_ENV=production
EXPOSE 3000
# 启动时建表/补种子(幂等)，再起服务
CMD ["sh", "-c", "node init-db.js && node server.js"]
