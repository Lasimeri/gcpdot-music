/* gcpdot-music: the next song chosen by where the old GCP dot would be.
 *
 * The dot (global-mind.org/gcpdot, gcpdot.com, defunct) showed an index:
 * the probability of the Global Consciousness Project's network variance
 * over the last one and two hours, the more coherent of the two; above
 * 0.95 blue (small variance), 0.90-0.95 teal, 0.40-0.90 green, 0.10-0.40
 * yellow, 0.05-0.10 orange, below 0.05 red (large variance). GCP 1.0 ended
 * on 3 April 2026; GCP 2.0 (gcp2.net) publishes its network variance once a
 * minute. That series is already smoothed (one minute to the next
 * correlates 0.964), so it is not a chi-square and the index is taken
 * empirically: the share of past windows (1 h, 2 h; up to 7 days of minutes
 * kept on disk) whose mean is at least the latest window's; of the 1 h and
 * 2 h indexes the one farther from 0.5 is shown.
 *
 * Each band plays a group of folders (~/.config/gcpdot-music/bands.conf).
 * When a song starts, the next one is queued in VLC right after it (MPRIS
 * TrackList.AddTrack): Intel Phi Jev (mjev query) is asked which band the
 * dot will be in when the current song ends, given the last two hours; the
 * band is drawn from Jev's probabilities with the CPU's hardware random
 * source (RDSEED), and the song from the band's folders the same way. With
 * Jev off or failing, the band is the dot's band now. Every queue and every
 * start is logged (~/.local/state/gcpdot-music/predictions.tsv) so Jev's
 * forecast can be scored against the band at the start and against
 * persistence (the band at the moment of asking).
 *
 *   gcpdot-music            run (the user service gcpdot-music.service)
 *   gcpdot-music dot        the dot now, and the share of the last 24 h
 *                           each band would have held (a check)
 *   gcpdot-music status     what the running daemon last wrote
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <ftw.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define NBANDS 6
static const char *band_name[NBANDS] = { "red", "orange", "yellow", "green", "teal", "blue" };
static const char *band_desc[NBANDS] = { "index below 0.05", "0.05 to 0.10", "0.10 to 0.40",
                                         "0.40 to 0.90", "0.90 to 0.95", "above 0.95" };

#define MAXMIN (7 * 1440)          /* minutes of series kept */
#define FETCH_S 1800               /* besides each song change, the 24 h history fetched every 30 min */
#define LOOP_S 5
#define UA "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"
#define PLAYER "org.mpris.MediaPlayer2.vlc"
#define MPATH "/org/mpris/MediaPlayer2"

static char home[512], cfgdir[600], statedir[600];

static void say(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char t[32]; time_t now = time(NULL); strftime(t, sizeof t, "%H:%M:%S", localtime(&now));
    printf("gcpdot-music: %s ", t); vprintf(fmt, ap); printf("\n"); fflush(stdout);
    va_end(ap);
}

/* ---- configuration ---- */
static char root[1024] = "/mnt/Personal_Media/Personal_Media/MusicMedia/(0)MainPL";
static int switch_now = 0, use_jev = 1;
static char band_dirs[NBANDS][2048];

static const char *default_conf =
    "# gcpdot-music: which folders each dot band plays (folders under root,\n"
    "# comma-separated). The dot's index is the share of past windows with a\n"
    "# network variance at least as high: red = unusually high variance (the\n"
    "# old dot's \"broadly shared coherence\"), blue = unusually low (\"deeply\n"
    "# shared, internally motivated focus\"), green = ordinary (about half the\n"
    "# time). Edit and `systemctl --user restart gcpdot-music`.\n"
    "root=/mnt/Personal_Media/Personal_Media/MusicMedia/(0)MainPL\n"
    "# next: the song playing always finishes, the next is chosen for the band\n"
    "# then; now: a band change cuts to a song of the new band\n"
    "switch=next\n"
    "# on: Intel Phi Jev forecasts the band at the next song (mjev query);\n"
    "# off: the dot's band now\n"
    "jev=on\n"
    "red=Deathstep,Hardstyle,Dubstep\n"
    "orange=Acid,Glitch\n"
    "yellow=Metal-Rock,Electro,EDM,Euro_Beat,Italio\n"
    "green=Collections,Normal_Music,Modern,Meme_Music,Electro-swing,Asiatic,Vocaloid-Anime\n"
    "teal=Chip_Tune-KeyGen,LoFiVaporwaveChill\n"
    "blue=Nordic-Medieval,Nationalist\n";

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char) e[-1])) *--e = 0;
}

