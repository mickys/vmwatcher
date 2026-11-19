import { inputEvents, inputFrames } from "./input.mjs";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { XMLParser } from "fast-xml-parser";
const execute = promisify(execFile);
const parser = new XMLParser({
  ignoreAttributes: false,
  attributeNamePrefix: "@_",
  parseAttributeValue: false,
});
export const uuidPattern =
  /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
export function checkId(id) {
  if (!uuidPattern.test(id)) throw Error("Invalid VM identifier");
  return id;
}
const localHosts = ["127.0.0.1", "::1", "localhost", "0.0.0.0", "::"];
// The address this viewer is bound to is, by definition, one of the
// hypervisor's own (user-approved 2026-11-14: consoles bound to it may be
// proxied too). Taken from configuration rather than os.networkInterfaces(),
// which needs AF_NETLINK and is blocked by the service sandbox.
export const ownAddresses = () =>
  [process.env.VIEWER_BIND].filter((x) => x && x !== "0.0.0.0" && x !== "::");
// The proxy only ever dials a console on the hypervisor itself: loopback, or
// one of the host's own interface addresses. Never another machine.
export function proxyable(endpoint, own = ownAddresses()) {
  const host = typeof endpoint?.host === "string" ? endpoint.host : "";
  return (
    !!endpoint &&
    (localHosts.includes(host) || (host !== "" && own.includes(host))) &&
    Number.isInteger(endpoint.port) &&
    endpoint.port >= 1024 &&
    endpoint.port <= 65535
  );
}
export function parseDomain(xml, state, own) {
  const d = parser.parse(xml).domain;
  if (!d || !uuidPattern.test(d.uuid)) throw Error("Invalid domain XML");
  const list = (x) => (Array.isArray(x) ? x : x ? [x] : []);
  const graphics = list(d.devices?.graphics).find((g) => g["@_type"] === "vnc");
  const heads = Math.max(
    1,
    Math.min(
      4,
      list(d.devices?.video).reduce(
        (n, v) => n + Number(v.model?.["@_heads"] || 1),
        0,
      ),
    ),
  );
  const endpoint = graphics
    ? {
        host:
          graphics["@_listen"] ||
          list(graphics.listen).find((x) => x["@_type"] === "address")?.[
            "@_address"
          ] ||
          "127.0.0.1",
        port: Number(graphics["@_port"]),
      }
    : null;
  return {
    id: d.uuid,
    name: String(d.name),
    state,
    heads,
    vnc: !!graphics && Number(graphics["@_port"]) > 0,
    // False when the console listens on an address that is not this host's,
    // which the proxy refuses; the UI uses it to explain why Live is unavailable.
    vncLocal:
      !graphics || Number(graphics["@_port"]) <= 0 || proxyable(endpoint, own),
    endpoint,
  };
}
export class Libvirt {
  constructor(
    run = async (args) =>
      (
        await execute("virsh", ["--connect", "qemu:///system", ...args], {
          timeout: 15000,
          maxBuffer: 2 * 1024 * 1024,
        })
      ).stdout,
    own = ownAddresses,
  ) {
    this.run = run;
    this.own = own;
    this.cached = null;
    this.until = 0;
    this.pending = null;
  }
  async inventory(force = false) {
    if (!force && this.cached && Date.now() < this.until) return this.cached;
    if (this.pending) return this.pending;
    this.pending = this.load().finally(() => (this.pending = null));
    return this.pending;
  }
  async load() {
    const ids = (await this.run(["list", "--all", "--uuid"]))
      .trim()
      .split(/\s+/)
      .filter((x) => uuidPattern.test(x));
    const result = [];
    let cursor = 0;
    await Promise.all(
      Array.from({ length: 4 }, async () => {
        while (cursor < ids.length) {
          const id = ids[cursor++];
          try {
            const [xml, state] = await Promise.all([
              this.run(["dumpxml", id]),
              this.run(["domstate", id]),
            ]);
            result.push(parseDomain(xml, state.trim(), this.own()));
          } catch {
            result.push({
              id,
              name: id,
              state: "unavailable",
              heads: 1,
              vnc: false,
            });
          }
        }
      }),
    );
    this.cached = result.sort((a, b) => a.name.localeCompare(b.name));
    this.until = Date.now() + 10000;
    return this.cached;
  }
  async get(id) {
    checkId(id);
    const vm = (await this.inventory()).find((x) => x.id === id);
    if (!vm) throw Error("VM not found");
    return vm;
  }
  async input(id, head, events) {
    checkId(id);
    const safe = inputEvents(events);
    const vm = await this.get(id);
    if (vm.state !== "running" || !Number.isInteger(head) || head < 0 || head >= vm.heads) throw Error("Monitor unavailable");
    const tree = parser.parse(await this.run(["dumpxml", id]));
    const videos = tree.domain.devices.video;
    let offset = head, target;
    for (const video of Array.isArray(videos) ? videos : [videos]) {
      const count = Number(video?.model?.["@_heads"] || 1);
      if (offset < count) { target = video?.alias?.["@_name"]; break; }
      offset -= count;
    }
    if (!target) throw Error("Monitor has no input target");
    const frames=inputFrames(safe);
    for(let index=0;index<frames.length;index++) {
      if(index)await new Promise(resolve=>setTimeout(resolve,25));
      const result = JSON.parse(await this.run(["qemu-monitor-command", id, JSON.stringify({execute:"input-send-event",arguments:{device:target,head:offset,events:frames[index]}})]));
      if (result.error) throw Error(result.error.desc || "Input rejected");
    }
  }
  async screenshot(id, path, head) {
    checkId(id);
    if (!Number.isInteger(head) || head < 0 || head > 3)
      throw Error("Invalid head");
    return this.run(["screenshot", id, path, "--screen", String(head)]);
  }
  async endpoint(id) {
    let vm = await this.get(id);
    if (!["running", "paused", "pmsuspended"].includes(vm.state))
      throw Error("VM is not running");
    await this.inventory(true);
    vm = await this.get(id);
    const endpoint = vm.endpoint;
    if (!proxyable(endpoint, this.own()))
      throw Error("No supported local VNC endpoint");
    return {
      host: !localHosts.includes(endpoint.host)
        ? endpoint.host
        : endpoint.host === "::1"
          ? "::1"
          : "127.0.0.1",
      port: endpoint.port,
    };
  }
}
