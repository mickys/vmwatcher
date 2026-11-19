const buttons = new Set(['left', 'middle', 'right', 'wheel-up', 'wheel-down']);
export function inputEvents(events) {
  if (!Array.isArray(events) || !events.length || events.length > 64) throw Error('Invalid input events');
  return events.map(e => {
    const d = e?.data;
    if (e.type === 'abs' && ['x','y'].includes(d?.axis) && Number.isInteger(d.value) && d.value >= 0 && d.value <= 32767)
      return {type:'abs',data:{axis:d.axis,value:d.value}};
    if (e.type === 'btn' && buttons.has(d?.button) && typeof d.down === 'boolean')
      return {type:'btn',data:{button:d.button,down:d.down}};
    if (e.type === 'key' && typeof d?.down === 'boolean' && d.key?.type === 'qcode' && /^[a-z0-9_-]{1,24}$/.test(d.key.data))
      return {type:'key',data:{down:d.down,key:{type:'qcode',data:d.key.data}}};
    throw Error('Invalid input event');
  });
}

// Absolute USB tablets address the whole guest desktop. Monitor-local input
// must include the offset and size of its display in the side-by-side wall.
export function desktopInputEvents(head, events, displays) {
  const safe=inputEvents(events);
  if(!Array.isArray(displays)||!displays.length||displays.length>4||!Number.isInteger(head)||head<0||head>=displays.length)throw Error('Invalid monitor layout');
  for(const d of displays)if(!Number.isInteger(d.width)||!Number.isInteger(d.height)||d.width<1||d.height<1||d.width>16384||d.height>16384)throw Error('Invalid monitor dimensions');
  const width=displays.reduce((n,d)=>n+d.width,0),height=Math.max(...displays.map(d=>d.height));
  const left=displays.slice(0,head).reduce((n,d)=>n+d.width,0),display=displays[head];
  return safe.map(event=>{
    if(event.type!=='abs')return event;
    const horizontal=event.data.axis==='x';
    const pixel=(horizontal?left:0)+(event.data.value/32767)*((horizontal?display.width:display.height)-1);
    const value=Math.round(pixel/Math.max(1,(horizontal?width:height)-1)*32767);
    return {type:'abs',data:{axis:event.data.axis,value}};
  });
}

// USB HID devices sample state on a poll. A press and release in one QMP
// transaction can disappear entirely; keep repeated edges in separate reports.
export function inputFrames(events) {
  const frames=[];let frame=[],changed=new Set();
  for(const event of events){
    const key=event.type==='key'?'key:'+event.data.key.data:event.type==='btn'?'btn:'+event.data.button:null;
    if(key&&changed.has(key)){frames.push(frame);frame=[];changed=new Set();}
    frame.push(event);if(key)changed.add(key);
  }
  if(frame.length)frames.push(frame);
  return frames;
}
