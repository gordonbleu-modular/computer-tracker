// WebMIDI connection to the Workshop Tracker card (SysEx needs a secure
// context: https:// or http://localhost, and Chrome/Edge/Opera).
// SPDX-License-Identifier: MIT

export class Device {
  constructor() {
    this.access = null;
    this.input = null;
    this.output = null;
    this.onMessage = () => {};
    this.onState = () => {};
    this.sent = 0;
  }

  static supported() { return typeof navigator !== 'undefined' && !!navigator.requestMIDIAccess; }

  async init() {
    if (this.access) return this.access;
    this.access = await navigator.requestMIDIAccess({ sysex: true });
    this.access.onstatechange = (e) => {
      const p = e.port;
      if ((p === this.input || p === this.output) && p.state === 'disconnected') this.disconnect('Device unplugged');
      this.onState();
    };
    return this.access;
  }

  ports() {
    const ins = [], outs = [];
    if (!this.access) return { ins, outs };
    this.access.inputs.forEach((p) => ins.push(p));
    this.access.outputs.forEach((p) => outs.push(p));
    return { ins, outs };
  }

  // Prefer ports named like the card; fall back to the given names.
  findPair(preferredName) {
    const { ins, outs } = this.ports();
    const match = (p) => /workshop tracker/i.test(p.name) || (preferredName && p.name === preferredName);
    const mtm = (p) => /mtm|music thing|workshop/i.test(p.name);
    const inp = ins.find(match) || ins.find(mtm);
    const out = outs.find(match) || outs.find(mtm) || (inp && outs.find((o) => o.name === inp.name));
    return inp && out ? { inp, out } : null;
  }

  async connect(inp, out) {
    this.disconnect();
    await inp.open();
    await out.open();
    this.input = inp;
    this.output = out;
    inp.onmidimessage = (e) => { if (e.data && e.data[0] === 0xf0) this.onMessage(e.data); };
    this.onState();
  }

  disconnect(reason) {
    if (this.input) this.input.onmidimessage = null;
    const was = !!this.output;
    this.input = null;
    this.output = null;
    if (was) this.onState(reason);
  }

  get connected() { return !!this.output; }

  send(bytes) {
    if (!this.output) return false;
    try {
      this.output.send(bytes);
      this.sent++;
      return true;
    } catch (err) {
      console.warn('MIDI send failed', err);
      return false;
    }
  }
}
