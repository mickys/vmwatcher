import test from "node:test";
import assert from "node:assert/strict";
import { ReadOnlyRfb } from "../server/rfb-filter.mjs";
import { decodePpm, encode, combine, resize } from "../server/images.mjs";
import { PNG } from "pngjs";
import { parseDomain, Libvirt } from "../server/libvirt.mjs";
import { esc, filtered, rowsFor, wallGrid } from "../src/model.js";
const id = "12345678-1234-1234-1234-123456789abc";
function ready() {
  const f = new ReadOnlyRfb();
  assert.equal(
    f.push(Buffer.from("RFB 003.008\n")).toString(),
    "RFB 003.008\n",
  );
  assert.deepEqual(f.push(Buffer.from([1, 0])), Buffer.from([1, 1]));
  return f;
}
test("read-only RFB forwards fragmented frame requests and strips keyboard, pointer, resize, clipboard and power", () => {
  const filter = ready(),
    pixel = Buffer.alloc(20);
  const enc = Buffer.from([2, 0, 0, 1, 0, 0, 0, 0]);
  const request = Buffer.from([3, 0, 0, 0, 0, 0, 0, 10, 0, 10]);
  const key = Buffer.from([4, 1, 0, 0, 0, 0, 0, 65]),
    pointer = Buffer.from([5, 1, 0, 2, 0, 3]);
  const text = Buffer.concat([
    Buffer.from([6, 0, 0, 0, 0, 0, 0, 3]),
    Buffer.from("abc"),
  ]);
  const size = Buffer.from([251, 0, 0, 10, 0, 10, 0, 0]);
  const power = Buffer.from([250, 0, 1, 2]);
  const stream = Buffer.concat([
    pixel,
    key,
    request,
    pointer,
    text,
    enc,
    size,
    power,
  ]);
  const output = [];
  for (const byte of stream) output.push(filter.push(Buffer.from([byte])));
  assert.deepEqual(Buffer.concat(output), Buffer.concat([pixel, request, enc]));
});
test("VNC password authentication remains opaque but input is blocked afterward", () => {
  const f = new ReadOnlyRfb();
  f.push(Buffer.from("RFB 003.008\n"));
  assert.deepEqual(f.push(Buffer.from([2])), Buffer.from([2]));
  assert.equal(f.push(Buffer.alloc(16, 7)).length, 16);
  assert.deepEqual(f.push(Buffer.from([0])), Buffer.from([1]));
  assert.equal(f.push(Buffer.from([4, 1, 0, 0, 0, 0, 0, 65])).length, 0);
});
test("RFB 3.3 waits for security type and rejects unknown protocol messages", () => {
  const f = new ReadOnlyRfb();
  f.observeServer(
    Buffer.concat([Buffer.from("RFB 003.003\n"), Buffer.from([0, 0, 0, 1])]),
  );
  f.push(Buffer.from("RFB 003.003\n"));
  assert.deepEqual(f.push(Buffer.from([0])), Buffer.from([1]));
  assert.throws(() => f.push(Buffer.from([99])), /Unsupported/);
  assert.throws(() => {
    const g = new ReadOnlyRfb();
    g.push(Buffer.from("RFB 003.008\n"));
    g.push(Buffer.from([19]));
  }, /authentication/);
});
test("images retain first binary whitespace byte, native dimensions and head order", () => {
  const image = decodePpm(
    Buffer.concat([
      Buffer.from("P6\n# comment\n2 1\n255\n"),
      Buffer.from([10, 20, 30, 200, 210, 220]),
    ]),
  );
  assert.deepEqual([...image.data.subarray(0, 4)], [10, 20, 30, 255]);
  const merged = combine([image, image]);
  assert.equal(merged.width, 12);
  assert.equal(merged.height, 1);
  const output = PNG.sync.read(encode(merged));
  assert.deepEqual([...output.data.subarray(40, 44)], [10, 20, 30, 255]);
  assert.equal(resize(image, 1).width, 1);
  assert.throws(
    () => decodePpm(Buffer.from("P6\n20000 20000\n255\n")),
    /dimensions/,
  );
});
test("domain parser handles attribute/child listen collision, escaped names, and multiple heads", () => {
  const vm = parseDomain(
    `<domain><name>A &amp; B</name><uuid>${id}</uuid><devices><graphics type="vnc" port="5902" listen="127.0.0.1"><listen type="address" address="127.0.0.1"/></graphics><video><model type="virtio" heads="2"/></video></devices></domain>`,
    "running",
  );
  assert.equal(vm.name, "A & B");
  assert.equal(vm.heads, 2);
  assert.deepEqual(vm.endpoint, { host: "127.0.0.1", port: 5902 });
  assert.equal(vm.vnc, true);
  assert.equal(vm.vncLocal, true);
  assert.equal(
    parseDomain(
      `<domain><name>L</name><uuid>${id}</uuid><devices><graphics type="vnc" port="5940" listen="192.0.2.4"/></devices></domain>`,
      "running",
      ["192.0.2.3"],
    ).vncLocal,
    false,
  );
});
test("VNC proxy resolves libvirt UUIDs only and cannot become an arbitrary TCP proxy", async () => {
  const xml = (host) =>
    `<domain><name>Lab</name><uuid>${id}</uuid><devices><graphics type="vnc" port="5902" listen="${host}"/></devices></domain>`;
  let host = "192.0.2.4";
  const lib = new Libvirt(async (args) =>
    args[0] === "list" ? id : args[0] === "domstate" ? "running" : xml(host),
  );
  await assert.rejects(lib.get("../../other"), /identifier/);
  await assert.rejects(lib.endpoint(id), /local VNC/);
  host = "0.0.0.0";
  await lib.inventory(true);
  assert.deepEqual(await lib.endpoint(id), { host: "127.0.0.1", port: 5902 });
  // A console bound to one of the hypervisor's own addresses is proxied to that
  // address; any other machine on the same network is still refused.
  const lan = new Libvirt(
    async (args) =>
      args[0] === "list" ? id : args[0] === "domstate" ? "running" : xml(host),
    () => ["127.0.0.1", "192.0.2.3"],
  );
  host = "192.0.2.3";
  assert.deepEqual(await lan.endpoint(id), { host: "192.0.2.3", port: 5902 });
  assert.equal((await lan.get(id)).vncLocal, true);
  host = "192.0.2.4";
  await lan.inventory(true);
  await assert.rejects(lan.endpoint(id), /local VNC/);
  assert.equal((await lan.get(id)).vncLocal, false);
});
test("wall filters pins/state without mutating inventory and escapes VM names", () => {
  const vms = [
    { id, name: "<Lab>", state: "running" },
    { id: "other", name: "Other", state: "shut off" },
  ];
  assert.equal(filtered(vms, {}).length, 1);
  assert.equal(filtered(vms, { showOff: true, query: "OTHER" }).length, 1);
  assert.equal(
    filtered(vms, { pinnedOnly: true, selected: new Set() }).length,
    0,
  );
  assert.equal(esc("<Lab>"), "&lt;Lab&gt;");
  assert.equal(vms.length, 2);
});

