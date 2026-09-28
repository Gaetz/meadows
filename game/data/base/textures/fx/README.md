# textures/fx

Flame flipbooks from Unity Labs, "Free VFX image sequences and flipbooks"
(https://unity.com/blog/2016/11/28/free-vfx-image-sequences-flipbooks/),
released under CC0: real fluid-simulation renders, 16 x 4 frames row-major,
frame aspect 1:2 (128 x 256 px per frame on a 2048 x 1024 sheet).

- `flame02_16x4.png` — a flame tongue with a smoke tail (torches, braziers).
- `smallflame01_16x4.png` — a dense low flame (ground fire, campfires);
  re-laid on 192 x 256 px frames (aspect 0.75): each frame shifted so the
  flame's bright core sits on the frame's centre line — the original sim
  sways ±45 px and read as the whole flame sliding sideways.

Referenced by `ParticleForm.texture` with `blend = "flame"` and the
`flipbook*` fields (docs/FIRE-RENDER.md F2).
