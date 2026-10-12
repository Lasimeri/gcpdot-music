# gcpdot-music

The next song in VLC chosen by where the old [GCP dot](https://global-mind.org/gcpdot/) would be, with
Intel Phi Jev forecasting where it is going.

- **The data:** GCP 2.0's network variance (gcp2.net, one value a minute), fetched the moment a song ends
  (so the next song is chosen on the newest minute; also every 30 min) and kept on
  disk up to 7 days. GCP 1.0 stopped collecting on 3 April 2026. gcp2.net rate-limits its one public token
  ("Too Many Attempts.", shared with every visitor): such answers are waited out (5, 15, 30 s).
- **The reference egg:** this machine's hardware RNG (RDSEED) through GCP 1.0's math, one trial a second
  (the sum of 200 bits, z = (trial - 100)/sqrt(50)); its 1 h / 2 h index is the exact chi-square tail of the
  summed z^2 (the original dot's math; Wilson-Hilferty). It is independent of the network, so it is the
  null reference: `gcpdot-music compare` gives the dots' correlation, band agreement against chance and mean
  difference over 24 h (compare.tsv), and Jev sees both. Raw trials: `egg-YYYYMMDD.tsv`; minutes: `egg-min.tsv`.
- **The dot:** the share of past 1 h and 2 h windows whose mean variance is at least the latest one's; the
  index farther from 0.5 is shown, as the original showed the more coherent of its two windows. Bands as on
  the old dot: red < 0.05 (high variance), orange 0.05-0.10, yellow 0.10-0.40, green 0.40-0.90,
  teal 0.90-0.95, blue > 0.95 (low variance). The minute series is already smoothed (lag-1 correlation
  0.964), so it is ranked empirically, not against a chi-square.
- **The forecast:** when a song starts, Jev (`mjev query`, the 35B on Intel Phi Jev) gets the last two hours
  and answers which band the dot will be in when the song ends.
- **The pick:** the band is drawn from Jev's probabilities with the CPU's hardware random source (RDSEED),
  then a song from that band's folders, appended at the end of the VLC playlist, after the last song queued (MPRIS
  `TrackList.AddTrack`), one ahead at a time; a restart takes over the song it queued. The song playing is
  never cut (`switch=next`; `switch=now` cuts on a band change).
- **The timing:** each forecast is for one GCP minute, the newest when it is made plus the song's time
  left (whole minutes; the same horizon for Jev and the analogs), logged as the `Q` line's last field, and
  is scored on that minute's live dot once the minute arrives, not on whatever minute is newest when the
  song starts (its lag differs: on 96 older forecasts that judged another band 4 times, 3 false hits).
- **The record:** `~/.local/state/gcpdot-music/predictions.tsv`: a `Q` line per queued song (time, playlist
  id, minutes ahead, dot, band then, source, the six probabilities, band drawn, file) and an `S` line when it
  starts (dot and band at the start), so the forecast can be scored against persistence. The last field
  of a `Q` line is the age of the newest minute in seconds when the song was chosen.

## Use

```
make -j && make install
systemctl --user enable --now gcpdot-music   # off: systemctl --user disable --now gcpdot-music
gcpdot-music dot                              # the dot now, band shares over 24 h, and a uniformity control
gcpdot-music compare                          # GCP 2.0's dot against the reference egg's, last 24 h
gcpdot-music status                           # what the running daemon last wrote
```

The first run writes `~/.config/gcpdot-music/bands.conf`: `root=`, `switch=`, `jev=on|off`, and one line per
band listing folders under the root, comma-separated. Files: FLAC, MP3, WAV; a FLAC beside a `.cue` of its
name (a whole album in one file) is skipped.

Needs: VLC with its MPRIS interface, `curl`, libsystemd (sd-bus), a CPU with RDSEED, and `mjev` on
`~/.local/bin` for the forecast (`jev=off` uses the dot's band now).

## Forecasting, and learning from its errors

The individual random bits are unpredictable by design. What is forecast is the dot, a slow hourly window,
a few minutes ahead (the time left on the playing song):

- **Persistence** (the band stays): right 94.7% at 2 min, 89.5% at 4, 84.8% at 6 on the first stored day.
  Its only misses are band changes.
- **Analog:** past minutes with a dot and 10-minute slope like now's, and the band h minutes after them.
  Alone it picked the top band worse than persistence (88.8% at 2 min) while catching a quarter of the
  band changes.
- **Anchored analog** (the model in use): the band now gets the measured stay rate for that horizon, the
  rest goes to the analogs' bands. Mean log loss 0.248 against persistence's 0.301 at 2 min, 0.419 against
  0.516 at 4.
- **Jev** gets its last 8 resolved forecasts (predicted band and probability, actual band) in its state.
- **The blend** is Jev^a * anchored^c, normalized; a and c are refitted (grid 0..2) to the lowest mean log
  loss over every resolved forecast each time a song starts (defaults 0.5 and 1 under 10 resolved).
  The band is drawn from the blend with RDSEED.

```
gcpdot-music score          # every resolved forecast: top band right and log loss per forecaster
gcpdot-music backtest 4     # the analog models on the stored series, 4 minutes ahead
```

## Skips and replays

Every poll (5 s) reads VLC's position. A **skip** is a song change with more than 15 s of the last song
left (its position carried on by the time since it was read, since a Jev question can hold the loop
about 40 s); a **replay** is the position jumping back over 10 s within a song, or a song played within
the last 50 starting again. Each is logged (`K` and `R` lines in predictions.tsv) and counted per song in
`prefs.tsv`; within a band, a song's weight in the draw is exp(0.6 x replays - 0.8 x skips), at least
0.05, so skipped songs come back less and replayed ones more. The daemon's own cuts (`switch=now`) are
not counted. `gcpdot-music score` prints the totals.

## The display

`gcpdot-show [OUTPUT]` opens `gcpdot-music show` in a small Alacritty window (44 x 22; Konsole when there
is no Alacritty) and moves it to the bottom-right corner of OUTPUT (default DP-2) with a one-shot KWin
script. It shows the live GCP 2.0 dot and its forecast for the next song (the same live dot), in the old
gcpdot's colours, the forecast per band,
your PC's reference egg (a local control, not GCP data) and whether the last forecast hit; it reads `~/.local/state/gcpdot-music/dots`,
which the daemon rewrites every loop, every 2 s.