static void load_conf(void) {
    char p[700]; snprintf(p, sizeof p, "%s/bands.conf", cfgdir);
    FILE *f = fopen(p, "r");
    if (!f) {
        mkdir(cfgdir, 0755);
        if ((f = fopen(p, "w"))) { fputs(default_conf, f); fclose(f); say("wrote the default %s", p); }
        f = fopen(p, "r");
        if (!f) { say("cannot read %s", p); exit(1); }
    }
    char line[2600];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0; char *k = line, *v = eq + 1;
        if (!strcmp(k, "root")) snprintf(root, sizeof root, "%s", v);
        else if (!strcmp(k, "switch")) switch_now = !strcmp(v, "now");
        else if (!strcmp(k, "jev")) use_jev = strcmp(v, "off") != 0;
        else for (int b = 0; b < NBANDS; b++) if (!strcmp(k, band_name[b])) snprintf(band_dirs[b], sizeof band_dirs[b], "%s", v);
    }
    fclose(f);
}

/* ---- the hardware random source ---- */
static uint64_t rdseed64(void) {
    unsigned long long v; unsigned char ok;
    for (int i = 0; i < 1000; i++) {
        __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) return v;
    }
    say("RDSEED gave nothing in 1000 tries");
    exit(1);
}
static double rnd01(void) { return (double) (rdseed64() >> 11) * (1.0 / 9007199254740992.0); }
static uint64_t rnd_below(uint64_t n) {   /* unbiased: rejection */
    uint64_t lim = UINT64_MAX - UINT64_MAX % n, v;
    do v = rdseed64(); while (v >= lim);
    return v % n;
}

/* ---- tracks ---- */
struct list { char **p; int n, cap; };
static struct list tracks[NBANDS];
static struct list *scan_into;
static int ext_ok(const char *p) {
    const char *d = strrchr(p, '.');
    return d && (!strcasecmp(d, ".flac") || !strcasecmp(d, ".mp3") || !strcasecmp(d, ".wav"));
}
static int scan_cb(const char *p, const struct stat *st, int type, struct FTW *ftw) {
    (void) st; (void) ftw;
    if (type != FTW_F || !ext_ok(p)) return 0;
    /* A FLAC beside a .cue of its name is a whole album in one file: skipped. */
    char cue[4200]; snprintf(cue, sizeof cue, "%.*s.cue", (int) (strrchr(p, '.') - p), p);
    if (!access(cue, F_OK)) return 0;
    struct list *l = scan_into;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->p = realloc(l->p, l->cap * sizeof *l->p); }
    l->p[l->n++] = strdup(p);
    return 0;
}
static void scan_tracks(void) {
    for (int b = 0; b < NBANDS; b++) {
        tracks[b].n = 0;
        char dirs[2048]; snprintf(dirs, sizeof dirs, "%s", band_dirs[b]);
        for (char *s = strtok(dirs, ","); s; s = strtok(NULL, ",")) {
            while (*s == ' ') s++;
            char d[2200]; snprintf(d, sizeof d, "%s/%s", root, s);
            scan_into = &tracks[b];
            if (nftw(d, scan_cb, 16, FTW_PHYS) != 0) say("no folder %s (band %s)", d, band_name[b]);
        }
        say("band %-6s %4d songs (%s)", band_name[b], tracks[b].n, band_dirs[b]);
    }
}
/* The last songs played per band, not drawn again (half the band, at most 40). */
#define RECENT 40
static char *recent[NBANDS][RECENT]; static int recent_n[NBANDS];
static const char *pick_track(int b) {
    struct list *l = &tracks[b];
    if (!l->n) return NULL;
    int keep = l->n / 2 < RECENT ? l->n / 2 : RECENT;
    for (int tries = 0; tries < 64; tries++) {
        const char *t = l->p[rnd_below((uint64_t) l->n)];
        int seen = 0;
        for (int i = 0; i < recent_n[b] && i < keep; i++) if (recent[b][i] && !strcmp(recent[b][i], t)) seen = 1;
        if (seen && tries < 63) continue;
        free(recent[b][RECENT - 1]);
        memmove(&recent[b][1], &recent[b][0], (RECENT - 1) * sizeof(char *));
        recent[b][0] = strdup(t);
        if (recent_n[b] < RECENT) recent_n[b]++;
        return t;
    }
    return NULL;
}

