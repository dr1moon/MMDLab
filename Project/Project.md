# MMDLab Project

A project holds the MikuMikuDance assets the editor loads. `Models/` holds one subfolder per
PMX model (its `.pmx` plus the `textures/` it references); `Motions/` holds VMD motion files
(the viewer's Motion tab lists and plays them). PMX and VMD are MikuMikuDance file-format identifiers.

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
| 寄明月 dance motion | `Motions/jimingyue-motion.vmd` | Motion by Nivsha (Bilibili), supplied by the user | Free non-commercial use only; no redistribution/reposting; no R18 or commercial use; credit the author or original song. Full rules in the bundled `动作 readme.txt`. |
