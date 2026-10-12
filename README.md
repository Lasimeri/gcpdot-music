# global-mood-ring

(The program and its user service keep the name `gcpdot-music`; the display is `gcpdot-show`.)

The next song in VLC chosen by where the [GCP dot](https://global-mind.org/gcpdot/) will be when the
song playing ends. The dot is rebuilt either from this PC's hardware random source (`source=local`,
the default here) or from GCP 2.0's live network variance (`source=gcp`); an exact forecast and Intel
Phi Jev say where it is going.

```
╔═ OUTSIDE THIS PC: gcp2.net (read only with source=gcp) ══════════════════════════════════════╗
║  getNetVarAggregate24H, one public bearer token (api_token.js), "Too Many Attempts." waited  ║
╚═══════════════════════════════════════════════╤══════════════════════════════════════════════╝
                                                │ every 2 min and on each song change           
                                                │ (minutes arrive 2.5 to 4 min late)            
╔═ DESKTOP: Ryzen 7 5800X, RTX 3090 Ti ═════════╪══════════════════════════════════════════════╗
║  ┌─ gcpdot-music.service (C daemon, loop 5 s) ─▼─────────────────────────────────────────┐   ║
║  │ series   local: egg minutes (RDSEED, 1 trial/s, mean z^2 a minute, under 1 min late)  │   ║
║  │          gcp:   GCP 2.0 netvar minutes, 7 days kept                                   │   ║
║  │ dot      local: exact chi-square of 1 h / 2 h summed z^2; gcp: rank of window means   │   ║
║  │ bands    red <0.05 orange <0.10 yellow <0.40 green <0.90 teal <0.95 blue              │   ║
║  │ forecast local: future minutes chi2(60 h); gcp: AR(2) netvar, windows rebuilt         │   ║
║  │ song start: provisional next song; 60 s before the end: final forecast, redraw        │   ║
║  │   blend Jev^a x forecast^c (refit on scored targets) -> band, song by RDSEED          │   ║
║  │ every new minute: 2-minute alert, scored live (alerts-local.tsv)                      │   ║
║  └──────┬──────────────────────────┬─────────────────────────────┬───────────────────────┘   ║
║         │ mjev query, 35 s limit   │ AddTrack at the bottom      │ dots, every loop          ║
║         ▼                          ▼                             ▼                           ║
║  ┌─ xks :8090, cuda site ───┐  ┌─ VLC (MPRIS) ────────────┐  ┌─ gcpdot-show 52 x 32 ──────┐  ║
║  │ Qwen3.8-35B-A3B Q4_K_M   │  │ (0)MainPL: 21 folders,   │  │ DP-2 bottom-right: dot,    │  ║
║  │ 21 of 41 layers on GPU   │  │ 6 bands; skips, replays  │  │ forecast, alert, hit log   │  ║
║  │ about 12.6 s a question  │  │ weigh the song draw      │  │                            │  ║
║  └──────────────────────────┘  └──────────────────────────┘  └────────────────────────────┘  ║
╚══════════════════════════════════════════════════════════════════════════════════════════════╝
```

## How it works

- **The series.** `source=local`: the reference egg, one trial a second from the CPU's RDSEED (the sum
  of 200 bits, z = (trial - 100)/sqrt(50)), one value a minute (the mean z^2 of its trials), closed
  under a second late; every minute counts with its own number of trials (a restart's partial minute
  too). `source=gcp`: GCP
  2.0's network variance from gcp2.net (`/api/getNetVarAggregate24H`, one value a minute), read every
  2 minutes and on each song change, 2.5 to 4 minutes late, kept up to 7 days. gcp2.net rate-limits its
  one public token ("Too Many Attempts.", shared with every visitor); such answers are waited out (5, 15,
  30 s). GCP 1.0 stopped collecting on 3 April 2026.
- **The dot.** The 1 h and 2 h windows, the index farther from 0.5 shown, as the original showed the more
  coherent of its two windows. Local: the windows' summed z^2 against chi-square with as many degrees of
  freedom as trials (the original dot's math; Wilson-Hilferty), exact because the egg's minutes are
  independent. GCP: the share of past windows whose mean variance is at least the latest one's (its
  minute series is already smoothed, lag-1 correlation 0.964, so it is ranked empirically). Bands as on
  the old dot: red < 0.05 (high variance), orange 0.05-0.10, yellow 0.10-0.40, green 0.40-0.90, teal
  0.90-0.95, blue > 0.95 (low variance).