/* ---- the GCP 2.0 feed ---- */
static char token[256];
static char *run_read(const char *cmd, size_t *len) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    size_t cap = 1 << 17, n = 0; char *b = malloc(cap);
    size_t r;
    while ((r = fread(b + n, 1, cap - n - 1, p)) > 0) { n += r; if (cap - n < 4096) b = realloc(b, cap *= 2); }
    b[n] = 0; pclose(p);
    if (len) *len = n;
    return b;
}
static int get_token(void) {
    char *js = run_read("curl -s -m 20 -A '" UA "' https://gcp2.net/js/data/api_token.js", NULL);
    if (!js) return -1;
    char *s = strstr(js, "Bearer ");
    if (!s) say("token page without a token: %.120s", js);
    int ok = -1;
    if (s) {
        s += 7;
        size_t n = strcspn(s, "'\"");
        if (n > 0 && n < sizeof token) { memcpy(token, s, n); token[n] = 0; ok = 0; }
    }
    free(js);
    return ok;
}
static long sep[MAXMIN]; static double sval[MAXMIN]; static int sn;
static void series_path(char *p, size_t n) { snprintf(p, n, "%s/netvar.tsv", statedir); }
static void series_load(void) {
    char p[700]; series_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    long e; double v;
    while (fscanf(f, "%ld %lf", &e, &v) == 2) {
        if (sn && e <= sep[sn - 1]) continue;
        if (sn == MAXMIN) { memmove(sep, sep + 1, (MAXMIN - 1) * sizeof *sep); memmove(sval, sval + 1, (MAXMIN - 1) * sizeof *sval); sn--; }
        sep[sn] = e; sval[sn] = v; sn++;
    }
    fclose(f);
}
/* New minutes from the 24 h history appended (memory and disk). */
static int fetch(void) {
    if (!token[0] && get_token()) return -1;
    for (int attempt = 0; attempt < 4; attempt++) {
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "curl -s -m 30 -A '" UA "' -H 'Authorization: Bearer %s' -H 'Accept: application/json' "
                 "-H 'Referer: https://gcp2.net/data-results/live-data' https://gcp2.net/api/getNetVarAggregate24H", token);
        char *j = run_read(cmd, NULL);
        if (!j) return -1;
        if (!strstr(j, "\"aggregates\"")) {
            for (char *q = j; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
            say("history answer without data (attempt %d): %.200s", attempt + 1, j);
            free(j);
            if (attempt == 0) get_token();
            if (attempt < 3) { sleep(3); continue; }
            return -1;
        }
        char p[700]; series_path(p, sizeof p);
        FILE *f = fopen(p, "a");
        int added = 0;
        for (char *s = j; (s = strstr(s, "\"end_epoch\":")); ) {
            s += 12;
            long e = strtol(s, &s, 10);
            char *v = strstr(s, "\"netvar_aggregate\":\"");
            if (!v) break;
            double x = strtod(v + 20, NULL);
            if (sn && e <= sep[sn - 1]) continue;
            if (sn == MAXMIN) { memmove(sep, sep + 1, (MAXMIN - 1) * sizeof *sep); memmove(sval, sval + 1, (MAXMIN - 1) * sizeof *sval); sn--; }
            sep[sn] = e; sval[sn] = x; sn++; added++;
            if (f) fprintf(f, "%ld %.4f\n", e, x);
        }
        if (f) fclose(f);
        free(j);
        return added;
    }
    return -1;
}

/* ---- the dot ---- */
/* Index of the window of W minutes ending at minute END (exclusive) against
 * every W-minute window of the baseline [B0, END): the share whose mean is at
 * least the latest one's. -1 when there is too little. */
