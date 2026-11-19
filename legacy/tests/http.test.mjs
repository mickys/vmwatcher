import http from "node:http";
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { PNG } from "pngjs";
import { unzipSync, strFromU8 } from "fflate";
import { createViewer } from "../server/index.mjs";
const id = "12345678-1234-1234-1234-123456789abc";
test("HTTP session/origin boundary, both-head capture and native audit ZIP", async () => {
  const base = path.resolve(".work/tests");
  await fs.mkdir(base, { recursive: true });
  const root = await fs.mkdtemp(path.join(base, "http-"));
  const vm = {
    id,
    name: "Test VM",
    state: "running",
    heads: 2,
    vnc: true,
    endpoint: { host: "127.0.0.1", port: 5900 },
  };
  const inputs=[];
  const fake = {
    input:async (...args)=>inputs.push(args),
    inventory: async () => [vm],
    get: async (key) => {
      assert.equal(key, id);
      return vm;
    },
    screenshot: async (key, file, head) => {
      const width = head ? 3 : 2;
      await fs.writeFile(
        file,
        Buffer.concat([
          Buffer.from(`P6\n${width} 1\n255\n`),
          Buffer.alloc(width * 3, head ? 200 : 50),
        ]),
      );
    },
  };
  await fs.mkdir(path.join(root, "dist"));
  await fs.writeFile(path.join(root, "dist/index.html"), "<h1>VM Wall</h1>");
  const app = await createViewer({
    libvirt: fake,
    root: path.join(root, "data"),
    dist: path.join(root, "dist"),
  });
  await new Promise((resolve) => app.server.listen(0, "127.0.0.1", resolve));
  const url = "http://127.0.0.1:" + app.server.address().port;
  try {
    assert.equal((await fetch(url + "/api/vms")).status, 401);
    const home = await fetch(url);
    assert.equal(home.status, 200);
    const cookie = home.headers.get("set-cookie").split(";")[0],
      headers = { cookie };
    assert.equal(
      (
        await fetch(url + "/api/vms", {
          headers: { ...headers, origin: "http://evil.invalid" },
        })
      ).status,
      403,
    );
    const inventory = await (await fetch(url + "/api/vms", { headers })).json();
    assert.equal(inventory.vms[0].endpoint, undefined);
    const inputUrl=url+'/api/vms/'+id+'/input';
    const event={type:'key',data:{down:true,key:{type:'qcode',data:'shift'}}};
    assert.equal((await fetch(inputUrl,{method:'POST',headers,body:JSON.stringify({head:1,events:[event]})})).status,403);
    assert.equal((await fetch(inputUrl,{method:'POST',headers:{...headers,origin:url},body:JSON.stringify({head:1,events:[event]})})).status,200);
    assert.deepEqual(inputs,[[id,1,[event]]]);
    // Groups: saved from the wall's own origin only, updated by name, validated.
    const put = (name, vms, extra = { origin: url }) =>
      fetch(url + "/api/groups/" + encodeURIComponent(name), {
        method: "PUT",
        headers: { ...headers, ...extra, "content-type": "application/json" },
        body: JSON.stringify({ vms }),
      });
    assert.equal((await put("Lab & co", [id], {})).status, 403);
    assert.equal((await put("Lab & co", [id])).status, 200);
    assert.equal((await put("Lab & co", ["../etc"])).status, 400);
    assert.equal((await put("Another", [])).status, 200);
    const updated = await (await put("Lab & co", [id, id])).json();
    assert.deepEqual(updated.groups, [
      { name: "Another", vms: [] },
      { name: "Lab & co", vms: [id] },
    ]);
    assert.deepEqual(
      (await (await fetch(url + "/api/groups", { headers })).json()).groups,
      updated.groups,
    );
    const capture = await fetch(url + "/api/vms/" + id + "/capture", {
      method: "POST",
      headers: { ...headers, origin: url },
    });
    assert.equal(capture.status, 201);
    const manifest = await capture.json();
    assert.deepEqual(
      manifest.displays.map((d) => d.width),
      [2, 3],
    );
    const archive = await fetch(
      url + "/api/audits/" + manifest.id + "/bundle.zip",
      { headers },
    );
    const files = unzipSync(new Uint8Array(await archive.arrayBuffer()));
    assert.deepEqual(Object.keys(files).sort(), [
      "combined.png",
      "head-0.png",
      "head-1.png",
      "manifest.json",
    ]);
    assert.equal(PNG.sync.read(Buffer.from(files["combined.png"])).width, 13);
    assert.equal(
      JSON.parse(strFromU8(files["manifest.json"])).vm.name,
      "Test VM",
    );
    assert.equal(
      (await fetch(url + "/api/vms/not-a-uuid/preview.png", { headers }))
        .status,
      400,
    );
  } finally {
    await app.close();
    await fs.rm(root, { recursive: true, force: true });
  }
});

test('configured LAN Host and matching Origin are allowed, unrelated origins remain blocked', async () => {
  const root = await fs.mkdtemp(path.resolve('.work/tests/lan-'));
  await fs.mkdir(path.join(root,'dist'));
  await fs.writeFile(path.join(root,'dist/index.html'),'LAN viewer');
  const app = await createViewer({libvirt:{inventory:async()=>[]},root:path.join(root,'data'),dist:path.join(root,'dist'),allowedHosts:['192.0.2.3']});
  await new Promise(resolve=>app.server.listen(0,'127.0.0.1',resolve));
  const base='http://127.0.0.1:'+app.server.address().port;
  try {
    const headers={host:'192.0.2.3:6088',origin:'http://192.0.2.3:6088'};
    const request=(route,headers)=>new Promise((resolve,reject)=>{const req=http.get(base+route,{headers},res=>{res.resume();res.on('end',()=>resolve({status:res.statusCode,headers:res.headers}));});req.on('error',reject);});
    const home=await request('/',headers);assert.equal(home.status,200);
    const cookie=home.headers['set-cookie'][0].split(';')[0];
    assert.equal((await request('/api/vms',{...headers,cookie})).status,200);
    assert.equal((await request('/api/vms',{...headers,cookie,origin:'http://other.invalid'})).status,403);
  } finally {await app.close();await fs.rm(root,{recursive:true,force:true});}
});
