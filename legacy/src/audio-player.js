export function decodePcm(data, channels=2) {
  if(data.byteLength%(channels*2))throw Error('Invalid audio sample packet');
  const frames=data.byteLength/(channels*2),view=new DataView(data);
  const output=Array.from({length:channels},()=>new Float32Array(frames));
  for(let frame=0;frame<frames;frame++)for(let channel=0;channel<channels;channel++)
    output[channel][frame]=view.getInt16((frame*channels+channel)*2,true)/32768;
  return output;
}
export class AudioPlayer {
  constructor(url, changed) {this.url=url;this.changed=changed;this.sources=new Set();this.stopped=false;this.status='Connecting audio…';}
  async start() {
    try {
      const Context=window.AudioContext||window.webkitAudioContext;
      if(!Context)throw Error('This browser cannot play VM audio');
      this.context=new Context({sampleRate:48000});await this.context.resume();
      if(this.stopped)return;
      this.socket=new WebSocket(this.url);this.socket.binaryType='arraybuffer';
      this.socket.onmessage=e=>{
        try {
          if(typeof e.data==='string'){
            const msg=JSON.parse(e.data);
            if(msg.type==='error')throw Error(msg.message);
            if(msg.type!=='format'||msg.format!=='s16le'||msg.channels!==2||msg.sampleRate!==48000)throw Error('Unsupported VM audio format');
            this.format=msg;this.status='Listening';this.changed();return;
          }
          if(!this.format)throw Error('VM audio format unavailable');
          const channels=decodePcm(e.data,this.format.channels);if(!channels[0].length)return;
          // Bound latency: discard packets rather than building a stale backlog.
          const now=this.context.currentTime;if((this.nextTime||0)>now+.35)return;
          const buffer=this.context.createBuffer(channels.length,channels[0].length,this.format.sampleRate);
          channels.forEach((samples,n)=>buffer.copyToChannel(samples,n));
          const source=this.context.createBufferSource();source.buffer=buffer;source.connect(this.context.destination);
          this.sources.add(source);source.onended=()=>{source.disconnect();this.sources.delete(source);};
          const start=Math.max(now+.04,this.nextTime||0);source.start(start);this.nextTime=start+buffer.duration;
        }catch(error){this.stop(error.message);}
      };
      this.socket.onerror=()=>this.stop('VM audio connection failed');
      this.socket.onclose=()=>this.stop('VM audio disconnected');
    }catch(error){this.stop(error.message);}
  }
  stop(error) {
    if(this.stopped)return;this.stopped=true;this.error=error;
    this.socket?.close();for(const source of this.sources){try{source.stop();source.disconnect();}catch{}}
    this.sources.clear();this.context?.close().catch(()=>{});this.changed(error);
  }
}
