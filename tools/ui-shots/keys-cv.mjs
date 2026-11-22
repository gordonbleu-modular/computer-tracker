// Browser check of CV note entry and the keyboard sketch (0.3.2): the cursor skips the
// removed CV VEL column, the octave stops at 5 on CV tracks, notes stay in C0..B5,
// QWERTZ labels swap Y/Z. Serve web/ at :8765 first (see shots.mjs). Exit code 1 on failure.
import puppeteer from 'puppeteer-core';
const b = await puppeteer.launch({ executablePath: '/usr/bin/google-chrome', headless: 'new', args: ['--no-sandbox'] });
const p = await b.newPage(); await p.setViewport({ width: 1366, height: 768 });
const errs = []; p.on('pageerror', (e) => errs.push(e.message));
await p.evaluateOnNewDocument(() => localStorage.clear());
await p.goto('http://localhost:8765/', { waitUntil: 'networkidle0' });
await p.select('#step', '0');
const k = (x) => p.keyboard.press(x);
const cur = () => p.evaluate(() => { const c = document.querySelector('#grid .f.c'); const t = c.closest('.trk'); return [...t.parentNode.querySelectorAll('.trk')].indexOf(t) + ':' + c.dataset.s; });
await p.click('#grid span.f[data-r="2"][data-t="4"][data-s="0"]');
const c0 = await cur(); await k('ArrowRight'); const c1 = await cur(); await k('ArrowLeft'); const c2 = await cur();
// octave 8 on a CV track: capped at 5
await p.select('#octave', '8'); await new Promise((r) => setTimeout(r, 50));
const octSel = await p.$eval('#octave', (e) => e.value);
await k('KeyP');                      // top note of the upper row
const top = await p.evaluate(() => [...document.querySelectorAll('#grid .r')][2].querySelectorAll('.trk')[4].textContent);
await p.select('#octave', '0'); await p.click('#grid span.f[data-r="5"][data-t="4"][data-s="0"]'); await k('KeyZ');
const low = await p.evaluate(() => [...document.querySelectorAll('#grid .r')][5].querySelectorAll('.trk')[4].textContent);
await p.click('#grid span.f[data-r="6"][data-t="4"][data-s="0"]'); await k('KeyY');  // QWERTY Y = A (upper row); on QWERTZ the same position is labelled Z
const y = await p.evaluate(() => [...document.querySelectorAll('#grid .r')][6].querySelectorAll('.trk')[4].textContent);
await p.click('#kbdLayout');
const lab = await p.evaluate(() => ({ layout: document.getElementById('kbdLayout').textContent,
  keyY: document.querySelector('#kbdKeys [data-code="KeyY"]').textContent, keyZ: document.querySelector('#kbdKeys [data-code="KeyZ"]').textContent }));
const res = { c0, c1, c2, octSel, top, low, y, lab, errs };
console.log(res);
const ok = c1 === '4:2' && c2 === '4:0' && octSel === '5' && top.startsWith('E-5') && low.startsWith('C-0') && y.startsWith('A-1') &&
  lab.keyY.startsWith('Z') && lab.keyZ.startsWith('Y') && !errs.length;
console.log(ok ? 'PASS' : 'FAIL');
process.exitCode = ok ? 0 : 1;
await b.close();