- **The decision, in two steps.** When a song starts, a provisional next song is queued at once from a
  forecast for the minute this song ends (a whole song ahead), so a song is always waiting. 60 s before
  the end (`DECIDE_S`; at once when less is left) the forecast is made again, about a minute ahead, and
  the band drawn again from it: the same band keeps the provisional song, another band replaces it, so
  the band follows the final forecast exactly. The target minute: local, the last minute closed when
  the song ends, the dot the next song starts on; GCP, the newest minute plus the time left. Each
  forecast goes to Jev as a suggestion with the last two hours and its last 8 resolved forecasts (on
  the GCP source also the egg as a reference); Jev answers within the song's time left less 25 s.
- **The pick.** The blend Jev^a x forecast^c (below) gives the band odds; the band is drawn from them with
  RDSEED, then a song from that band's folders, appended at the bottom of the VLC playlist (MPRIS
  `TrackList.AddTrack` after its highest id), one ahead at a time; a restart takes over the song it
  queued. The song playing is never cut (`switch=next`; `switch=now` cuts on a band change).
- **The record.** `predictions.tsv` (`predictions-local.tsv` with the local source): a `Q` line per queued
  song (time, playlist id, minutes left, dot, band then, Jev or dot, Jev's six probabilities, band drawn,
  file, newest minute's age, the forecast's and the blend's probabilities, exponents, target minute last)
  and an `S` line when it starts (dot and band then); `P` provisional forecasts (not scored); `K` skips and
  `R` replays. Each final forecast is scored on its target minute's dot once that minute is in.

## Use

```
make -j && make install
systemctl --user enable --now gcpdot-music   # off: systemctl --user disable --now gcpdot-music
gcpdot-show                                   # the display window (bottom-right of DP-2)
gcpdot-music start                            # start the playlist: a song from the dot's band now, played at once
gcpdot-music dot                              # the dot now, band shares, a uniformity control
gcpdot-music score                            # resolved forecasts, the live alert record, skips, replays
gcpdot-music mech H [METHOD]                  # backtest of the forecast H minutes ahead
gcpdot-music compare                          # GCP 2.0's dot against the reference egg's, last 24 h
gcpdot-music status                           # what the running daemon last wrote
```

The first run writes `~/.config/gcpdot-music/bands.conf`: `root=`, `switch=next|now`, `jev=on|off`,
`source=gcp|local`, and one line per band listing folders under the root, comma-separated. Files: FLAC,
MP3, WAV; a FLAC beside a `.cue` of its name (a whole album in one file) is skipped.

Needs: VLC with its MPRIS interface, `curl` (GCP source), libsystemd (sd-bus), a CPU with RDSEED, and
`mjev` on `~/.local/bin` for Jev (`jev=off` uses the forecast alone). Jev's model runs on Intel Phi Jev's
`xks`, here on its cuda site with half the layers on the RTX 3090 Ti (`XKS_SITE=cuda`,
`XKS_GPU_LAYERS=50%` in `~/.config/xks/xks.conf`): 21 of 41 layers, 11.6 GB, a question in a median
12.6 s against 53.0 s on the CPU alone.

## The forecast

**Local (exact).** H minutes on, each window has dropped its oldest minutes (known) and gained H new
ones whose summed z^2 is chi-square with 60 x H degrees of freedom; 21 quantiles of it through the index
give the band odds. With the actual future minutes (the oracle check) it reproduces the live dot 100%
of the time. It is calibrated: its own expected accuracy (the mean probability of the band it names)
matches the realized one, so at a given horizon it is at the ceiling for a random source. Measured with
`gcpdot-music mech H` on 0.4 days of the egg:

| minutes ahead | right | persistence | expected (own) | log loss |
|---|---|---|---|---|
| 1 | 85.6% | 83.1% | 85.4% | 0.285 |
| 2 | 83.0% | 74.8% | 81.4% | 0.378 |
| 3 | 78.0% | 68.6% | 78.0% | 0.498 |
| 4 | 77.2% | 67.1% | 76.5% | 0.539 |
| 5 | 72.8% | 65.2% | 73.6% | 0.610 |

The egg's dot crosses band edges about four to five times as often as GCP 2.0's smoothed one, and each
crossing is decided by bits not yet drawn; the one lever is the horizon, hence the decision 60 s before
the song ends (the final step) rather than at its start (a whole song is about 3.5 minutes ahead).

**GCP (rebuilt).** The windows are rebuilt the same way with the new minutes forecast by AR(2) on the
minute-to-minute changes, ranked exactly as the live dot; the forecaster's past H-step errors at 21
quantiles give the odds (oracle check 100%, so all error is the netvar forecast's). Backtest over about
1.3 days, every other minute:

| ahead | right (persistence) | band changes caught | changes called, right | log loss |
|---|---|---|---|---|
| 2 min | 97.8% (94.5%) | 38 / 43 (88.4%) | 38 / 50 (76.0%) | 0.062 |
| 5 min | 95.7% (86.8%) | 90 / 104 (86.5%) | 90 / 112 (80.4%) | 0.108 |

Other forecasters tried on the same data (last value, 5-minute trend, veridical mapping on the shape of
the last 10 changes, AR(2) with hypo-priors or time-of-day spread, AR(2) averaged with veridical
mapping) tie AR(2) within one or two events.

**Jev and the blend.** Jev gets the series, the dot, its last 8 resolved forecasts and the forecast's
band odds as a suggestion. The blend Jev^a x forecast^c is normalized; a and c are refitted (grid 0..2)
to the lowest mean log loss over every resolved forecast (defaults 0.5 and 1 under 10 resolved). On the
GCP source it settled at Jev^0 x forecast^1.25: Jev added nothing to the forecast it was given.

## The 2-minute alert

On every new minute the forecast is made 2 minutes ahead and logged (`alerts.tsv`, `alerts-local.tsv`):
time made, target minute, band then, six probabilities. It alerts when P(change) reaches 0.5 (local, a
change more likely than not: 55% of changes caught, 67% of alerts right at 2 minutes) or 0.6 (GCP, its
balanced point: about 84% and 83%). Each alert is scored when its minute arrives, over the last 24 h:
top band right, **hits** (changes an alert caught, naming the right band), **misses** (changes without
one), **false alarms** (alerts whose change did not come). A switch can be counted by two alerts made a
minute apart. The horizon is fixed by the alert's definition, so its accuracy is at the ceiling above;
the threshold only trades hits against false alarms.

## Skips and replays

Every poll (5 s) reads VLC's position. A **skip** is a song change with more than 15 s of the last song
left (its position carried on by the time since it was read, since a Jev question can hold the loop);
a **replay** is the position jumping back over 10 s within a song, or a song played within the last 50
starting again. Each is logged and counted per song in `prefs.tsv`; within a band, a song's weight in
the draw is exp(0.6 x replays - 0.8 x skips), at least 0.05, and the last 40 played are left out. The
daemon's own cuts (`switch=now`) are not counted.

## The display

`gcpdot-show [OUTPUT]` opens `gcpdot-music show` in an Alacritty window (52 x 32; Konsole when there is
no Alacritty) and moves it to the bottom-right corner of OUTPUT (default DP-2) with a one-shot KWin
script, kept above other windows. It reads `~/.local/state/gcpdot-music/dots`, which the daemon
rewrites every loop:

- the source, the live dot and its minute's age, and the forecast dot for the song's target minute (the
  2-minute forecast when there is no song forecast or its minute has passed), in the old gcpdot's colours;
- the change alert, the forecast per band, and the RNG line (local: the live RNG readout, every second:
  its dot, this trial's sum of 200 bits and z, the minute so far (trials of 60, mean z^2), from
  `$XDG_RUNTIME_DIR/gcpdot-music-rng`, which the egg thread rewrites each second; red "RNG not
  updating" when it is over 5 s old; GCP: this PC's egg as a control); the window redraws every second;
- the last forecast's hit or miss, the live alert record, and the band-change log: hits and misses over
  the last 24 h's changes, false alarms over its alerts, and the latest five events, newest first.

Line wrap is off, so an overlong line is cut rather than scrolling the window.

## Files

`~/.local/state/gcpdot-music/`: `netvar.tsv` (GCP minutes), `egg-YYYYMMDD.tsv` (raw trials),
`egg-min.tsv` (egg minutes), `predictions[-local].tsv`, `alerts[-local].tsv`, `prefs.tsv`,
`compare.tsv`, `dots`, `status`, `busy`, `queued`.
