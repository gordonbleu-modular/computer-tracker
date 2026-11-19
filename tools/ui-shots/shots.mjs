// Screenshots + layout checks of the editor against a fake module (WebMIDI mock).
// Needs: puppeteer-core, Chrome, build-host/xcheck, `node tools/ui-shots/mkdump.mjs`,
// and the editor served at $URL (default http://localhost:8765/, e.g. `cd web && python3 -m http.server 8765`).
// Run from the repo root: node tools/ui-shots/shots.mjs   (PROTO=3 / 2 / 1 simulate firmware 0.3.0 / 0.2 / 0.1)
import puppeteer from 'puppeteer-core';
import { readFileSync } from 'node:fs';
const dump = [...readFileSync('build-host/ui-dump.bin')];
const proto = +(process.env.PROTO || 4);
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
      if (d[3] === 0x01) emit([0xF0,0x7D,0x54,0x41, proto,0,proto >= 3 ? 3 : proto,proto >= 4 ? 1 : 0, proto >= 3 ? 8 : 6,4,16,64,6,32, 0, 3, 0xF7]);
      if (d[3] === 0x02) { let s=-1; for (let i=0;i<dump.length;i++){ if(dump[i]===0xF0) s=i; else if(dump[i]===0xF7&&s>=0){ emit(dump.slice(s,i+1)); s=-1; } } }
      if (d[3] === 0x09) {
        row = (row + 1) % 16;
        const st=[1,1,1,row,0,0,0, 0,118, 56, 64+30,64-20,64+18, 48,0, (row%4===0?1:0) | (proto >= 3 && row%2===0 ? 4 : 0), 41,58, 96,70,88,60, 50,0, 110,104, 0b1011, 0];
        emit([0xF0,0x7D,0x54,0x45,...st,0xF7]);
        if (proto >= 2) {
          // Panel: perform page, knobs Main 2 o'clock, X 10 o'clock, Y waiting for pickup
          const pn=[0, 94,44,82, 4, 64+30,64-20,64+18, 0b1011,0b0110];
          const win = window.__song;
          emit([0xF0,0x7D,0x54,0x46,...pn, ...window.__eff(), 0,118,56,0,1,1,1,64+0, ...(proto >= 3 ? window.__effV3() : []), 0xF7]);
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
    // v3 extras: CV/IN routing masks, then effective CV gate, CV glide, IN decay, IN drive
    window.__effV3 = () => {
      const s = window.__appSong ? window.__appSong() : null;
      const mac = [30, -20, 18];
      if (!s) return new Array(10).fill(0);
      return [s.macroCvMask(0), s.macroCvMask(1),
        m.effectiveCv(s, mac, 0, 0), m.effectiveCv(s, mac, 1, 0), m.effectiveCv(s, mac, 0, 1), m.effectiveCv(s, mac, 1, 1),
        m.effectiveIn(s, mac, 0, 4), m.effectiveIn(s, mac, 1, 4), m.effectiveIn(s, mac, 0, 2), m.effectiveIn(s, mac, 1, 2)];
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
      gridFont: document.getElementById('grid').style.fontSize,
      trkW: [...document.querySelectorAll('#grid .r')][0] ? [...document.querySelectorAll('#grid .r')][0].querySelectorAll('.trk')[4].getBoundingClientRect().width : 0,
      subLabels: [...document.querySelectorAll('#grid .head .trk')].map((t) => t.querySelector('.tname').textContent + ':' + [...t.querySelectorAll('.l')].filter((x) => getComputedStyle(x).display !== 'none').map((x) => x.textContent).join('/')),
      stepLabel: document.getElementById('step').parentElement.textContent.trim(),
      hexLeft: /[A-F]/.test([...document.querySelectorAll('#grid .f.v, #grid .f.xv')].map((x) => x.textContent).join('')),
    };
  });
  const tag = proto >= 4 ? '' : `-proto${proto}`;
  await p.screenshot({ path: `${out}/screenshot-voices-${w}x${h}${tag}.png` });
  // FX picker on pattern 3, drum 3, row 16 (ratchet R4 in the demo song)
  await p.click('#follow');
  await p.select('#pattern', '2');
  await new Promise((r) => setTimeout(r, 150));
  const fxCell = '#grid span.f[data-r="15"][data-t="2"][data-s="2"]';
  await p.$eval(fxCell, (e) => e.scrollIntoView({ block: 'center' }));
  await p.click(fxCell);
  await new Promise((r) => setTimeout(r, 200));
  await p.hover('.picker .pitem:nth-of-type(3)');
  await new Promise((r) => setTimeout(r, 100));
  r.picker = await p.evaluate(() => {
    const k = document.querySelector('.picker');
    if (!k) return null;
    const b = k.getBoundingClientRect();
    return { items: [...k.querySelectorAll('.pitem b')].map((x) => x.textContent).join(''), inView: b.right <= innerWidth && b.bottom <= innerHeight && b.left >= 0 && b.top >= 0 };
  });
  r.statusLine = await p.$eval('#statusLine', (e) => e.textContent);
  if (!tag) await p.screenshot({ path: `${out}/screenshot-grid-fxpicker-${w}x${h}.png` });
  await p.keyboard.press('Escape');
  r.pageScrollAfter = await p.evaluate(() => document.documentElement.scrollHeight > innerHeight || document.documentElement.scrollWidth > innerWidth);
  await p.click('#tabMixer');
  await new Promise((r) => setTimeout(r, 300));
  r.mixerVScroll = await p.evaluate(() => { const e = document.getElementById('pane-mixer'); return e.scrollHeight > e.clientHeight + 1 || e.scrollWidth > e.clientWidth + 1; });
  r.mixerNames = await p.$$eval('#mixer .mtitle b', (x) => x.map((e) => e.textContent).join(','));
  if (!tag) await p.screenshot({ path: `${out}/screenshot-mixer-${w}x${h}.png` });
  await p.click('#tabCv');
  await new Promise((r) => setTimeout(r, 200));
  r.cvScroll = await p.evaluate(() => { const e = document.getElementById('pane-cv'); return e.scrollHeight > e.clientHeight + 1 || e.scrollWidth > e.clientWidth + 1; });
  r.cvModules = await p.$$eval('#cvpanel .mtitle b', (x) => x.map((e) => e.textContent).join(','));
  r.cvModulated = await p.$$eval('#cvpanel .knob.modulated', (x) => x.length);
  r.cvRoutesOn = await p.$$eval('#cvpanel .routes button.on', (x) => x.length);
  r.cvRoutesDisabled = await p.$$eval('#cvpanel .routes button:disabled', (x) => x.length);
  await p.screenshot({ path: `${out}/screenshot-cvin-${w}x${h}${tag}.png` });
  r.errors = errs;
  results[`${w}x${h}`] = r;
  await p.close();
}
console.log(JSON.stringify(results, null, 1));
await b.close();