test('preview dimensions follow the requested tile width and actual combined head aspect ratio', async () => {
  const {Captures}=await import('../server/captures.mjs');
  const captures=new Captures({get:async()=>({heads:2})},'unused');
  captures.frame=async(id,head)=>({image:new PNG({width:head?400:800,height:head?300:600})});
  const image=PNG.sync.read(await captures.preview(id,604));
  assert.equal(image.width,604);
  assert.equal(image.height,300);
  const native=PNG.sync.read(await captures.preview(id,3000));
  assert.equal(native.width,1208);
  assert.equal(native.height,600);
});

test("wall rows account for tiles that are several monitors wide", () => {
  assert.equal(rowsFor([1, 1, 1], 3), 1);
  assert.equal(rowsFor([1, 1, 1, 1], 3), 2);
  // A two-monitor tile that does not fit the rest of a row starts a new one.
  assert.equal(rowsFor([1, 1, 2], 3), 2);
  assert.equal(rowsFor([2, 1, 1], 4), 1);
  // Wider than the grid: it takes a full row instead of overflowing.
  assert.equal(rowsFor([2, 1], 1), 2);
});

test('wall adapts between single and dual monitor groups without stacking everything',()=>{
 assert.deepEqual(wallGrid(Array(4).fill(16/9),2722,1230),{columns:2,rows:2});
 assert.deepEqual(wallGrid(Array(2).fill(32/9),2722,1230),{columns:1,rows:2});
 assert.deepEqual(wallGrid(Array(4).fill(16/9),800,1200),{columns:1,rows:4});
});
