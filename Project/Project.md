# MMDLab Project

A project holds the MikuMikuDance assets the editor loads. `Models/` holds one subfolder per
PMX model (its `.pmx` plus the `textures/` it references); `Motions/` holds VMD motion files
(consumed by a later milestone). PMX and VMD are MikuMikuDance file-format identifiers.

## Layout

```text
Project/
├─ Models/          # one subfolder per PMX model (.pmx + its textures/)
│  └─ <model>/
├─ Motions/         # .vmd motion files (used by a later milestone)
└─ Project.md       # this file: structure + asset source/license log
```

## Asset source and license

Record every asset copied into `Models/` or `Motions/` here before use. Assets are git-ignored;
this log is the durable record of where each came from and under what terms.

| Asset | File(s) | Source (URL / author) | License |
| --- | --- | --- | --- |
| Odeta (奥黛塔) | `Models/奥黛塔/奥黛塔.pmx`, `剑.pmx`, `沙拉.pmx` + `tex/`, `spa/`, toon `.bmp`s | Original model by miHoYo (Genshin Impact); edited by 观海 (Bilibili: 观海子) | Free non-commercial use only; no redistribution; no extracting parts; no adult/extreme content; recoloring, clothing edits, and added sphere/toon maps allowed. Full rules in the bundled `readme【一定要看】.txt`. |
