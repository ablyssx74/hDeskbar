# hDeskbar v0.25.1

## New
- **The tooltips of the CPU graph and the volume update while they are shown.** The "CPU: 23%" and "Volume: 45%" that come when you rest the pointer on them used to say what was true at the moment they appeared. The CPU one is now drawn again every time the load is sampled (twice a second, with the graph), and the volume one the moment the volume or the mute changes: with the mouse wheel on the speaker, or from another program. The capsule keeps its size as the numbers change.

## Install
From the `linux` folder: `makepkg -si`
