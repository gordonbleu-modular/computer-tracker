// Builds a song dump with the real firmware parser (build-host/xcheck) for the
// fake module used by shots.mjs. Run from the repo root: node tools/ui-shots/mkdump.mjs
import { demoSong, uploadMessages } from '../../web/js/protocol.js';
import { execFileSync } from 'node:child_process';
import { writeFileSync, mkdirSync } from 'node:fs';
const xc = new URL('../../build-host/xcheck', import.meta.url).pathname;
mkdirSync('build-host', { recursive: true });
const s = demoSong();
s.setMacroMask(0, 0b1011); // Main: drums 1, 2, 4
s.setMacroMask(1, 0b0110); // X: drums 2, 3
s.setDrum(3, 0, 4);        // Modal on drum 4
[12, 80, 64, 72, 0, 30].forEach((x, ch) => s.setSend(ch, x)); // some reverb send (0.4.0)
const input = Buffer.concat(uploadMessages(s).map((m) => Buffer.from(m)));
writeFileSync('build-host/ui-dump.bin', execFileSync(xc, ['apply'], { input, maxBuffer: 1 << 22 }));
console.log('wrote build-host/ui-dump.bin');