static double index_w(int w, int b0, int end) {
    if (end - b0 < 2 * w) return -1;
    double cur = 0;
    for (int i = end - w; i < end; i++) cur += sval[i];
    double s = 0; int tot = 0, ge = 0;
    for (int i = b0; i < b0 + w; i++) s += sval[i];
    for (int i = b0 + w; ; i++) {
        tot++; if (s >= cur - 1e-9) ge++;
        if (i >= end) break;
        s += sval[i] - sval[i - w];
    }
    return (double) ge / tot;
}
static double dot_at(int end, int b0) {
    double a = index_w(60, b0, end), b = index_w(120, b0, end);
    if (a < 0) return b;
    if (b < 0) return a;
    return fabs(b - 0.5) > fabs(a - 0.5) ? b : a;
}
static int band_of(double x) {
    return x < 0.05 ? 0 : x < 0.10 ? 1 : x < 0.40 ? 2 : x < 0.90 ? 3 : x < 0.95 ? 4 : 5;
}
static double dot_now(void) { return sn ? dot_at(sn, 0) : -1; }

/* ---- Jev ---- */
static int ask_jev(double horizon_min, double probs[NBANDS]) {
    if (sn < 120) return -1;
    char state[4096]; int o = 0;
    o += snprintf(state + o, sizeof state - o, "GCP 2.0 global network variance (netvar), the mean of each 10 minutes over the last two hours, oldest first:");
    for (int k = 0; k < 12; k++) {
        double s = 0; for (int i = sn - 120 + k * 10; i < sn - 120 + (k + 1) * 10; i++) s += sval[i];
        o += snprintf(state + o, sizeof state - o, "%s %.1f", k ? "," : "", s / 10);
    }
    o += snprintf(state + o, sizeof state - o, ". The gcpdot index (the share of past windows with a mean network variance at least as high; low = unusually high variance) every 10 minutes over the same two hours:");
    for (int k = 0; k < 12; k++) {
        double x = dot_at(sn - 110 + k * 10, 0);
        o += snprintf(state + o, sizeof state - o, "%s %.2f", k ? "," : "", x);
    }
    o += snprintf(state + o, sizeof state - o, ". Now: %.2f (%s).", dot_now(), band_name[band_of(dot_now())]);
    char choice[1024]; int c = 0;
    c += snprintf(choice + c, sizeof choice - c, "band=Which band will the gcpdot index be in %.0f minutes from now?|", horizon_min < 1 ? 1 : horizon_min);
    for (int b = 0; b < NBANDS; b++) c += snprintf(choice + c, sizeof choice - c, "%s%s:%s", b ? ";" : "", band_name[b], band_desc[b]);
    char cmd[6000];
    snprintf(cmd, sizeof cmd, "timeout 300 '%s/.local/bin/mjev' query --state '%s' --choice '%s' 2>/dev/null", home, state, choice);
    char *out = run_read(cmd, NULL);
    if (!out) return -1;
    char *p = strstr(out, "\"probabilities\"");
    int got = 0;
    if (p) for (int b = 0; b < NBANDS; b++) {
        char key[32]; snprintf(key, sizeof key, "\"%s\":", band_name[b]);
        char *k = strstr(p, key);
        probs[b] = k ? strtod(k + strlen(key), NULL) : 0;
        if (k) got++;
    }
    free(out);
    if (got != NBANDS) return -1;
    double t = 0; for (int b = 0; b < NBANDS; b++) t += probs[b];
    if (t <= 0) return -1;
    for (int b = 0; b < NBANDS; b++) probs[b] /= t;
    return 0;
}
static int draw_band(const double probs[NBANDS]) {
    double u = rnd01(), acc = 0;
    for (int b = 0; b < NBANDS; b++) { acc += probs[b]; if (u < acc) return b; }
    return NBANDS - 1;
}

