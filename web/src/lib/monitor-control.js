const named = {Enter:'ret',Escape:'esc',Backspace:'backspace',Tab:'tab',Space:'spc',ShiftLeft:'shift',ShiftRight:'shift_r',ControlLeft:'ctrl',ControlRight:'ctrl_r',AltLeft:'alt',AltRight:'alt_r',MetaLeft:'meta_l',MetaRight:'meta_r',ArrowUp:'up',ArrowDown:'down',ArrowLeft:'left',ArrowRight:'right',Delete:'delete',Insert:'insert',Home:'home',End:'end',PageUp:'pgup',PageDown:'pgdn',CapsLock:'caps_lock',Minus:'minus',Equal:'equal',BracketLeft:'bracket_left',BracketRight:'bracket_right',Backslash:'backslash',Semicolon:'semicolon',Quote:'apostrophe',Backquote:'grave_accent',Comma:'comma',Period:'dot',Slash:'slash'};
export function qcode(code) {
  if (/^Key[A-Z]$/.test(code)) return code.slice(3).toLowerCase();
  if (/^Digit[0-9]$/.test(code)) return code.slice(5);
  if (/^F([1-9]|1[0-2])$/.test(code)) return code.toLowerCase();
  if (/^Numpad[0-9]$/.test(code)) return 'kp_'+code.slice(6);
  return named[code] || ({NumpadEnter:'kp_enter',NumpadAdd:'kp_add',NumpadSubtract:'kp_subtract',NumpadMultiply:'kp_multiply',NumpadDivide:'kp_divide',NumpadDecimal:'kp_decimal',NumLock:'num_lock'})[code];
}
export function bindMonitorControl(root, button, send, report) {
  let active=null, head=0, pending=[], sending=false, timer=null, hintTimer=null, hint=null;
  const keys=new Set(), buttons=new Set(), pressedCodes=new Set();
  async function flush() {
    clearTimeout(timer);timer=null;
    if(sending || !pending.length)return;
    sending=true;
    const batch=pending.splice(0,64), target=batch[0].head;
    // A batch must address only one monitor.
    const same=[];
    while(batch.length && batch[0].head===target)same.push(batch.shift().event);
    pending.unshift(...batch);
    try { await send(target,same); }
    catch(error) { pending=[];release();report(error.message); }
    finally {sending=false;if(pending.length)timer=setTimeout(flush,0);}
  }
  function queue(event) {
    if(event.type==='abs') {
      for(let i=pending.length-1;i>=0&&pending[i].head===head&&pending[i].event.type==='abs';i--) {
        if(pending[i].event.data.axis===event.data.axis){pending[i].event=event;return;}
      }
    }
    pending.push({head,event});if(!timer)timer=setTimeout(flush,20);
  }
  function release() {
    clearTimeout(hintTimer);hint?.remove();hint=null;pressedCodes.clear();
    for(const key of keys)queue({type:'key',data:{down:false,key:{type:'qcode',data:key}}});
    for(const name of buttons)queue({type:'btn',data:{down:false,button:name}});
    keys.clear();buttons.clear();
    active?.classList.remove('controlling');active=null;
    button.textContent='Click a monitor to control';button.classList.remove('danger');
  }
  function activate(screen,index) {
    const grabbing=active!==screen;
    if(grabbing)release();
    active=screen;head=index;active.classList.add('controlling');active.focus({preventScroll:true});
    button.textContent=`Release Display ${index+1} (Left Shift + Windows)`;button.classList.add('danger');
    if(grabbing) {
      hint=document.createElement('div');hint.className='control-hint';
      hint.textContent='Release control: Left Shift + Windows key';screen.append(hint);
      hintTimer=setTimeout(()=>{hint?.remove();hint=null;},2000);
    }
  }
  function position(e,screen) {
    const media=screen.querySelector('canvas,img,video');if(!media)return false;
    const r=media.getBoundingClientRect(),w=media.videoWidth||media.naturalWidth||media.width,h=media.videoHeight||media.naturalHeight||media.height;
    if(!w||!h||!r.width||!r.height)return false;
    const scale=Math.min(r.width/w,r.height/h),width=w*scale,height=h*scale;
    const x=Math.max(0,Math.min(1,(e.clientX-r.left-(r.width-width)/2)/width));
    const y=Math.max(0,Math.min(1,(e.clientY-r.top-(r.height-height)/2)/height));
    queue({type:'abs',data:{axis:'x',value:Math.round(x*32767)}});
    queue({type:'abs',data:{axis:'y',value:Math.round(y*32767)}});return true;
  }
  root.querySelectorAll('.monitor .screen').forEach((screen,index)=>{
    screen.tabIndex=0;screen.title='Click to control this monitor · Left Shift + Windows key releases control';
    screen.addEventListener('pointerdown',e=>{
      if(![0,1,2].includes(e.button))return;
      e.preventDefault();e.stopImmediatePropagation();activate(screen,index);
      if(!position(e,screen))return;
      screen.setPointerCapture?.(e.pointerId);
      const name=['left','middle','right'][e.button];buttons.add(name);queue({type:'btn',data:{down:true,button:name}});
    },true);
    screen.addEventListener('pointermove',e=>{if(active!==screen)return;e.preventDefault();e.stopImmediatePropagation();position(e,screen);},true);
    screen.addEventListener('pointerup',e=>{if(active!==screen)return;e.preventDefault();e.stopImmediatePropagation();const name=['left','middle','right'][e.button];if(buttons.delete(name))queue({type:'btn',data:{down:false,button:name}});},true);
    screen.addEventListener('pointercancel',release);
    screen.addEventListener('contextmenu',e=>e.preventDefault());
    screen.addEventListener('wheel',e=>{if(active!==screen)return;e.preventDefault();e.stopImmediatePropagation();const name=e.deltaY<0?'wheel-up':'wheel-down';queue({type:'btn',data:{button:name,down:true}});queue({type:'btn',data:{button:name,down:false}});},{capture:true,passive:false});
    for(const type of ['keydown','keyup'])screen.addEventListener(type,e=>{
      if(active!==screen)return;e.preventDefault();e.stopImmediatePropagation();
      if(type==='keydown')pressedCodes.add(e.code);else pressedCodes.delete(e.code);
      if(pressedCodes.has('ShiftLeft') && (pressedCodes.has('MetaLeft') || pressedCodes.has('MetaRight'))) {release();return;}
      const key=qcode(e.code);if(!key||e.repeat)return;
      const down=type==='keydown';if(down)keys.add(key);else if(!keys.delete(key))return;
      queue({type:'key',data:{down,key:{type:'qcode',data:key}}});
    },true);
    screen.addEventListener('focusout',e=>{if(active===screen&&!screen.contains(e.relatedTarget))release();});
  });
  button.disabled=false;button.onclick=release;release();
  const hidden=()=>{if(document.hidden)release();};
  window.addEventListener('blur',release);document.addEventListener('visibilitychange',hidden);
  return ()=>{release();flush();window.removeEventListener('blur',release);document.removeEventListener('visibilitychange',hidden);};
}
