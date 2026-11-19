import { inputEvents, desktopInputEvents } from "./input.mjs";
import { streamAudio } from "./audio.mjs";
import http from "node:http";
import net from "node:net";
import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { randomBytes } from "node:crypto";
import { WebSocketServer, WebSocket } from "ws";
import { zipSync, strToU8 } from "fflate";
import { Libvirt, checkId } from "./libvirt.mjs";
import { Captures } from "./captures.mjs";
import { Groups } from "./groups.mjs";
import { encode, resize } from "./images.mjs";
import { PNG } from "pngjs";
import { ReadOnlyRfb } from "./rfb-filter.mjs";
export async function createViewer({
  libvirt = new Libvirt(),
  allowedHosts = (
    process.env.VIEWER_ALLOWED_HOSTS || "localhost,127.0.0.1,[::1]"
  )
    .split(",")
    .map((x) => x.trim()),
  root = process.env.VIEWER_DATA || "./data",
  dist = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../dist"),
} = {}) {
  const captures = new Captures(libvirt, root);
  await captures.init();
  const groups = new Groups(root);
  const body = (req) =>
    new Promise((resolve, reject) => {
      let text = "";
      req.setEncoding("utf8");
      req.on("data", (chunk) => {
        text += chunk;
        if (text.length > 16384) reject(Error("Request too large"));
      });
      req.on("end", () => {
        try {
          resolve(JSON.parse(text));
        } catch {
          reject(Error("Invalid JSON"));
        }
      });
      req.on("error", reject);
    });
  const sessions = new Map();
  const wss = new WebSocketServer({
    noServer: true,
    maxPayload: 16 * 1024 * 1024,
    handleProtocols: (protocols) =>
      protocols.has("binary") ? "binary" : false,
  });
  let sockets = 0;
  const perSession = new Map();
  function trusted(req) {
    try {
      const u = new URL("http://" + req.headers.host);
      return (
        !u.username &&
        !u.password &&
        allowedHosts.includes(u.hostname) &&
        req.headers["sec-fetch-site"] !== "cross-site" &&
        (!req.headers.origin ||
          req.headers.origin === "http://" + req.headers.host)
      );
    } catch {
      return false;
    }
  }
  function session(req) {
    const key = (req.headers.cookie || "")
      .split(";")
      .map((x) => x.trim())
      .find((x) => x.startsWith("vmwall_viewer="))
      ?.slice("vmwall_viewer=".length);
    const expires = sessions.get(key);
    if (expires > Date.now()) {
      sessions.set(key, Date.now() + 12 * 3600000);
      return key;
    }
    return null;
  }
  function headers(res, type = "application/json") {
    res.setHeader("Content-Type", type);
    res.setHeader("Cache-Control", "no-store");
    res.setHeader("X-Content-Type-Options", "nosniff");
    res.setHeader("Cross-Origin-Resource-Policy", "same-origin");
    res.setHeader(
      "Content-Security-Policy",
      "default-src 'self'; connect-src 'self' ws: wss:; img-src 'self' blob: data:; style-src 'self' 'unsafe-inline'; font-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'",
    );
  }
  const send = (res, status, value) => {
    res.statusCode = status;
    headers(res);
    res.end(JSON.stringify(value));
  };
  const server = http.createServer(async (req, res) => {
    if (!trusted(req))
      return send(res, 403, { error: "Use the configured viewer address" });
    const url = new URL(req.url, "http://localhost");
    try {
      if (!url.pathname.startsWith("/api/") && req.method === "GET") {
        if (!session(req)) {
          for (const [key, until] of sessions)
            if (until < Date.now()) sessions.delete(key);
          if (sessions.size >= 256)
            return send(res, 503, { error: "Session limit" });
          const key = randomBytes(32).toString("hex");
          sessions.set(key, Date.now() + 12 * 3600000);
          res.setHeader(
            "Set-Cookie",
            "vmwall_viewer=" +
              key +
              "; Path=/; HttpOnly; SameSite=Strict; Max-Age=604800",
          );
        }
        const target = url.pathname === "/" ? "/index.html" : url.pathname;
        if (!/^\/(?:index\.html|assets\/[A-Za-z0-9_.-]+)$/.test(target))
          return send(res, 404, { error: "Not found" });
        const file = path.join(dist, target);
        const type = target.endsWith(".js")
          ? "text/javascript"
          : target.endsWith(".css")
            ? "text/css"
            : "text/html";
        headers(res, type);
        res.end(await fs.readFile(file));
        return;
      }
      if (!session(req))
        return send(res, 401, {
          error: "Open the dashboard first",
        });
      if (url.pathname === "/api/vms" && req.method === "GET") {
        const vms = await libvirt.inventory();
        return send(res, 200, {
          vms: vms.map(({ endpoint, ...vm }) => ({
            ...vm,
            displays: Array.from({ length: vm.heads }, (_, head) => ({
              head,
              ...captures.heads.get(vm.id + ":" + head),
            })),
          })),
          transport: process.env.VIEWER_BIND
            ? "LAN viewer session"
            : "SSH-authenticated loopback service",
          capturedAt: new Date().toISOString(),
        });
      }
      if (url.pathname === "/api/groups" && req.method === "GET")
        return send(res, 200, { groups: await groups.list() });
      const input = url.pathname.match(/^\/api\/vms\/([^/]+)\/input$/);
      if (input) {
        if (req.method !== "POST") return send(res,405,{error:"Use POST"});
        if (!req.headers.origin) return send(res,403,{error:"Use the viewer controls"});
        const data = await body(req);
        const id=checkId(input[1]);
        let events=inputEvents(data.events);
        if(events.some(event=>event.type==="abs")) {
          const vm=await libvirt.get(id);
          if(!Number.isInteger(data.head)||data.head<0||data.head>=vm.heads)throw Error("Monitor unavailable");
          const displays=await Promise.all(Array.from({length:vm.heads},async(_,head)=>{
            const known=captures.heads.get(id+":"+head);
            if(known)return known;
            const frame=await captures.frame(id,head);
            return {width:frame.image.width,height:frame.image.height};
          }));
          events=desktopInputEvents(data.head,events,displays);
        }
        await libvirt.input(id, data.head, events);
        return send(res,200,{ok:true});
      }
      const group = url.pathname.match(/^\/api\/groups\/([^/]+)$/);
      if (group) {
        if (req.method !== "PUT") return send(res, 405, { error: "Use PUT" });
        // Writes must come from the wall's own page, never a bare request.
        if (!req.headers.origin)
          return send(res, 403, { error: "Use the wall to save groups" });
        return send(res, 200, {
          groups: await groups.save(
            decodeURIComponent(group[1]),
            (await body(req)).vms,
          ),
        });
      }
      if (url.pathname === "/api/audits" && req.method === "GET")
        return send(res, 200, {
          audits: (await captures.list(url.searchParams.get("vm"))).slice(
            0,
            40,
          ),
        });
      let match = url.pathname.match(
        /^\/api\/vms\/([^/]+)\/(preview\.png|heads\/([0-3])\.png|capture)$/,
      );
      if (match) {
        const id = checkId(match[1]);
        if (match[2] === "capture" && req.method === "POST")
          return send(res, 201, await captures.capture(id));
        if (req.method !== "GET")
          return send(res, 405, { error: "Method not allowed" });
        if (match[2] === "capture")
          return send(res, 405, { error: "Use POST" });
        const png =
          match[2] === "preview.png"
            ? await captures.preview(id, Math.max(128, Math.min(4096, Number(url.searchParams.get("width")) || 720)))
            : (await captures.frame(id, Number(match[3]))).png;
        headers(res, "image/png");
        res.end(png);
        return;
      }
      match = url.pathname.match(
        /^\/api\/audits\/([0-9a-f-]{36})\/(bundle\.zip|manifest\.json|combined\.png|preview\.png|head-[0-3]\.png)$/,
      );
      if (match && req.method === "GET") {
        const { folder, manifest } = await captures.files(match[1]);
        if (match[2] === "bundle.zip") {
          const files = {
            "manifest.json": strToU8(JSON.stringify(manifest, null, 2)),
          };
          for (const file of [
            "combined.png",
            ...manifest.displays.map((d) => d.file),
          ])
            files[file] = new Uint8Array(
              await fs.readFile(path.join(folder, file)),
            );
          headers(res, "application/zip");
          res.setHeader(
            "Content-Disposition",
            'attachment; filename="vm-audit-' + match[1] + '.zip"',
          );
          res.end(Buffer.from(zipSync(files, { level: 0 })));
        } else {
          if (match[2] === "preview.png") {
            try {
              await fs.access(path.join(folder, "preview.png"));
            } catch {
              await fs.writeFile(
                path.join(folder, "preview.png"),
                encode(
                  resize(
                    PNG.sync.read(
                      await fs.readFile(path.join(folder, "combined.png")),
                    ),
                    720,
                  ),
                ),
                { mode: 0o600 },
              );
            }
          }
          headers(
            res,
            match[2].endsWith(".png") ? "image/png" : "application/json",
          );
          res.end(await fs.readFile(path.join(folder, match[2])));
        }
        return;
      }
      return send(res, 404, { error: "Not found" });
    } catch (error) {
      console.error("Request failed:", url.pathname, error.message);
      return send(res, error.code === "ENOENT" ? 404 : 400, {
        error: error.message,
      });
    }
  });
  server.on("upgrade", async (req, socket, head) => {
    const reject = (code) =>
      socket.end("HTTP/1.1 " + code + " Rejected\r\nConnection: close\r\n\r\n");
    try {
      const key = session(req);
      if (!trusted(req) || !key || !req.headers.origin) return reject(403);
      const url = new URL(req.url, "http://localhost");
      const match = url.pathname.match(/^\/api\/vms\/([^/]+)\/(vnc|audio)$/);
      if (!match) return reject(404);
      if (sockets >= 24 || (perSession.get(key) || 0) >= 6) return reject(429);
      const id = checkId(match[1]),
        endpoint = await libvirt.endpoint(id);
      if (socket.destroyed) return;
      wss.handleUpgrade(req, socket, head, (ws) => {
        if (sockets >= 24 || (perSession.get(key) || 0) >= 6) {
          ws.close(1013, "Live console limit");
          return;
        }
        sockets++;
        perSession.set(key, (perSession.get(key) || 0) + 1);
        if (match[2] === "audio") {
          streamAudio(ws, endpoint, () => {
            sockets--;
            perSession.set(key, Math.max(0, (perSession.get(key) || 1) - 1));
          });
          return;
        }
        const tcp = net.connect(endpoint);
        const filter =
          url.searchParams.get("control") === "1" ? null : new ReadOnlyRfb();
        let alive = true;
        const finish = () => {
          if (!alive) return;
          alive = false;
          sockets--;
          perSession.set(key, Math.max(0, (perSession.get(key) || 1) - 1));
          tcp.destroy();
          ws.close();
        };
        tcp.setTimeout(120000, finish);
        tcp.on("data", (data) => {
          try {
            filter?.observeServer(data);
            if (ws.readyState === WebSocket.OPEN) {
              if (ws.bufferedAmount > 16 * 1024 * 1024) return finish();
              ws.send(data);
              const pending = filter?.push(Buffer.alloc(0));
              if (pending?.length) tcp.write(pending);
            }
          } catch {
            finish();
          }
        });
        tcp.on("error", finish);
        tcp.on("close", finish);
        ws.on("message", (data, binary) => {
          if (!binary) return finish();
          try {
            const payload = filter
              ? filter.push(Buffer.from(data))
              : Buffer.from(data);
            if (payload.length && !tcp.write(payload)) {
              ws._socket.pause();
              tcp.once("drain", () => ws._socket.resume());
            }
          } catch {
            finish();
          }
        });
        ws.on("close", finish);
        ws.on("error", finish);
      });
    } catch {
      reject(400);
    }
  });
  return {
    server,
    libvirt,
    captures,
    close: async () => {
      for (const client of wss.clients) client.terminate();
      await new Promise((resolve) => server.close(resolve));
    },
  };
}
if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const app = await createViewer();
  const bind = process.env.VIEWER_BIND || "127.0.0.1";
  app.server.listen(Number(process.env.PORT || 6088), bind, () =>
    console.log(
      "VM Wall listening on " + bind + ":" + (process.env.PORT || 6088),
    ),
  );
}
