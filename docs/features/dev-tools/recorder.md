# Recorder (F6) and the furniture tracer (development build)

`features/recorder.cpp`. The shortcut (Ctrl+Shift+X / 6 / F6 by preset) starts a recording, again stops it; it stops by
itself after 20 s and writes `Documents\...\Apex Radiance\ApexRadiance_Recording_<hhmmss>.txt`, every line with its clock
time, sorted:
- `[log]`: the log lines written meanwhile;
- `[solve]`: the solve journal (level_light_share.cpp: rooms invalidated / sent / held / solved, with the caller, the LOD
  class, the light counts, the lot and the camera story `cam`);
- `[status]`, each time one changes (checked every 100 ms): the indoor light between stories, Rooms at Night, Faster Room
  Lighting, the indoor object maps (RoomMapPadding pairings), "Furniture (last 100 ms)" (room-mode draws, Rooms at Night
  not acting, no rig chain, turned, Apex's indoor-object shader, rig slots turned / lamps kept, the brightness and blue
  values, the night level), "Camera: lot / story" (a floor switch is a new line), and "Rooms at Night sliders" (on,
  Brightness, Blue tint, night level, and what they give furniture in and outside a dark room);
- `[furniture]` (30/09): every room-mode object part (world position from the VS world rows + pixel shader) once at its
  first draw and again at every change of: the path (A = Apex's indoor-object shader, B = the game's shader turned by
  Rooms at Night, game = untouched), dark (the rig holds [NoLight] lights) and acting, the 4 rig lights as the game set
  them (N [NoLight], F fill, L lamp, - empty, with the colour), the vertex lights' sum, the ambient cube weight (game ->
  drawn), the blue kept, and for path A the room light map, its first directional map and the read scale. Capped at 40000
  lines; cheap hashes decide when a line is written;
- `[room]` (30/09): every indoor room of the loaded lots once at the start (these lines were dropped until the 30/09
  fix) and again when its ambient (+0x110 / +0x120), normalisation, solve state, LOD class, light count or shown story
  changes (checked every 100 ms);
- `[probe]`: Light Probe captures (see below);
- at the end, `ApexRadiance.toml` as it was at the start (every setting).

## Light Probe: automatic captures after a floor change (30/09)

After a capture by its shortcut (F7), the same pixel is measured again 1 s and 3 s after each change of the camera's
story on the same lot (`LevelLightShare::ActiveCamera`: the lot with story manager +0x288 == 1, its +0x284), up to 6
automatic captures within 2 minutes. Each LightProbe.txt says why it was taken ("Capture: ..."). Keep the camera still
between the F7 and the floor switches, so the pixel stays on the same object.

## How to capture a furniture problem with a floor switch
1. Point the mouse at the object, press F7 (the "before").
2. Press F6, switch floors (and back), wait until the recording stops by itself.
3. The captures of the object 1 s and 3 s after each switch are taken automatically; the recording has the object's
   `[furniture]` lines (find it by its world position, e.g. from the F7's VS world rows) and every room solve.

**Caps (30/09).** [room] lines have their own cap (20000); [furniture] and [probe] lines share 40000 (a Brightness drag used to fill the shared cap with furniture lines in 4 s, F6 105204, and the room tracer stopped). The file says which one stopped.
