# gcpdot-music

The next song in VLC chosen by where the old [GCP dot](https://global-mind.org/gcpdot/) would be, with
Intel Phi Jev forecasting where it is going.

- **The data:** GCP 2.0's network variance (gcp2.net, one value a minute), fetched the moment a song ends
  (so the next song is chosen on the newest minute; also every 30 min) and kept on
  disk up to 7 days. GCP 1.0 stopped collecting on 3 April 2026.
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
- **The record:** `~/.local/state/gcpdot-music/predictions.tsv`: a `Q` line per queued song (time, playlist
  id, minutes ahead, dot, band then, source, the six probabilities, band drawn, file) and an `S` line when it
  starts (dot and band at the start), so the forecast can be scored against persistence. The last field
  of a `Q` line is the age of the newest minute in seconds when the song was chosen.

## Use

```
make -j && make install
systemctl --user enable --now gcpdot-music   # off: systemctl --user disable --now gcpdot-music
gcpdot-music dot                              # the dot now, band shares over 24 h, and a uniformity control
gcpdot-music status                           # what the running daemon last wrote
```

The first run writes `~/.config/gcpdot-music/bands.conf`: `root=`, `switch=`, `jev=on|off`, and one line per
band listing folders under the root, comma-separated. Files: FLAC, MP3, WAV; a FLAC beside a `.cue` of its
name (a whole album in one file) is skipped.

Needs: VLC with its MPRIS interface, `curl`, libsystemd (sd-bus), a CPU with RDSEED, and `mjev` on
`~/.local/bin` for the forecast (`jev=off` uses the dot's band now).
