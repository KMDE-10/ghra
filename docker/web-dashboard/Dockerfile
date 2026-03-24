FROM node:20-alpine AS build

WORKDIR /app
COPY src/package.json src/package-lock.json* ./
RUN npm install
COPY src/ .
RUN npm run build

FROM nginx:alpine
RUN apk add --no-cache apache2-utils
COPY --from=build /app/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 443
