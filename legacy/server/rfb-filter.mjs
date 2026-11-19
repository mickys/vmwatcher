// Fail-closed client-message filter. Viewers may request frames, never inject input,
// clipboard, resize or power operations. Supports RFB 3.3/3.7/3.8 None/VNC auth.
export class ReadOnlyRfb {
  constructor() {
    this.state = "version";
    this.buffer = Buffer.alloc(0);
    this.server = Buffer.alloc(0);
    this.security33 = null;
  }
  observeServer(data) {
    if (this.security33 !== null) return;
    this.server = Buffer.concat([this.server, data]).subarray(0, 32);
    if (
      this.server.length >= 12 &&
      this.server.subarray(0, 12).toString() !== "RFB 003.003\n"
    ) {
      this.security33 = -1;
      this.server = Buffer.alloc(0);
      return;
    }
    if (
      this.server.length >= 16 &&
      this.server.subarray(0, 12).toString() === "RFB 003.003\n"
    )
      this.security33 = this.server.readUInt32BE(12);
  }
  push(data) {
    this.buffer = Buffer.concat([this.buffer, data]);
    if (this.buffer.length > 16 * 1024 * 1024)
      throw Error("RFB message too large");
    const output = [];
    while (this.buffer.length) {
      let length,
        allow = true;
      if (this.state === "version") {
        if (this.buffer.length < 12) break;
        const version = this.buffer.subarray(0, 12).toString();
        if (!/^RFB 003\.00[378]\n$/.test(version))
          throw Error("Unsupported RFB version");
        length = 12;
        this.state = version === "RFB 003.003\n" ? "security33" : "security";
      } else if (this.state === "security33") {
        if (this.security33 === null) break;
        this.selectSecurity(this.security33);
        continue;
      } else if (this.state === "security") {
        length = 1;
        this.selectSecurity(this.buffer[0]);
      } else if (this.state === "auth") {
        length = 16;
        if (this.buffer.length < length) break;
        this.state = "init";
      } else if (this.state === "init") {
        length = 1;
        output.push(Buffer.from([1]));
        allow = false;
        this.state = "messages";
      } else {
        const type = this.buffer[0];
        if (type === 0) length = 20;
        else if (type === 2) {
          if (this.buffer.length < 4) break;
          const n = this.buffer.readUInt16BE(2);
          if (n > 512) throw Error("Too many encodings");
          length = 4 + 4 * n;
        } else if (type === 3) length = 10;
        else if (type === 4) {
          length = 8;
          allow = false;
        } else if (type === 5) {
          length = 6;
          allow = false;
        } else if (type === 6) {
          if (this.buffer.length < 8) break;
          length = 8 + Math.abs(this.buffer.readInt32BE(4));
          allow = false;
        } else if (type === 150) length = 10;
        else if (type === 248) {
          if (this.buffer.length < 9) break;
          length = 9 + this.buffer[8];
        } else if (type === 250) {
          length = 4;
          allow = false;
        } else if (type === 251) {
          if (this.buffer.length < 8) break;
          length = 8 + 16 * this.buffer[6];
          allow = false;
        } else if (type === 255) {
          if (this.buffer.length < 2) break;
          if (this.buffer[1] !== 0)
            throw Error("Unsupported QEMU client message");
          length = 12;
          allow = false;
        } else throw Error("Unsupported read-only RFB message " + type);
      }
      if (length > 16 * 1024 * 1024) throw Error("RFB message too large");
      if (this.buffer.length < length) break;
      if (allow) output.push(this.buffer.subarray(0, length));
      this.buffer = this.buffer.subarray(length);
    }
    return Buffer.concat(output);
  }
  selectSecurity(type) {
    if (type !== 1 && type !== 2)
      throw Error(
        "Read-only mode supports None or VNC password authentication",
      );
    this.state = type === 2 ? "auth" : "init";
  }
}
