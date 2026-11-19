import fs from "node:fs/promises";
import path from "node:path";
import { randomUUID, createHash } from "node:crypto";
import { decodePpm, encode, resize, combine } from "./images.mjs";
export class Captures {
  constructor(libvirt, root) {
    this.libvirt = libvirt;
    this.root = root;
    this.cache = new Map();
    this.inflight = new Map();
    this.active = 0;
    this.waiters = [];
    this.heads = new Map();
    this.auditTail = Promise.resolve();
    this.auditPending = 0;
  }
  async init() {
    await fs.mkdir(path.join(this.root, "scratch"), {
      recursive: true,
      mode: 0o700,
    });
    await fs.mkdir(path.join(this.root, "audits"), {
      recursive: true,
      mode: 0o700,
    });
    for (const name of await fs.readdir(path.join(this.root, "scratch")))
      if (/^[0-9a-f-]+\.ppm$/.test(name))
        await fs.unlink(path.join(this.root, "scratch", name));
  }
  async slot(fn) {
    if (this.active >= 3) {
      if (this.waiters.length >= 80) throw Error("Capture queue busy");
      await new Promise((resolve) => this.waiters.push(resolve));
    }
    this.active++;
    try {
      return await fn();
    } finally {
      this.active--;
      this.waiters.shift()?.();
    }
  }
  async frame(id, head, { fresh = false } = {}) {
    const key = id + ":" + head,
      old = this.cache.get(key);
    if (!fresh && old && Date.now() - old.time < 1200) return old;
    if (this.inflight.has(key)) return this.inflight.get(key);
    const job = this.slot(async () => {
      const vm = await this.libvirt.get(id);
      if (!["running", "paused", "pmsuspended"].includes(vm.state))
        throw Error("VM is not running");
      if (head >= vm.heads) throw Error("Head is not configured");
      const file = path.join(this.root, "scratch", randomUUID() + ".ppm");
      try {
        await this.libvirt.screenshot(id, file, head);
        const stat = await fs.stat(file);
        if (stat.size > 64 * 1024 * 1024) throw Error("Screenshot too large");
        const image = decodePpm(await fs.readFile(file));
        const result = { image, png: encode(image), time: Date.now(), head };
        this.cache.set(key, result);
        this.heads.set(key, {
          width: image.width,
          height: image.height,
          capturedAt: new Date(result.time).toISOString(),
        });
        while (
          this.cache.size > 1 &&
          (this.cache.size > 32 ||
            [...this.cache.values()].reduce(
              (n, f) => n + f.image.data.length + f.png.length,
              0,
            ) >
              128 * 1024 * 1024)
        )
          this.cache.delete(this.cache.keys().next().value);
        return result;
      } finally {
        await fs.unlink(file).catch(() => {});
      }
    }).finally(() => this.inflight.delete(key));
    this.inflight.set(key, job);
    return job;
  }
  async preview(id, width = 720) {
    const vm = await this.libvirt.get(id);
    const frames = await Promise.all(
      Array.from({ length: vm.heads }, (_, n) =>
        this.frame(id, n).catch(() => null),
      ),
    );
    if (!frames.some(Boolean)) throw Error("No display available");
    return encode(
      resize(combine(frames.filter(Boolean).map((f) => f.image)), width),
    );
  }
  capture(id) {
    if (this.auditPending >= 20)
      return Promise.reject(Error("Audit queue busy"));
    this.auditPending++;
    const job = this.auditTail
      .then(() => this.captureNow(id))
      .finally(() => this.auditPending--);
    this.auditTail = job.catch(() => {});
    return job;
  }
  async captureNow(id) {
    const vm = await this.libvirt.get(id);
    const startedAt = new Date().toISOString(),
      frames = [],
      unavailable = [];
    for (let head = 0; head < vm.heads; head++) {
      try {
        frames.push(await this.frame(id, head, { fresh: true }));
      } catch (error) {
        unavailable.push({ head, error: error.message });
      }
    }
    if (!frames.length) throw Error("No displays could be captured");
    const captureId = randomUUID(),
      folder = path.join(this.root, "audits", captureId);
    await fs.mkdir(folder, { mode: 0o700 });
    try {
      const files = [];
      for (const frame of frames) {
        const name = "head-" + frame.head + ".png";
        await fs.writeFile(path.join(folder, name), frame.png, { mode: 0o600 });
        files.push({
          head: frame.head,
          file: name,
          width: frame.image.width,
          height: frame.image.height,
          capturedAt: new Date(frame.time).toISOString(),
          sha256: createHash("sha256").update(frame.png).digest("hex"),
        });
      }
      const canvas = combine(frames.map((f) => f.image));
      const merged = encode(canvas);
      await fs.writeFile(
        path.join(folder, "preview.png"),
        encode(resize(canvas, 720)),
        { mode: 0o600 },
      );
      await fs.writeFile(path.join(folder, "combined.png"), merged, {
        mode: 0o600,
      });
      const manifest = {
        id: captureId,
        vm: { id: vm.id, name: vm.name, state: vm.state },
        startedAt,
        finishedAt: new Date().toISOString(),
        method:
          "libvirt per-head capture, sequential; combined image preserves native pixel dimensions",
        displays: files,
        unavailable,
        combined: {
          file: "combined.png",
          sha256: createHash("sha256").update(merged).digest("hex"),
        },
      };
      await fs.writeFile(
        path.join(folder, "manifest.json"),
        JSON.stringify(manifest, null, 2),
        { mode: 0o600 },
      );
      await this.trim();
      return manifest;
    } catch (error) {
      await fs.rm(folder, { recursive: true, force: true });
      throw error;
    }
  }
  async list(id = null) {
    const items = [];
    for (const folder of await fs.readdir(path.join(this.root, "audits"))) {
      if (!/^[0-9a-f-]{36}$/.test(folder)) continue;
      try {
        const item = JSON.parse(
          await fs.readFile(
            path.join(this.root, "audits", folder, "manifest.json"),
            "utf8",
          ),
        );
        if (!id || item.vm.id === id) items.push(item);
      } catch {}
    }
    return items.sort((a, b) => b.startedAt.localeCompare(a.startedAt));
  }
  async trim() {
    const items = await this.list();
    let used = 0;
    for (let i = 0; i < items.length; i++) {
      const folder = path.join(this.root, "audits", items[i].id);
      let bytes = 0;
      for (const f of await fs.readdir(folder))
        bytes += (await fs.stat(path.join(folder, f))).size;
      used += bytes;
      if (i >= 200 || used > 2 * 1024 ** 3)
        await fs.rm(folder, { recursive: true, force: true });
    }
  }
  async files(captureId) {
    if (!/^[0-9a-f-]{36}$/.test(captureId))
      throw Error("Invalid capture identifier");
    const folder = path.join(this.root, "audits", captureId);
    const manifest = JSON.parse(
      await fs.readFile(path.join(folder, "manifest.json"), "utf8"),
    );
    return { folder, manifest };
  }
}
