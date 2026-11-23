"""Combine the part shots (PARTS dir) into docs/screenshot-voices-mixer-montage.png."""
import sys
from PIL import Image
parts = sys.argv[1] if len(sys.argv) > 1 else '/tmp/shots'
a, b, m = (Image.open(f'{parts}/part-{n}.png').convert('RGB') for n in ('voices-a', 'voices-b', 'mixer'))
gap = 12
w = a.width + gap + b.width
h = max(a.height, b.height) + gap + m.height
out = Image.new('RGB', (w, h), (18, 18, 20))
out.paste(a, (0, 0)); out.paste(b, (a.width + gap, 0)); out.paste(m, (0, max(a.height, b.height) + gap))
out.save('docs/screenshot-voices-mixer-montage.png', optimize=True)
print('montage', out.size)
