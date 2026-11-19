import test from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import fs from 'node:fs/promises';
import path from 'node:path';
import {once} from 'node:events';
import {WebSocket} from 'ws';
import {AudioRfb} from '../server/audio.mjs';
import {createViewer} from '../server/index.mjs';
import {decodePcm,AudioPlayer} from '../src/audio-player.js';
const id='12345678-1234-1234-1234-123456789abc';
function init(){const b=Buffer.alloc(24);b.writeUInt16BE(1280);b.writeUInt16BE(720,2);return b;}
function ack(){const b=Buffer.alloc(16);b.writeUInt16BE(1,2);b.writeInt32BE(-259,12);return b;}
function packet(pcm){const h=Buffer.from([255,1,0,2,0,0,0,0]);h.writeUInt32BE(pcm.length,4);return Buffer.concat([h,pcm]);}
test('audio-only RFB negotiates shared PCM playback and handles fragmented packets',()=>{
 const writes=[],samples=[];let ready=0;
 const client=new AudioRfb(b=>writes.push(b),()=>ready++,b=>samples.push(Buffer.from(b)));
 const stream=Buffer.concat([Buffer.from('RFB 003.008\n'),Buffer.from([1,1]),Buffer.alloc(4),init(),ack(),Buffer.from([255,1,0,1]),packet(Buffer.from([0,0,255,127,0,128,0,0]))]);
 for(const byte of stream)client.push(Buffer.from([byte]));
 assert.equal(ready,1);assert.equal(writes[2][0],1,'must use shared session');
 assert.equal(writes[3].readInt32BE(4),-259);assert.equal(writes[4].readUInt32BE(6),48000);
 assert.deepEqual(samples,[Buffer.from([0,0,255,127,0,128,0,0])]);
 assert.throws(()=>client.push(packet(Buffer.alloc(3))),/Invalid audio packet/);
 const auth=new AudioRfb(()=>{},()=>{},()=>{});auth.push(Buffer.from('RFB 003.008\n'));assert.throws(()=>auth.push(Buffer.from([1,2])),/authentication/);
});
test('PCM playback preserves signed little-endian stereo samples and validates packet size',()=>{
 const data=Uint8Array.from([0,128,255,127,0,0,0,64]).buffer;
 const [left,right]=decodePcm(data);assert.deepEqual([...left],[-1,0]);assert.deepEqual([...right],[32767/32768,.5]);
 assert.throws(()=>decodePcm(new ArrayBuffer(3)),/Invalid/);
});
test('muting closes the audio socket and stops scheduled sound',async()=>{
 const savedWindow=globalThis.window,savedSocket=globalThis.WebSocket;let context,socket,stops=0;
 class Context{constructor(){context=this;this.currentTime=0;this.destination={};}async resume(){}createBuffer(channels,length,rate){return {duration:length/rate,copyToChannel(){}};}createBufferSource(){return {connect(){},disconnect(){},start(){},stop(){stops++;}};}async close(){this.closed=true;}}
 class Socket{constructor(){socket=this;}close(){this.closed=true;}}
 try{
  globalThis.window={AudioContext:Context};globalThis.WebSocket=Socket;
  const player=new AudioPlayer('ws://localhost/audio',()=>{});await player.start();
  socket.onmessage({data:JSON.stringify({type:'format',format:'s16le',sampleRate:48000,channels:2})});
  socket.onmessage({data:new ArrayBuffer(16)});assert.equal(player.sources.size,1);
  player.stop();assert.equal(stops,1);assert.equal(context.closed,true);assert.equal(socket.closed,true);
 }finally{globalThis.window=savedWindow;globalThis.WebSocket=savedSocket;}
});
test('audio WebSocket requires the viewer session and origin and streams PCM without browser input',async()=>{
 const peers=new Set();let audioCommands;
 const tcp=net.createServer(socket=>{
  peers.add(socket);socket.on('close',()=>peers.delete(socket));socket.write('RFB 003.008\n');
  let phase=0,buffer=Buffer.alloc(0);
  socket.on('data',chunk=>{
   buffer=Buffer.concat([buffer,chunk]);
   for(;;){
    const lengths=[12,1,1,8,14];const n=lengths[phase];if(!n||buffer.length<n)return;
    const b=buffer.subarray(0,n);buffer=buffer.subarray(n);
    if(phase===0)socket.write(Buffer.from([1,1]));
    if(phase===1)socket.write(Buffer.alloc(4));
    if(phase===2){assert.equal(b[0],1);socket.write(init());}
    if(phase===3){assert.equal(b.readInt32BE(4),-259);socket.write(ack());}
    if(phase===4){audioCommands=Buffer.from(b);socket.write(packet(Buffer.from([1,0,2,0])));}
    phase++;
   }
  });
 });
 await new Promise(r=>tcp.listen(0,'127.0.0.1',r));
 const folder=path.resolve('.work/tests/audio-'+Date.now());await fs.mkdir(path.join(folder,'dist'),{recursive:true});await fs.writeFile(path.join(folder,'dist/index.html'),'viewer');
 const app=await createViewer({root:path.join(folder,'data'),dist:path.join(folder,'dist'),libvirt:{endpoint:async key=>{assert.equal(key,id);return {host:'127.0.0.1',port:tcp.address().port};}}});
 await new Promise(r=>app.server.listen(0,'127.0.0.1',r));const base='http://127.0.0.1:'+app.server.address().port,url=base.replace('http:','ws:')+'/api/vms/'+id+'/audio';let ws;
 try{
  const cookie=(await fetch(base)).headers.get('set-cookie').split(';')[0];
  for(const headers of [{origin:base},{cookie,origin:'http://evil.invalid'}]){
   const denied=new WebSocket(url,{headers});const code=await new Promise(resolve=>{denied.on('unexpected-response',(_,res)=>{resolve(res.statusCode);res.resume();denied.terminate();});denied.on('error',()=>{});});assert.equal(code,403);
  }
  ws=new WebSocket(url,{headers:{cookie,origin:base}});const received=[];
  const gotPcm=new Promise((resolve,reject)=>{ws.on('message',(b,binary)=>{received.push({b,binary});if(binary)resolve();});ws.on('error',reject);});
  await gotPcm;assert.equal(JSON.parse(received[0].b).format,'s16le');assert.deepEqual(received[1].b,Buffer.from([1,0,2,0]));assert.equal(audioCommands.readUInt32BE(6),48000);
  ws.send(Buffer.from([4,1,0,0,0,0,0,65]));await once(ws,'close');
  assert.equal(audioCommands.length,14,'browser input must never be forwarded');
 }finally{ws?.terminate();await app.close();for(const p of peers)p.destroy();await new Promise(r=>tcp.close(r));}
});
