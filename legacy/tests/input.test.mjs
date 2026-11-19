import test from 'node:test';
import assert from 'node:assert/strict';
import {inputEvents,desktopInputEvents,inputFrames} from '../server/input.mjs';
import {Libvirt} from '../server/libvirt.mjs';
import {qcode} from '../src/monitor-control.js';
const id='12345678-1234-1234-1234-123456789abc';
test('monitor input validates bounded events and rejects arbitrary QMP data',()=>{
 const events=[{type:'abs',data:{axis:'x',value:32767}},{type:'key',data:{down:true,key:{type:'qcode',data:'ctrl'}}}];
 assert.deepEqual(inputEvents(events),events);
 for(const invalid of [[],Array(65).fill(events[0]),[{type:'abs',data:{axis:'x',value:32768}}],[{type:'btn',data:{button:'power',down:true}}],[{type:'key',data:{down:true,key:{type:'qcode',data:'x";system_reset'}}}],[{type:'system_reset',data:{}}]])assert.throws(()=>inputEvents(invalid));
 assert.equal(qcode('KeyA'),'a');assert.equal(qcode('ControlRight'),'ctrl_r');assert.equal(qcode('NumpadEnter'),'kp_enter');assert.equal(qcode('Unidentified'),undefined);
});
test('secondary input targets the right video device and rejects unavailable heads',async()=>{
 const commands=[];
 const lib=new Libvirt(async args=>{commands.push(args);if(args[0]==='dumpxml')return '<domain><devices><video><model heads="2"/><alias name="video0"/></video></devices></domain>';return '{"return":{}}';});
 lib.get=async()=>({state:'running',heads:2});
 await lib.input(id,1,[{type:'abs',data:{axis:'x',value:16384}}]);
 const command=JSON.parse(commands[1][2]);assert.equal(command.execute,'input-send-event');assert.equal(command.arguments.device,'video0');assert.equal(command.arguments.head,1);
 await assert.rejects(()=>lib.input(id,2,[{type:'abs',data:{axis:'x',value:0}}]));assert.equal(commands.length,2);
});

test('monitor-local pointer positions map into the correct desktop region',()=>{
 const displays=[{width:1280,height:720},{width:1280,height:720}];
 const move=(head,x,y)=>desktopInputEvents(head,[{type:'abs',data:{axis:'x',value:x}},{type:'abs',data:{axis:'y',value:y}}],displays).map(e=>e.data.value);
 const [leftX,leftY]=move(0,16384,16384),[rightX,rightY]=move(1,16384,16384);
 assert.ok(Math.abs(leftX-8192)<8);assert.ok(Math.abs(rightX-24576)<8);
 assert.equal(leftY,rightY);assert.equal(rightY,16384);
 assert.deepEqual(move(0,0,0),[0,0]);assert.deepEqual(move(1,32767,32767),[32767,32767]);
 const mixed=desktopInputEvents(1,[{type:'abs',data:{axis:'y',value:32767}}],[{width:1920,height:1080},{width:1280,height:720}]);
 assert.equal(mixed[0].data.value,Math.round(719/1079*32767));
 assert.throws(()=>desktopInputEvents(2,[{type:'abs',data:{axis:'x',value:0}}],displays));
});

test('quick right clicks and keystrokes keep press/release in separate USB reports',async()=>{
 const press={type:'btn',data:{button:'right',down:true}},release={type:'btn',data:{button:'right',down:false}};
 assert.deepEqual(inputFrames([press,release]),[[press],[release]]);
 const key={type:'key',data:{down:true,key:{type:'qcode',data:'a'}}};
 assert.deepEqual(inputFrames([key,{...key,data:{...key.data,down:false}}]).map(frame=>frame.length),[1,1]);
 const commands=[];const lib=new Libvirt(async args=>{commands.push(args);return args[0]==='dumpxml'?'<domain><devices><video><model heads="2"/><alias name="video0"/></video></devices></domain>':'{"return":{}}';});
 lib.get=async()=>({state:'running',heads:2});await lib.input(id,1,[press,release]);
 const events=commands.filter(c=>c[0]==='qemu-monitor-command').map(c=>JSON.parse(c[2]).arguments.events);
 assert.deepEqual(events,[[press],[release]]);
});
