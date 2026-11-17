// Screenshots + layout checks of the editor against a fake module (WebMIDI mock).
// Needs: puppeteer-core, Chrome, build-host/xcheck, `node tools/ui-shots/mkdump.mjs`,
// and the editor served at $URL (default http://localhost:8765/, e.g. `cd web && python3 -m http.server 8765`).
// Run from the repo root: node tools/ui-shots/shots.mjs   (PROTO=1 simulates old firmware)
import puppeteer from 'puppeteer-core';
import { readFileSync } from 'node:fs';
const dump = [...readFileSync('build-host/ui-dump.bin')];
const proto = +(process.env.PROTO || 2);
const out = process.env.OUT || 'docs';
const b = await puppeteer.launch({ executablePath: process.env.CHROME || '/usr/bin/google-chrome', headless: 'new', args: ['--no-sandbox', '--font-render-hinting=none'] });
const results = {};
for (const [w, h] of [[1366, 768], [1920, 1080]]) {
  const p = await b.newPage(); const errs = [];
  p.on('pageerror', (e) => errs.push(e.message)); p.on('console', (m) => { if (m.type() === 'error') errs.push(m.text()); });
  await p.setViewport({ width: w, height: h });
  await p.evaluateOnNewDocument((dump, proto) => {
    localStorage.clear();
    window.__dump = dump;
    window.__sent = [];
    const inp = { name: 'Workshop Tracker', state: 'connected', open: async () => {}, onmidimessage: null };
    const emit = (arr) => setTimeout(() => inp.onmidimessage && inp.onmidimessage({ data: new Uint8Array(arr) }), 1);
    let row = 0;
    const out = { name: 'Workshop Tracker', state: 'connected', open: async () => {}, send(bytes) {
      const d = [...bytes]; window.__sent.push(d);
      if (d[3] === 0x01) emit([0xF0,0x7D,0x54,0x41, proto,0,proto,0, 6,4,16,64,6,32, 0, 3, 0xF7]);
      if (d[3] === 0x02) { let s=-1; for (let i=0;i<dump.length;i++){ if(dump[i]===0xF0) s=i; else if(dump[i]===0xF7&&s>=0){ emit(dump.slice(s,i+1)); s=-1; } } }
      if (d[3] === 0x09) {
        row = (row + 1) % 16;
        const st=[1,1,1,row,0,0,0, 0,118, 56, 64+30,64-20,64+18, 48,0, row%4===0?1:0, 41,58, 96,70,88,60, 50,0, 110,104, 0b1011, 0];
        emit([0xF0,0x7D,0x54,0x45,...st,0xF7]);
        if (proto >= 2) {
          // Panel: perform page, knobs Main 2 o'clock, X 10 o'clock, Y waiting for pickup
          const pn=[0, 94,44,82, 4, 64+30,64-20,64+18, 0b1011,0b0110];
          const win = window.__song;
          emit([0xF0,0x7D,0x54,0x46,...pn, ...window.__eff(), 0,118,56,0,1,1,1,64+0, 0xF7]);
        }
      }
    } };
    navigator.requestMIDIAccess = async () => ({ inputs: new Map([['i', inp]]), outputs: new Map([['o', out]]), onstatechange: null });
  }, dump, proto);
  await p.goto(process.env.URL || 'http://localhost:8765/', { waitUntil: 'networkidle0' });
  // effective values the firmware would report (computed with the JS mirror of protocol.h)
  await p.evaluate(async () => {
    const m = await import('/js/protocol.js');
    const dc = new m.DumpCollector(); let song = null; let st = -1;
    const d = window.__dump;
    for (let i = 0; i < d.length; i++) { if (d[i] === 0xF0) st = i; else if (d[i] === 0xF7 && st >= 0) { const r = dc.feed(m.decode(d.slice(st, i + 1))); if (r) song = r; st = -1; } }
    window.__appSong = () => song;
    window.__eff = () => {
      const s = window.__appSong ? window.__appSong() : null;
      const mac = [30, -20, 18];
      if (!s) return [0,0,0,0, 0,0,0,0, 64];
      const d = [0,1,2,3].map((v) => m.effectiveDrum(s, mac, v, 1));
      const a = [0,1,2,3].map((v) => m.effectiveDrum(s, mac, v, 2));
      return [...d, ...a, m.effectiveFilter(s, mac)];
    };
  });
  await p.click('#btnConnect');
  await new Promise((r) => setTimeout(r, 2200));
  const r = await p.evaluate(() => {
    const pane = document.getElementById('pane-voices');
    return {
      pageScroll: document.documentElement.scrollHeight > innerHeight || document.documentElement.scrollWidth > innerWidth,
      paneVScroll: pane.scrollHeight > pane.clientHeight + 1, paneHScroll: pane.scrollWidth > pane.clientWidth + 1,
      gridW: document.getElementById('grid').clientWidth, gridScrollW: document.getElementById('grid').scrollWidth,
      name: document.getElementById('songName').value, conn: document.getElementById('connState').textContent,
      modulated: document.querySelectorAll('#voices .knob.modulated').length,
      routesOn: [...document.querySelectorAll('.routes button.on')].length,
      comp: document.querySelector('.computer .page').textContent,
      banner: document.getElementById('banner').hidden ? '' : document.getElementById('banner').textContent,
      mode: document.getElementById('modeOut').textContent,
    };
  });
  await p.screenshot({ path: `${out}/screenshot-voices-${w}x${h}.png` });
  await p.click('#tabMixer');
  await new Promise((r) => setTimeout(r, 300));
  r.mixerVScroll = await p.evaluate(() => { const e = document.getElementById('pane-mixer'); return e.scrollHeight > e.clientHeight + 1 || e.scrollWidth > e.clientWidth + 1; });
  r.mixerNames = await p.$$eval('#mixer .mtitle b', (x) => x.map((e) => e.textContent).join(','));
  await p.screenshot({ path: `${out}/screenshot-mixer-${w}x${h}.png` });
  await p.click('#tabCv');
  await new Promise((r) => setTimeout(r, 200));
  r.cvScroll = await p.evaluate(() => { const e = document.getElementById('pane-cv'); return e.scrollHeight > e.clientHeight + 1; });
  if (w === 1366) await p.screenshot({ path: `${out}/screenshot-cv-${w}x${h}.png` });
  r.errors = errs;
  results[`${w}x${h}`] = r;
  await p.close();
}
console.log(JSON.stringify(results, null, 1));
await b.close();
