"""Generates sounds/wind/gust.wav: a 1.6 s whoosh (filtered noise under a
swept one-pole low-pass and a swell envelope). CC0, made for this project."""
import math, random, struct, wave, os
rate, seconds = 44100, 1.6
n = int(rate * seconds)
rng = random.Random(7)
out = []
lp = 0.0
for i in range(n):
    t = i / rate
    u = t / seconds
    # Cut-off sweeps 250 -> 1600 -> 350 Hz: the gust arrives, passes, fades.
    swell = math.sin(math.pi * min(u / 0.45, 1.0)) if u < 0.45 else 1.0
    cutoff = 250.0 + 1350.0 * math.sin(math.pi * u) ** 1.5
    a = 1.0 - math.exp(-2.0 * math.pi * cutoff / rate)
    lp += a * (rng.uniform(-1.0, 1.0) - lp)
    env = (1.0 - math.exp(-t / 0.25)) * math.exp(-max(0.0, t - 0.7) / 0.35)
    out.append(lp * env * swell * 2.2)
peak = max(abs(s) for s in out) or 1.0
os.makedirs('game/data/base/sounds/wind', exist_ok=True)
with wave.open('game/data/base/sounds/wind/gust.wav', 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(b''.join(struct.pack('<h', int(max(-1.0, min(1.0, s / peak * 0.8)) * 32767)) for s in out))
print('gust.wav', n, 'samples')
