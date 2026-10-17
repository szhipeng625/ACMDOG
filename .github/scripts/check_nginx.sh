#!/usr/bin/env bash
set -euo pipefail
WORK=$(mktemp -d)
trap "rm -rf $WORK" EXIT
mkdir -p "$WORK/le/live/www.acmdog.cn"
openssl req -x509 -nodes -newkey rsa:2048 -days 1 -subj /CN=ci.local -keyout "$WORK/le/live/www.acmdog.cn/privkey.pem" -out "$WORK/le/live/www.acmdog.cn/fullchain.pem" > /dev/null 2>&1
openssl dhparam -dsaparam -out "$WORK/le/ssl-dhparams.pem" 2048 > /dev/null 2>&1
: > "$WORK/le/options-ssl-nginx.conf"
docker run --rm -v "$PWD/nginx/nginx.conf:/etc/nginx/nginx.conf:ro" -v "$PWD/nginx/conf.d:/etc/nginx/conf.d:ro" -v "$WORK/le:/etc/letsencrypt:ro" nginx:alpine nginx -t
echo nginx config ok
