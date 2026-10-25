# Playing a converted game

How a game built with `anyswf build` ([EXPORT.md](EXPORT.md)) behaves at run time:
controls, the per-game `anyswf.toml` settings, saves, sound, what ActionScript the
runtime supports, and the profiler.

## Controls

**PSP.** The analog nub moves an on-screen cursor (it speeds up while you hold the nub)
and Cross is the mouse button. The other buttons are keyboard keys:

| Button | Key (default) |
|---|---|
| D-pad | arrow keys |
| Cross | mouse button |
| Circle | Space |
| Square / Triangle | Z / X |
| L / R | Shift / Control |
| Start / Select | Enter / Escape |

Every mapping can be changed per game in `anyswf.toml` (below). Holding L+R and pressing
Select shows or hides the profiler overlay.

**PC and Android.** The real mouse (or touch) and keyboard drive Flash's `Mouse` and
`Key` directly; the drawn cursor disappears once the mouse moves. A gamepad works like
the PSP (left stick = cursor, south button = mouse button, other buttons as mapped
above) while no keyboard key is held.

## anyswf.toml

`anyswf build --config game.toml` copies the file next to `assets.bin` as `anyswf.toml`,
where the game reads it at start. Its top-level keys and `[android]` table are build
settings ([EXPORT.md](EXPORT.md)); the runtime reads these tables:

```toml
[stage]
scale = "showAll"   # showAll (default) | exactFit | noBorder | noScale

[cursor]
speed = 240         # pixels per second with the nub fully tilted
accel = 1.5         # extra speed reached after holding the nub for 1 s (0 = none)
visible = true      # draw the cursor (Mouse.hide() still hides it)

[keys]              # up down left right cross circle square triangle l r start select
cross = "mouse"     # the mouse button
circle = "space"    # a key name: a letter or digit, up, down, left, right, space, enter,
square = "z"        # escape, shift, control, alt, tab, backspace, delete, insert, home,
start = 13          # end, pgup, pgdn; a Flash key code (1-254); "none"

[debug]
profiler = false    # start with the profiler overlay (on PSP it also logs)
```

Unknown settings and bad values are reported in the log and skipped; the defaults stay.

## Saves

`SharedObject.getLocal(name)` keeps `<name>.sol` in the game's save folder:
`ms0:/PSP/SAVEDATA/<id>/` on PSP, the user data folder on PC (see
[EXPORT.md](EXPORT.md)). `flush()` writes the file at once; every open shared object is
also written when the game exits normally. Values are saved in Flash's AMF0 encoding:
numbers, strings, booleans, `null`, `undefined`, objects, arrays and dates, including
shared and circular references. Functions, movie clips and getter properties are not
saved. Names may not contain spaces or `~ % & \ ; : " ' , < > ? #` (Flash refuses them
too, and `getLocal` returns `null`).

## Sound

Event sounds, `Sound.attachSound` sounds and timeline stream sounds play through a
software mixer (32 voices, 44.1 kHz stereo). Streams stay in step with their timeline
and stop when it stops. MP3 streams are decoded with minimp3 (two at a time); other
sounds were converted to IMA ADPCM at build time. `Sound.setVolume`/`setPan` apply to
the target clip and everything below it; `new Sound()` is the global volume.
`Sound.loadSound` (external files) is not supported.

## ActionScript support

The runtime implements the AS1/AS2 built-ins classic games use:

- MovieClip: properties (`_x`, `_alpha`, `_rotation`, `_width`, `_xmouse`,
  `_droptarget`, ...), timeline control, `attachMovie`, `duplicateMovieClip`,
  `createEmptyMovieClip`, `createTextField`, `removeMovieClip`, `swapDepths`,
  `getNextHighestDepth`, `getInstanceAtDepth`, `getBounds`/`getRect`,
  `localToGlobal`/`globalToLocal`, `hitTest` (bounds and shape), `startDrag`/`stopDrag`,
  the drawing API (`lineStyle`, `beginFill`, `beginGradientFill`, `moveTo`, `lineTo`,
  `curveTo`, `endFill`, `clear`), `Object.registerClass` linkage, and the events
  `onEnterFrame`, `onLoad`, `onUnload`, `onPress`, `onRelease`, `onReleaseOutside`,
  `onRollOver`, `onRollOut`, `onDragOver`, `onDragOut`, `onMouseDown/Up/Move` and every
  `onClipEvent`.
- Buttons: states, hit areas, `on (...)` actions including key presses, the Button
  handler methods and `enabled`.
- `Key`, `Mouse`, `Stage`, `AsBroadcaster`, `Color`, `Date`, `Sound`, `SharedObject`,
  `TextField` and `TextFormat` (dynamic text, variables, colours, border, background,
  `autoSize`), `setInterval`/`clearInterval`, `setTimeout`/`clearTimeout`, `getTimer`,
  plus `Math`, `String`, `Array`, `Object`, `Number`, `Boolean`, `Function`, `Error`.

Not supported yet: input text editing (fields show their text but cannot be typed into),
rich text formatting runs and HTML layout (`setTextFormat` applies to the whole field),
`MovieClip.setMask`, `beginBitmapFill` (drawn grey), filters, `BitmapData`, and anything
that loads files or uses the network (`loadMovie`, `loadVariables`, `XML`, `getURL`).
Drawing-API gradients are shaded per vertex on a grid, close to but not exactly Flash's.
Translucent thick lines can show darker joins.

## Profiler

The overlay shows frames per second, the worst frame, milliseconds per stage (scripts,
timeline, rendering, waiting for the GPU and vsync), draw calls and triangles, and memory:
C heap (PSP), script heap and texture memory (on PSP, the VRAM texture cache's use and
size). On PC, `--profile` logs the same once per second; on PSP the `[debug] profiler`
setting logs it to the console (visible under psplink or PPSSPP). A one-time warning
appears when the PSP heap passes 40 MB.

On PSP, textures are copied into the VRAM left after the frame buffers when they are
first drawn and evicted least recently used; a texture that does not fit is drawn from
main memory, which is slower but correct.
