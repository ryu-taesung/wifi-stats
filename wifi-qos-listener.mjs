// wifi-qos-listener.mjs
import dgram from 'unix-dgram';
import { unlinkSync, existsSync } from 'fs';
import { EventEmitter } from 'events';
import { resolve } from 'path';

const SOCK_PATH = process.env.QOS_SOCK || '/run/user/' + process.getuid() + '/wifi_qos.sock';

class WifiQosSource extends EventEmitter {
  #sock;
  constructor(path = SOCK_PATH) { super(); this.path = path; this.#init(); }
  #init() {
    if (existsSync(this.path)) try { unlinkSync(this.path); } catch {}
    this.#sock = dgram.createSocket('unix_dgram');
    this.#sock.on('message', buf => {
      if (buf.length !== 24) return;
      const tsNs  = buf.readBigUInt64LE(0);
      const rssi  = buf.readInt32LE(8);
      const ok    = buf.readUInt32LE(12);
      const retry = buf.readUInt32LE(16);
      const fail  = buf.readUInt32LE(20);
      const total = ok + retry + fail;
      this.emit('sample', {
        ts: Number(tsNs), rssi, ok, retry, fail,
        efficiency: total ? (ok / total) * 100 : 0
      });
    });
    this.#sock.on('error', e => this.emit('error', e));
    this.#sock.bind(SOCK_PATH, () => this.emit('ready'));
  }
  close(cb) { this.#sock.close(cb); }
}

export default WifiQosSource;

/* demo */
const qos = new WifiQosSource();
qos.on('sample', s => {
  let signal = s.rssi;
  let signal_percent = 2 * (signal + 100);
  if (signal_percent > 100) signal_percent = 100;
  if (signal_percent < 0) signal_percent = 0;
  console.log(`RSSI ${s.rssi} dBm (${signal_percent}%), eff ${s.efficiency.toFixed(2)}%`);
});
qos.on('ready', () => console.log('listener bound:', SOCK_PATH));
qos.on('error', err => console.error('qos err:', err));