/* ---- VLC over MPRIS ---- */
static sd_bus *bus;
static char *uri_of(const char *path) {
    size_t n = strlen(path);
    char *u = malloc(n * 3 + 8), *o = u;
    o += sprintf(o, "file://");
    for (const unsigned char *s = (const unsigned char *) path; *s; s++) {
        if (isalnum(*s) || strchr("/-._~", *s)) *o++ = (char) *s;
        else o += sprintf(o, "%%%02X", *s);
    }
    *o = 0;
    return u;
}
struct meta { char trackid[256]; char url[4096]; int64_t length; };
/* Reads one a{sv} (the message positioned at it) into M. */
static int read_meta(sd_bus_message *m, struct meta *md) {
    memset(md, 0, sizeof *md);
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return -1;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *k; sd_bus_message_read(m, "s", &k);
        const char *ct = NULL; char t;
        sd_bus_message_peek_type(m, &t, &ct);
        if (!strcmp(k, "mpris:trackid") && ct && !strcmp(ct, "o")) { const char *v; sd_bus_message_read(m, "v", "o", &v); snprintf(md->trackid, sizeof md->trackid, "%s", v); }
        else if (!strcmp(k, "xesam:url") && ct && !strcmp(ct, "s")) { const char *v; sd_bus_message_read(m, "v", "s", &v); snprintf(md->url, sizeof md->url, "%s", v); }
        else if (!strcmp(k, "mpris:length") && ct && !strcmp(ct, "x")) { int64_t v; sd_bus_message_read(m, "v", "x", &v); md->length = v; }
        else sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    return 0;
}
static int vlc_meta(struct meta *md) {
    sd_bus_message *r = NULL; sd_bus_error e = SD_BUS_ERROR_NULL;
    int ok = sd_bus_get_property(bus, PLAYER, MPATH, "org.mpris.MediaPlayer2.Player", "Metadata", &e, &r, "a{sv}");
    sd_bus_error_free(&e);
    if (ok < 0) return -1;
    ok = read_meta(r, md);
    sd_bus_message_unref(r);
    return ok;
}
static int64_t vlc_position(void) {
    int64_t p = 0; sd_bus_error e = SD_BUS_ERROR_NULL; sd_bus_message *r = NULL;
    if (sd_bus_get_property(bus, PLAYER, MPATH, "org.mpris.MediaPlayer2.Player", "Position", &e, &r, "x") >= 0) { sd_bus_message_read(r, "x", &p); sd_bus_message_unref(r); }
    sd_bus_error_free(&e);
    return p;
}
static int vlc_status(char *s, size_t n) {
    char *v = NULL; sd_bus_error e = SD_BUS_ERROR_NULL;
    int ok = sd_bus_get_property_string(bus, PLAYER, MPATH, "org.mpris.MediaPlayer2.Player", "PlaybackStatus", &e, &v);
    sd_bus_error_free(&e);
    if (ok < 0) return -1;
    snprintf(s, n, "%s", v); free(v);
    return 0;
}
/* The URL of playlist item ID ("" when there is none). */
static int track_url(const char *id, char *url, size_t n) {
    sd_bus_message *r = NULL; sd_bus_error e = SD_BUS_ERROR_NULL;
    url[0] = 0;
    int ok = sd_bus_call_method(bus, PLAYER, MPATH, "org.mpris.MediaPlayer2.TrackList", "GetTracksMetadata", &e, &r, "ao", 1, id);
    sd_bus_error_free(&e);
    if (ok < 0) return -1;
    if (sd_bus_message_enter_container(r, 'a', "a{sv}") > 0) {
        struct meta md;
        if (read_meta(r, &md) == 0) snprintf(url, n, "%s", md.url);
        sd_bus_message_exit_container(r);
    }
    sd_bus_message_unref(r);
    return 0;
}
static long id_num(const char *id) { const char *s = strrchr(id, '/'); return s ? atol(s + 1) : -1; }
static long max_seen_id;
/* Queues URI after the current item; returns its id (VLC's Tracks property
 * stays stale, so the new id is found by its URL among the ids after any
 * seen so far). */
/* The last item of the playlist: VLC numbers items in the order they are
 * added and everything is appended (the file manager's opens and this
 * daemon's songs), so the highest id still present. MPRIS's NoTrack as the
 * position means "first", not "last", which put songs at the top. */
static int vlc_last_id(char *out, size_t n) {
    long best = -1;
    char id[96], url[4096];
    for (long i = max_seen_id + 1, miss = 0; miss < 40; i++) {
        snprintf(id, sizeof id, "/org/videolan/vlc/playlist/%ld", i);
        if (track_url(id, url, sizeof url) == 0 && url[0]) { best = i; miss = 0; } else miss++;
    }
    if (best > max_seen_id) max_seen_id = best;
    for (long i = max_seen_id; best < 0 && i > 0 && i > max_seen_id - 4000; i--) {
        snprintf(id, sizeof id, "/org/videolan/vlc/playlist/%ld", i);
        if (track_url(id, url, sizeof url) == 0 && url[0]) best = i;
    }
    if (best < 0) return -1;
    snprintf(out, n, "/org/videolan/vlc/playlist/%ld", best);
    return 0;
}
static int vlc_queue(const char *uri, const char *after, char *id_out, size_t n) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int ok = sd_bus_call_method(bus, PLAYER, MPATH, "org.mpris.MediaPlayer2.TrackList", "AddTrack", &e, NULL, "sob", uri, after, 0);
    if (ok < 0) { say("AddTrack refused: %s", e.message ? e.message : strerror(-ok)); sd_bus_error_free(&e); return -1; }
    sd_bus_error_free(&e);
    usleep(300000);
    long found = -1;
    for (long i = max_seen_id + 1; i <= max_seen_id + 400; i++) {
        char id[96], url[4096]; snprintf(id, sizeof id, "/org/videolan/vlc/playlist/%ld", i);
        if (track_url(id, url, sizeof url) == 0 && !strcmp(url, uri)) found = i;
    }
    if (found < 0) return -1;
    if (found > max_seen_id) max_seen_id = found;
    snprintf(id_out, n, "/org/videolan/vlc/playlist/%ld", found);
    return 0;
}

