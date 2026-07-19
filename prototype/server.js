/* Tiny static dev server for the CappedWeather Console prototype.
   Accepts --port/-p and --host (plus PORT env), defaults to 7100. */
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
function argVal(names, fallback) {
  for (let i = 0; i < args.length; i++) {
    if (names.includes(args[i]) && args[i + 1]) return args[i + 1];
    const eq = names.find(n => args[i].startsWith(n + '='));
    if (eq) return args[i].slice(eq.length + 1);
  }
  return fallback;
}
const port = parseInt(argVal(['--port', '-p'], process.env.PORT || '7100'), 10);
const host = argVal(['--host', '-h'], '127.0.0.1');

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.json': 'application/json',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
};

const root = __dirname;
http.createServer((req, res) => {
  let urlPath = decodeURIComponent(req.url.split('?')[0]);
  if (urlPath === '/') urlPath = '/index.html';
  const file = path.normalize(path.join(root, urlPath));
  if (!file.startsWith(root)) { res.writeHead(403); res.end(); return; }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
    res.end(data);
  });
}).listen(port, host, () => {
  console.log(`CappedWeather Console prototype → http://${host}:${port}/`);
});
