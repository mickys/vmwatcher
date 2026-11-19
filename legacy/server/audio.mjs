import net from 'node:net';
import {WebSocket} from 'ws';

// QEMU VNC audio extension: ui/vnc.c and ui/vnc.h. This client requests audio
// only, uses a shared session, and never accepts input messages from the browser.
export class AudioRfb {
  constructor(write, ready, pcm) {
    this.write=write;this.ready=ready;this.pcm=pcm;
    this.buffer=Buffer.alloc(0);this.state='version';this.enabled=false;
  }
  push(chunk) {
    this.buffer=Buffer.concat([this.buffer,chunk]);
    if(this.buffer.length>2*1024*1024)throw Error('Audio protocol buffer too large');
    for(;;){
      const b=this.buffer;let n;
      if(this.state==='version'){
        n=12;if(b.length<n)return;
        if(!/^RFB 003\.00[378]\n$/.test(b.subarray(0,n).toString()))throw Error('Unsupported audio console protocol');
        if(b.subarray(0,n).toString()==='RFB 003.003\n')throw Error('Audio requires an RFB 3.7 or newer console');
        this.write(Buffer.from('RFB 003.008\n'));this.state='security';
      }else if(this.state==='security'){
        if(b.length<1)return;n=1+b[0];if(b.length<n)return;
        if(!b[0]||!b.subarray(1,n).includes(1))throw Error('Audio is unavailable for this console authentication mode');
        this.write(Buffer.from([1]));this.state='result';
      }else if(this.state==='result'){
        n=4;if(b.length<n)return;if(b.readUInt32BE(0)!==0)throw Error('Audio console authentication failed');
        this.write(Buffer.from([1]));this.state='init';
      }else if(this.state==='init'){
        if(b.length<24)return;n=24+b.readUInt32BE(20);if(n>65536)throw Error('Audio console name too long');if(b.length<n)return;
        const enc=Buffer.from([2,0,0,1,0xff,0xff,0xfe,0xfd]);this.write(enc);this.state='messages';
      }else{
        if(!b.length)return;
        if(b[0]===0){
          if(b.length<4)return;const count=b.readUInt16BE(2);if(count>16)throw Error('Unexpected audio framebuffer update');n=4+count*12;if(b.length<n)return;
          for(let i=0;i<count;i++)if(b.readInt32BE(4+i*12+8)!==-259)throw Error('Console does not support audio-only playback');
          if(count&&!this.enabled){
            this.enabled=true;
            const format=Buffer.from([255,1,0,2,3,2,0,0,0,0]);format.writeUInt32BE(48000,6);
            this.write(Buffer.concat([format,Buffer.from([255,1,0,0])]));this.ready();
          }
        }else if(b[0]===255){
          if(b.length<4)return;if(b[1]!==1)throw Error('Unexpected audio extension');
          const type=b.readUInt16BE(2);n=4;
          if(type===2){
            if(b.length<8)return;const size=b.readUInt32BE(4);if(size>1024*1024||size%4)throw Error('Invalid audio packet');n=8+size;if(b.length<n)return;
            if(!this.enabled)throw Error('Audio received before negotiation');this.pcm(b.subarray(8,n));
          }else if(type!==0&&type!==1)throw Error('Unknown audio packet');
        }else if(b[0]===2){n=1;}
        else if(b[0]===3){if(b.length<8)return;n=8+b.readUInt32BE(4);if(n>65536)throw Error('Console clipboard too large');if(b.length<n)return;}
        else throw Error('Unexpected audio console message');
      }
      this.buffer=this.buffer.subarray(n);
    }
  }
}

export function streamAudio(ws, endpoint, closed) {
  const tcp=net.connect(endpoint);let done=false;
  const finish=(message)=>{
    if(done)return;done=true;clearTimeout(handshake);
    if(message&&ws.readyState===WebSocket.OPEN)ws.send(JSON.stringify({type:'error',message}));
    tcp.destroy();ws.close();closed();
  };
  const handshake=setTimeout(()=>finish('This VM has no enabled VNC audio stream'),10000);
  const parser=new AudioRfb(data=>tcp.write(data),()=>{
    clearTimeout(handshake);
    if(ws.readyState===WebSocket.OPEN)ws.send(JSON.stringify({type:'format',format:'s16le',sampleRate:48000,channels:2}));
  },data=>{
    if(ws.readyState!==WebSocket.OPEN)return;
    if(ws.bufferedAmount>512*1024)return finish('Audio connection is too slow; reconnect to listen');
    ws.send(data);
  });
  tcp.on('data',data=>{try{parser.push(data);}catch(error){finish(error.message);}});
  tcp.on('error',()=>finish('VM audio connection failed'));tcp.on('close',()=>finish());
  ws.on('message',()=>finish('Audio playback does not accept input'));
  ws.on('close',()=>finish());ws.on('error',()=>finish());
}
