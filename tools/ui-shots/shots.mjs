// Screenshots + layout checks of the editor against a fake module (WebMIDI mock).
// Needs: puppeteer-core, Chrome, build-host/xcheck, `node tools/ui-shots/mkdump.mjs`,
// and the editor served at $URL (default http://localhost:8765/, e.g. `cd web && python3 -m http.server 8765`).
// Run from the repo root: node tools/ui-shots/shots.mjs   (PROTO=6 / 5 / 4 / 3 / 2 / 1 simulate firmware 0.4.1 / 0.3.2 / 0.3.1 / 0.3.0 / 0.2 / 0.1)
import puppeteer from 'puppeteer-core';
import { readFileSync } from 'node:fs';
const dump = [...readFileSync('build-host/ui-dump.bin')];
const proto = +(process.env.PROTO || 7);
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
    window.__mutes = 0;
    const out = { name: 'Workshop Tracker', state: 'connected', open: async () => {}, send(bytes) {
      const d = [...bytes]; window.__sent.push(d);
      if (d[3] === 0x01) emit([0xF0,0x7D,0x54,0x41, proto,0,proto >= 6 ? 4 : proto >= 3 ? 3 : proto,proto >= 7 ? 2 : proto >= 6 ? 1 : proto >= 5 ? 2 : proto >= 4 ? 1 : 0, proto >= 3 ? 8 : 6,4,16,64,6,32, 0, 3, 0xF7]);
      if (d[3] === 0x16 && d[4] >= 8 && proto >= 7) { const bit = d[4] === 8 ? 32 : 64; window.__mutes = d[5] >= 64 ? (window.__mutes | bit) : (window.__mutes & ~bit); }
      if (d[3] === 0x02) { window.__dumps = (window.__dumps || 0) + 1; }
      if (d[3] === 0x02) { let s=-1; for (let i=0;i<dump.length;i++){ if(dump[i]===0xF0) s=i; else if(dump[i]===0xF7&&s>=0){ emit(dump.slice(s,i+1)); s=-1; } } }
      if (d[3] === 0x09) {
        row = (row + 1) % 16;
        const st=[1,1,1,row,0,window.__slot || 0,window.__mutes, 0,118, 56, 64+30,64-20,64+18, 48,0, (row%4===0?1:0) | (proto >= 3 && row%2===0 ? 4 : 0), 41,58, 96,70,88,60, 50,0, 110,104, 0b1011, window.__swaps || 0, ...(proto >= 6 ? [72] : [])];
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
      kbdShown: !document.getElementById('kbd').hidden,
      kbdKeys: document.querySelectorAll('#kbdKeys .kb-key.note').length,
      kbdTitle: document.getElementById('kbdTitle').textContent,
      slotNow: document.getElementById('slotNow').textContent,
      hexLeft: /[A-F]/.test([...document.querySelectorAll('#grid .f.v, #grid .f.xv')].map((x) => x.textContent).join('')),
    };
  });
  const tag = proto >= 7 ? '' : `-proto${proto}`;
  r.panelStyles = await p.$$eval('#voices .module.drum', (x) => x.map((e) => [...e.classList].find((c) => c.startsWith('p-')) + ':' + e.querySelector('.plate').textContent));
  r.voiceJacks = await p.$$eval('#voices .jack', (x) => x.length);
  await p.screenshot({ path: `${out}/screenshot-voices-${w}x${h}${tag}.png` });
  // cursor on CV1 (VCO 1): the keyboard sketch shows the CV octave and voltage
  await p.click('#grid span.f[data-r="0"][data-t="4"][data-s="0"]');
  await new Promise((r) => setTimeout(r, 150));
  r.kbdCv = await p.evaluate(() => ({
    title: document.getElementById('kbdTitle').textContent,
    octMax: Math.max(...[...document.querySelectorAll('#octave option')].filter((o) => !o.disabled).map((o) => +o.value)),
    q: document.querySelector('#kbdKeys .kb-key[data-code="KeyQ"]')?.textContent,
    z: document.querySelector('#kbdKeys .kb-key[data-code="KeyZ"]')?.textContent,
  }));
  if (!tag) await p.screenshot({ path: `${out}/screenshot-grid-keyboard-${w}x${h}.png` });
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
  r.mixerJacks = await p.$$eval('#mixer .jack', (x) => x.length);
  r.mixerMutes = await p.$$eval('#mixer button.mute', (x) => x.map((e) => (e.disabled ? 'off' : 'on')).join(','));
  if (!tag) await p.screenshot({ path: `${out}/screenshot-mixer-${w}x${h}.png` });
  if (!tag && w === 1920) await (await p.$('#mixer')).screenshot({ path: `${process.env.PARTS || '/tmp'}/part-mixer.png` });
  // REVERB + MASTER mutes (v7): the click sends SetMaster 8 / 9, the module's Status flags confirm it
  const mb = await p.$$('#mixer button.mute');
  if (proto >= 7) {
    await mb[mb.length - 2].click();
    await new Promise((r) => setTimeout(r, 900));
    r.reverbMute = await p.evaluate(() => ({
      sent: window.__sent.some((d) => d[3] === 0x16 && d[4] === 8 && d[5] === 127),
      lit: document.querySelectorAll('#mixer .strip.reverb button.mute.on').length === 1,
      flags: window.__mutes,
    }));
    await mb[mb.length - 2].click();
    await new Promise((r) => setTimeout(r, 900));
    r.reverbUnmute = await p.evaluate(() => document.querySelectorAll('#mixer button.mute.on').length === 0 && window.__mutes === 0);
  } else r.oldFwMutesDisabled = await p.$$eval('#mixer .strip.reverb button.mute, #mixer .strip.master button.mute', (x) => x.every((e) => e.disabled));
  await p.click('#tabCv');
  await new Promise((r) => setTimeout(r, 200));
  r.cvScroll = await p.evaluate(() => { const e = document.getElementById('pane-cv'); return e.scrollHeight > e.clientHeight + 1 || e.scrollWidth > e.clientWidth + 1; });
  r.cvModules = await p.$$eval('#cvpanel .mtitle b', (x) => x.map((e) => e.textContent).join(','));
  r.cvModulated = await p.$$eval('#cvpanel .knob.modulated', (x) => x.length);
  r.cvRoutesOn = await p.$$eval('#cvpanel .routes button.on', (x) => x.length);
  r.cvRoutesDisabled = await p.$$eval('#cvpanel .routes button:disabled', (x) => x.length);
  await p.screenshot({ path: `${out}/screenshot-cvin-${w}x${h}${tag}.png` });
  // every faceplate: switch the four drum engines to the two models the demo
  // song does not use (Pluck, Noise) and back to Kick / Snare
  if (!tag) {
    await p.click('#tabVoices');
    await new Promise((r) => setTimeout(r, 150));
    await p.click('#banner button').catch(() => {});
    if (w === 1920) await (await p.$('#voices')).screenshot({ path: `${process.env.PARTS || '/tmp'}/part-voices-a.png` });
    const sel = await p.$$('#voices select.engine');
    for (const [i, e] of [[0, '1'], [1, '2'], [2, '0'], [3, '5']]) await sel[i].select(e);
    await new Promise((r) => setTimeout(r, 200));
    r.panelStyles2 = await p.$$eval('#voices .module.drum', (x) => x.map((e) => [...e.classList].find((c) => c.startsWith('p-'))));
    r.voicesScroll2 = await p.evaluate(() => { const e = document.getElementById('pane-voices'); return e.scrollHeight > e.clientHeight + 1 || e.scrollWidth > e.clientWidth + 1; });
    await p.mouse.move(5, h - 5);
    await p.screenshot({ path: `${out}/screenshot-voices-models-${w}x${h}.png` });
    if (w === 1920) await (await p.$('#voices')).screenshot({ path: `${process.env.PARTS || '/tmp'}/part-voices-b.png` });
  }
  // the module switches slot on its panel while the editor has unsaved edits
  await p.click('#tabVoices').catch(() => {});
  await p.click('#grid span.f[data-r="1"][data-t="4"][data-s="0"]');
  await p.keyboard.press('KeyQ');
  await new Promise((r) => setTimeout(r, 100));
  const dumps0 = await p.evaluate(() => window.__dumps);
  await p.evaluate(() => { window.__slot = 1; window.__swaps = 1; });
  await new Promise((r) => setTimeout(r, 1500));
  r.switch = await p.evaluate((d0) => ({
    redumped: window.__dumps > d0,
    banner: document.getElementById('banner').hidden ? '' : document.getElementById('banner').textContent,
    restore: !document.getElementById('btnRestore').hidden,
    slotNow: document.getElementById('slotNow').textContent,
    slotSel: document.getElementById('slot').value,
  }), dumps0);
  if (!tag && w === 1366) {
    await p.click('#banner button').catch(() => {});
    await new Promise((r) => setTimeout(r, 100));
    await p.click('#btnHelp');
    await new Promise((r) => setTimeout(r, 200));
    await p.screenshot({ path: `${out}/screenshot-keys-help-${w}x${h}.png` });
  }
  r.errors = errs;
  results[`${w}x${h}`] = r;
  await p.close();
}
console.log(JSON.stringify(results, null, 1));
await b.close();
