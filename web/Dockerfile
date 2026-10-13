# The dashboard in a container (decisions D28): build the static files with Node, serve them
# with nginx, which also forwards /api/ to the api service (nginx.conf).

FROM node:22.23.3-alpine3.24 AS build
WORKDIR /web
COPY package.json package-lock.json ./
RUN npm ci
COPY . .
RUN npm run build

FROM nginx:1.30.5-alpine
COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=build /web/dist /usr/share/nginx/html
EXPOSE 80
