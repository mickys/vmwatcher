import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs/promises";
import path from "node:path";
import net from "node:net";
import { WebSocket } from "ws";
import { createViewer } from "../server/index.mjs";
const id = "12345678-1234-1234-1234-123456789abc";
test("WebSocket proxy authenticates, forces shared view-only sessions, drops input and allows explicit control", async () => {
  const received = [];
  const connections = new Set();
  const tcp = net.createServer((socket) => {
    connections.add(socket);
    socket.on("close", () => connections.delete(socket));
    let buffer = Buffer.alloc(0),
      phase = 0;
    socket.write("RFB 003.008\n");
    socket.on("data", (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      while (true) {
        if (phase === 0) {
          if (buffer.length < 12) return;
          buffer = buffer.subarray(12);
          socket.write(Buffer.from([1, 1]));
          phase = 1;
        } else if (phase === 1) {
          if (buffer.length < 1) return;
          buffer = buffer.subarray(1);
          socket.write(Buffer.alloc(4));
          phase = 2;
        } else if (phase === 2) {
          if (buffer.length < 1) return;
          received.push({ shared: buffer[0] });
          buffer = buffer.subarray(1);
          const init = Buffer.alloc(24);
          init.writeUInt16BE(320, 0);
          init.writeUInt16BE(200, 2);
          socket.write(init);
          phase = 3;
        } else {
          if (buffer.length) received.push(Buffer.from(buffer));
          buffer = Buffer.alloc(0);
          return;
        }
      }
    });
  });
  await new Promise((r) => tcp.listen(0, "127.0.0.1", r));
  const folder = path.resolve(".work/tests/ws-" + Date.now());
  await fs.mkdir(path.join(folder, "dist"), { recursive: true });
  await fs.writeFile(path.join(folder, "dist/index.html"), "viewer");
  const fake = {
    endpoint: async () => ({ host: "127.0.0.1", port: tcp.address().port }),
  };
  const app = await createViewer({
    libvirt: fake,
    root: path.join(folder, "data"),
    dist: path.join(folder, "dist"),
  });
  await new Promise((r) => app.server.listen(0, "127.0.0.1", r));
  const base = "http://127.0.0.1:" + app.server.address().port;
  const home = await fetch(base);
  const cookie = home.headers.get("set-cookie").split(";")[0];
  const sockets = [];
  async function client(control) {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(
        base.replace("http:", "ws:") +
          "/api/vms/" +
          id +
          "/vnc" +
          (control ? "?control=1" : ""),
        "binary",
        { headers: { cookie, origin: base } },
      );
      sockets.push(ws);
      let phase = 0,
        buffer = Buffer.alloc(0);
      ws.on("error", reject);
      ws.on("message", (data) => {
        buffer = Buffer.concat([buffer, data]);
        while (true) {
          if (phase === 0) {
            if (buffer.length < 12) return;
            buffer = buffer.subarray(12);
            ws.send(Buffer.from("RFB 003.008\n"));
            phase = 1;
          } else if (phase === 1) {
            if (buffer.length < 2) return;
            buffer = buffer.subarray(2);
            ws.send(Buffer.from([1]));
            phase = 2;
          } else if (phase === 2) {
            if (buffer.length < 4) return;
            buffer = buffer.subarray(4);
            ws.send(Buffer.from([0]));
            phase = 3;
          } else {
            if (buffer.length < 24) return;
            resolve(ws);
            return;
          }
        }
      });
    });
  }
  try {
    const readonly = await client(false);
    assert.equal(received[0].shared, 1);
    received.length = 0;
    const key = Buffer.from([4, 1, 0, 0, 0, 0, 0, 65]);
    readonly.send(key);
    await new Promise((r) => setTimeout(r, 25));
    assert.equal(received.length, 0);
    const request = Buffer.from([3, 0, 0, 0, 0, 0, 0, 10, 0, 10]);
    readonly.send(request);
    await new Promise((r) => setTimeout(r, 25));
    assert.deepEqual(received[0], request);
    received.length = 0;
    const control = await client(true);
    assert.equal(received[0].shared, 0);
    received.length = 0;
    control.send(key);
    await new Promise((r) => setTimeout(r, 25));
    assert.deepEqual(received[0], key);
  } finally {
    for (const ws of sockets) ws.terminate();
    await app.close();
    for (const socket of connections) socket.destroy();
    await new Promise((r) => tcp.close(r));
    await fs.rm(folder, { recursive: true, force: true });
  }
});