/* ---- the daemon ---- */
static FILE *predlog;
static void logline(const char *fmt, ...) {
    if (!predlog) return;
    va_list ap; va_start(ap, fmt); vfprintf(predlog, fmt, ap); va_end(ap); fflush(predlog);
}
static void write_status(const char *fmt, ...) {
    char p[700]; snprintf(p, sizeof p, "%s/status", statedir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void) s; stop = 1; }

static void paths(void) {
    snprintf(home, sizeof home, "%s", getenv("HOME") ? getenv("HOME") : ".");
    const char *xc = getenv("XDG_CONFIG_HOME"), *xs = getenv("XDG_STATE_HOME");
    if (xc && *xc) snprintf(cfgdir, sizeof cfgdir, "%s/gcpdot-music", xc); else snprintf(cfgdir, sizeof cfgdir, "%s/.config/gcpdot-music", home);
    if (xs && *xs) snprintf(statedir, sizeof statedir, "%s/gcpdot-music", xs); else snprintf(statedir, sizeof statedir, "%s/.local/state/gcpdot-music", home);
    char p[700]; snprintf(p, sizeof p, "%s/.local/state", home); mkdir(p, 0755);
    mkdir(statedir, 0755);
}

static int cmd_dot(void) {
    series_load();
    int f = fetch();
    if (f < 0) say("the feed did not answer; the minutes kept on disk only");
    if (sn < 240) { say("only %d minutes of the series", sn); return 1; }
    double x = dot_now();
    printf("minutes kept: %d (%.1f days), latest netvar %.4f at %ld\n", sn, sn / 1440.0, sval[sn - 1], sep[sn - 1]);
    printf("1 h index %.3f, 2 h index %.3f -> dot %.3f: %s\n", index_w(60, 0, sn), index_w(120, 0, sn), x, band_name[band_of(x)]);
    /* The last 24 h, each minute's dot against the same baseline. */
    int cnt[NBANDS] = { 0 }, tot = 0;
    for (int end = sn - 1440 > 240 ? sn - 1440 : 240; end <= sn; end++) { double d = dot_at(end, 0); if (d >= 0) { cnt[band_of(d)]++; tot++; } }
    printf("share of the last %d minutes per band:", tot);
    for (int b = 0; b < NBANDS; b++) printf(" %s %.1f%%", band_name[b], 100.0 * cnt[b] / (tot ? tot : 1));
    printf("\n");
    /* Control: the 1 h index alone, every window against the same full set,
     * is uniform by construction, so the shares must be 5/5/30/50/5/5. */
    int c1[NBANDS] = { 0 }, t1 = 0;
    for (int end = 60; end <= sn; end++) {
        double cur = 0; for (int i = end - 60; i < end; i++) cur += sval[i];
        double s = 0; int tot2 = 0, ge = 0;
        for (int i = 0; i < 60; i++) s += sval[i];
        for (int i = 60; ; i++) { tot2++; if (s >= cur - 1e-9) ge++; if (i >= sn) break; s += sval[i] - sval[i - 60]; }
        c1[band_of((double) ge / tot2)]++; t1++;
    }
    printf("control, 1 h index against all %d windows:", t1);
    for (int b = 0; b < NBANDS; b++) printf(" %s %.1f%%", band_name[b], 100.0 * c1[b] / (t1 ? t1 : 1));
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    paths();
    if (argc > 1 && !strcmp(argv[1], "dot")) return cmd_dot();
    if (argc > 1 && !strcmp(argv[1], "status")) {
        char p[700]; snprintf(p, sizeof p, "%s/status", statedir);
        FILE *f = fopen(p, "r"); if (!f) { printf("not running yet (no %s)\n", p); return 1; }
        char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); fputs(b, stdout);
        return 0;
    }
    if (argc > 1) { fprintf(stderr, "gcpdot-music [dot|status]\n"); return 2; }

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    load_conf();
    scan_tracks();
    series_load();
    char lp[700]; snprintf(lp, sizeof lp, "%s/predictions.tsv", statedir);
    predlog = fopen(lp, "a");
    if (sd_bus_open_user(&bus) < 0) { say("no session bus"); return 1; }
    say("switch %s, Jev %s; %d minutes of the series kept", switch_now ? "now" : "at the next song", use_jev ? "on" : "off", sn);

    time_t last_fetch = 0, last_ok = 0;
    char queued_id[96] = "", queued_uri[4600] = "", last_track[256] = "";
    int queued_band = -1, last_band = -1;
    /* The song this daemon queued, kept on disk: after a restart it is taken
     * over while VLC still holds it at that id, not queued a second time. */
    char qp[700]; snprintf(qp, sizeof qp, "%s/queued", statedir);
    {
        FILE *q = fopen(qp, "r");
        char line[4800];
        if (q && fgets(line, sizeof line, q)) {
            char *id = strtok(line, "\t\n"), *u = strtok(NULL, "\t\n"), *b = strtok(NULL, "\t\n");
            char url[4600];
            if (id && u && b && atoi(b) >= 0 && atoi(b) < NBANDS && track_url(id, url, sizeof url) == 0 && !strcmp(url, u)) {
                snprintf(queued_id, sizeof queued_id, "%s", id);
                snprintf(queued_uri, sizeof queued_uri, "%s", u);
                queued_band = atoi(b);
                if (id_num(id) > max_seen_id) max_seen_id = id_num(id);
                say("taking over the queued %s (band %s)", id, band_name[queued_band]);
            }
        }
        if (q) fclose(q);
    }
    while (!stop) {
        time_t now = time(NULL);
        struct meta md;
        char st[32] = "";
        int have = vlc_meta(&md) == 0 && vlc_status(st, sizeof st) == 0;
        /* Fetched the moment a song ends (the next one is then chosen on the
         * newest minute), at the start, while the feed has not answered (every
         * 30 s), and every 30 min to keep the status line current. */
        int song_changed = have && md.trackid[0] && strcmp(md.trackid, last_track) != 0;
        if ((song_changed && now - last_fetch >= 10) || !last_fetch || (!last_ok && now - last_fetch >= 30) || now - last_fetch >= FETCH_S) {
            int a = fetch();
            last_fetch = now;
            if (a >= 0) { last_ok = now; say("feed%s: %d new minutes, latest netvar %.4f, %ld s old", song_changed ? " (song change)" : "", a, sval[sn - 1], (long) (now - sep[sn - 1])); }
            else say("feed: no answer (keeping the series as it is)");
        }
        double dot = dot_now();
        int band = dot >= 0 ? band_of(dot) : 3;
        int stale = !last_ok || now - last_ok > 3 * FETCH_S;
        if (!have) {
            queued_id[0] = 0; last_track[0] = 0;   /* VLC gone: its ids start again */
        } else {
            long cur = id_num(md.trackid);
            if (cur > max_seen_id) max_seen_id = cur;
            /* Our song started: its band at the start goes in the log. */
            if (queued_id[0] && !strcmp(md.trackid, queued_id)) {
                logline("S\t%ld\t%s\t%.4f\t%s\t%s\n", (long) now, queued_id, dot, band_name[band], band_name[queued_band]);
                say("started (%s, dot now %.3f %s)", band_name[queued_band], dot, band_name[band]);
                queued_id[0] = 0;
                unlink(qp);   /* played: a restart must not take it over */
            }
            /* A song added after ours is playing (opened from the file
             * manager: VLC appends and plays it), so ours sits before it and
             * would never play: taken out, and a new one appended after it.
             * VLC numbers items in the order they are added. */
            if (queued_id[0] && cur > id_num(queued_id)) {
                sd_bus_error e = SD_BUS_ERROR_NULL;
                sd_bus_call_method(bus, PLAYER, MPATH, "org.mpris.MediaPlayer2.TrackList", "RemoveTrack", &e, NULL, "o", queued_id);
                sd_bus_error_free(&e);
                say("%s plays, added after the queued %s: that one taken out, a new one appended", md.trackid, queued_id);
                queued_id[0] = 0;
                unlink(qp);
            }
            /* Gone from the playlist (replaced or removed): queued again. */
            if (queued_id[0]) {
                char url[4600];
                if (track_url(queued_id, url, sizeof url) == 0 && strcmp(url, queued_uri)) { queued_id[0] = 0; unlink(qp); }
            }
            /* "now": a band change cuts to the new band. */
            if (switch_now && !stale && last_band >= 0 && band != last_band && strcmp(st, "Playing") == 0) {
                const char *t = pick_track(band);
                if (t) {
                    char *u = uri_of(t);
                    sd_bus_error e = SD_BUS_ERROR_NULL;
                    sd_bus_call_method(bus, PLAYER, MPATH, "org.mpris.MediaPlayer2.Player", "OpenUri", &e, NULL, "s", u);
                    sd_bus_error_free(&e);
                    say("band %s -> %s: %s", band_name[last_band], band_name[band], t);
                    logline("N\t%ld\t%.4f\t%s\t%s\n", (long) now, dot, band_name[band], t);
                    free(u);
                    queued_id[0] = 0;
                }
            }
            if (!queued_id[0] && md.trackid[0] && strcmp(st, "Stopped") != 0 && sn >= 240) {
                double left = md.length > 0 ? (double) (md.length - vlc_position()) / 60e6 : 4;
                if (left < 0) left = 0;
                double probs[NBANDS] = { 0 };
                int from_jev = use_jev && ask_jev(left, probs) == 0;
                if (!from_jev) { for (int b = 0; b < NBANDS; b++) probs[b] = 0; probs[band] = 1; }
                int nb = draw_band(probs);
                const char *t = pick_track(nb);
                if (!t) { say("band %s has no songs; using green", band_name[nb]); nb = 3; t = pick_track(nb); }
                if (t) {
                    char *u = uri_of(t);
                    char id[96];
                    /* At the bottom of the playlist: after its last item. */
                    char last[96];
                    if (vlc_last_id(last, sizeof last) != 0) snprintf(last, sizeof last, "%s", md.trackid);
                    if (vlc_queue(u, last, id, sizeof id) == 0) {
                        snprintf(queued_id, sizeof queued_id, "%s", id);
                        snprintf(queued_uri, sizeof queued_uri, "%s", u);
                        queued_band = nb;
                        FILE *q = fopen(qp, "w");
                        if (q) { fprintf(q, "%s\t%s\t%d\n", id, u, nb); fclose(q); }
                        logline("Q\t%ld\t%s\t%.1f\t%.4f\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%s\t%s\t%ld\n", (long) now, id, left, dot,
                                band_name[band], from_jev ? "jev" : "dot", probs[0], probs[1], probs[2], probs[3], probs[4], probs[5], band_name[nb], t,
                                sn ? (long) (now - sep[sn - 1]) : -1L);
                        say("queued at the end (%s plays, %.1f min left): %s band %s [%s %.2f %.2f %.2f %.2f %.2f %.2f] %s", md.trackid, left,
                            from_jev ? "Jev" : "dot", band_name[nb], from_jev ? "Jev" : "dot", probs[0], probs[1], probs[2], probs[3], probs[4], probs[5], t);
                    } else say("could not queue %s", t);
                    free(u);
                }
            }
            snprintf(last_track, sizeof last_track, "%s", md.trackid);
        }
        last_band = band;
        write_status("dot %.3f (%s)%s, latest netvar %.4f, %d minutes kept\nVLC: %s%s\nqueued: %s%s%s\nswitch %s, Jev %s\n",
                     dot, band_name[band], stale ? " STALE" : "", sn ? sval[sn - 1] : 0, sn,
                     have ? st : "not running", have ? "" : "", queued_id[0] ? queued_id : "nothing", queued_id[0] ? " band " : "",
                     queued_id[0] ? band_name[queued_band] : "", switch_now ? "now" : "next", use_jev ? "on" : "off");
        for (int i = 0; i < LOOP_S * 10 && !stop; i++) usleep(100000);
    }
    say("stopping");
    sd_bus_unref(bus);
    return 0;
}
