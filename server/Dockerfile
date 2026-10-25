FROM node:20-alpine
WORKDIR /app
COPY relay.js package.json ./
ENV CARMA2_PORT=8080
EXPOSE 8080
USER node
CMD ["node", "relay.js"]
