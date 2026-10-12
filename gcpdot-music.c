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
#include <pthread.h>

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
/* Skips and replays per song (prefs.tsv: URL, skips, replays), counted by
 * the main loop from what VLC plays; a song's weight in the draw is
 * exp(0.6 * replays - 0.8 * skips), never under 0.05. */
struct pref { char *url; int skips, replays; };
static struct pref *prefs; static int npref, cpref;
static char *uri_of(const char *path);
static struct pref *pref_get(const char *url, int make) {
    for (int i = 0; i < npref; i++) if (!strcmp(prefs[i].url, url)) return &prefs[i];
    if (!make) return NULL;
    if (npref == cpref) { cpref = cpref ? cpref * 2 : 256; prefs = realloc(prefs, cpref * sizeof *prefs); }
    prefs[npref] = (struct pref) { strdup(url), 0, 0 };
    return &prefs[npref++];
}
static void prefs_path(char *p, size_t n) { snprintf(p, n, "%s/prefs.tsv", statedir); }
static void prefs_load(void) {
    char p[700]; prefs_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[4800];
    while (fgets(line, sizeof line, f)) {
        char *u = strtok(line, "\t\n"), *s = strtok(NULL, "\t\n"), *r = strtok(NULL, "\t\n");
        if (u && s && r) { struct pref *q = pref_get(u, 1); q->skips = atoi(s); q->replays = atoi(r); }
    }
    fclose(f);
}
static void prefs_save(void) {
    char p[700], t[720]; prefs_path(p, sizeof p); snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w");
    if (!f) return;
    for (int i = 0; i < npref; i++) fprintf(f, "%s\t%d\t%d\n", prefs[i].url, prefs[i].skips, prefs[i].replays);
    fclose(f);
    rename(t, p);
}
static double pref_weight(const char *path) {
    char *u = uri_of(path);
    struct pref *q = pref_get(u, 0);
    free(u);
    if (!q) return 1;
    double w = exp(0.6 * q->replays - 0.8 * q->skips);
    return w < 0.05 ? 0.05 : w;
}
static const char *pick_track(int b) {
    struct list *l = &tracks[b];
    if (!l->n) return NULL;
    int keep = l->n / 2 < RECENT ? l->n / 2 : RECENT;
    /* Weighted draw (RDSEED) over the band's songs not among its recent
     * ones; all of them when every one is recent. */
    double *w = malloc(l->n * sizeof *w), tot = 0;
    for (int pass = 0; pass < 2 && tot <= 0; pass++)
        for (int k = 0; k < l->n; k++) {
            int seen = 0;
            if (!pass) for (int i = 0; i < recent_n[b] && i < keep; i++) if (recent[b][i] && !strcmp(recent[b][i], l->p[k])) { seen = 1; break; }
            w[k] = seen ? 0 : pref_weight(l->p[k]);
            tot += w[k];
        }
    double u = rnd01() * tot, acc = 0;
    int k = l->n - 1;
    for (int i = 0; i < l->n; i++) { acc += w[i]; if (u < acc) { k = i; break; } }
    free(w);
    const char *t = l->p[k];
    free(recent[b][RECENT - 1]);
    memmove(&recent[b][1], &recent[b][0], (RECENT - 1) * sizeof(char *));
    recent[b][0] = strdup(t);
    if (recent_n[b] < RECENT) recent_n[b]++;
    return t;
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
            /* "Too Many Attempts.": gcp2.net's rate limit, on the one public
             * token every visitor's browser shares, so it comes and goes with
             * their traffic: waited out (5, 15, 30 s), the token kept. Any
             * other answer: the token fetched again, once. */
            int limited = strstr(j, "Too Many Attempts") != NULL;
            free(j);
            static const int wait_s[3] = { 5, 15, 30 };
            if (!limited && attempt == 0) get_token();
            if (attempt < 3) { sleep(limited ? wait_s[attempt] : 3); continue; }
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

/* ---- the reference egg: this machine's RNG through GCP 1.0's math ----
 * One trial a second, the sum of 200 bits from RDSEED (a GCP egg's trial:
 * mean 100, variance 50), z = (trial - 100) / sqrt(50). The bits are
 * independent, so a window's sum of z^2 over k seconds is chi-square with k
 * degrees of freedom and its index is the exact upper-tail probability, the
 * original dot's math; the 1 h and 2 h indexes, the one farther from 0.5.
 * Raw trials per day in egg-YYYYMMDD.tsv (epoch, trial), minutes in
 * egg-min.tsv (minute end, trials, sum z, sum z^2). It is the null
 * reference beside GCP 2.0's network: the two are independent sources, so
 * their dots should agree no better than chance. */
static long emin[MAXMIN]; static int en[MAXMIN]; static double ez[MAXMIN], ez2[MAXMIN]; static int esn;
static pthread_mutex_t emu = PTHREAD_MUTEX_INITIALIZER;
static double egg_last_z; static long egg_trials;

static void egg_push(long m, int n, double z, double z2) {
    if (esn == MAXMIN) {
        memmove(emin, emin + 1, (MAXMIN - 1) * sizeof *emin); memmove(en, en + 1, (MAXMIN - 1) * sizeof *en);
        memmove(ez, ez + 1, (MAXMIN - 1) * sizeof *ez); memmove(ez2, ez2 + 1, (MAXMIN - 1) * sizeof *ez2); esn--;
    }
    emin[esn] = m; en[esn] = n; ez[esn] = z; ez2[esn] = z2; esn++;
}
static void egg_load(void) {
    char p[700]; snprintf(p, sizeof p, "%s/egg-min.tsv", statedir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    long m; int n; double z, z2;
    while (fscanf(f, "%ld %d %lf %lf", &m, &n, &z, &z2) == 4) if (!esn || m > emin[esn - 1]) egg_push(m, n, z, z2);
    fclose(f);
}
static int popcount200(void) {
    int c = 0;
    for (int i = 0; i < 3; i++) c += __builtin_popcountll(rdseed64());
    return c + __builtin_popcountll(rdseed64() & ((1ULL << 8) - 1));   /* 64 * 3 + 8 = 200 bits */
}
static void *egg_thread(void *arg) {
    (void) arg;
    long cur_m = 0; int n = 0; double sz = 0, sz2 = 0;
    FILE *raw = NULL; char raw_day[16] = "";
    char mp[700]; snprintf(mp, sizeof mp, "%s/egg-min.tsv", statedir);
    for (;;) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1; ts.tv_nsec = 0;
        while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
        long t = ts.tv_sec;
        int trial = popcount200();
        double z = (trial - 100) / sqrt(50.0);
        char day[16]; time_t tt = t; strftime(day, sizeof day, "%Y%m%d", gmtime(&tt));
        if (strcmp(day, raw_day)) {
            if (raw) fclose(raw);
            char rp[720]; snprintf(rp, sizeof rp, "%s/egg-%s.tsv", statedir, day);
            raw = fopen(rp, "a"); snprintf(raw_day, sizeof raw_day, "%s", day);
        }
        if (raw) { fprintf(raw, "%ld\t%d\n", t, trial); if (t % 60 == 0) fflush(raw); }
        long m = (t / 60) * 60 + 60;   /* the minute this second ends in, as GCP 2.0's end_epoch */
        if (m != cur_m) {
            if (cur_m && n) {
                pthread_mutex_lock(&emu); egg_push(cur_m, n, sz, sz2); pthread_mutex_unlock(&emu);
                FILE *f = fopen(mp, "a");
                if (f) { fprintf(f, "%ld %d %.6f %.6f\n", cur_m, n, sz, sz2); fclose(f); }
            }
            cur_m = m; n = 0; sz = sz2 = 0;
        }
        n++; sz += z; sz2 += z * z;
        egg_last_z = z; egg_trials++;
    }
    return NULL;
}
/* P(chi-square with k degrees of freedom >= x), Wilson-Hilferty (for the
 * k of 3600 and more here, within about 1e-5 of the exact tail). */
static double chi2_upper(double x, double k) {
    if (k <= 0) return -1;
    double h = 2.0 / (9.0 * k);
    double zz = (cbrt(x / k) - (1.0 - h)) / sqrt(h);
    return 0.5 * erfc(zz / sqrt(2.0));
}
/* The egg's index over the W minutes ending at minute END (an end_epoch),
 * -1 when under 80% of those seconds were recorded. Lock held by caller. */
static double egg_index_w(int w, long end) {
    double s2 = 0; long k = 0;
    for (int i = esn - 1; i >= 0; i--) {
        if (emin[i] > end) continue;
        if (emin[i] <= end - (long) w * 60) break;
        s2 += ez2[i]; k += en[i];
    }
    if (k < (long) (0.8 * w * 60)) return -1;
    return chi2_upper(s2, (double) k);
}
static double egg_dot_at(long end) {
    pthread_mutex_lock(&emu);
    double a = egg_index_w(60, end), b = egg_index_w(120, end);
    pthread_mutex_unlock(&emu);
    if (a < 0) return b;
    if (b < 0) return a;
    return fabs(b - 0.5) > fabs(a - 0.5) ? b : a;
}
static long egg_latest(void) { pthread_mutex_lock(&emu); long m = esn ? emin[esn - 1] : 0; pthread_mutex_unlock(&emu); return m; }

/* ---- forecasting the dot from its own history ----
 * The dot is a one- and two-hour window moving a minute at a time, so where
 * it will be a few minutes on shows in where it is and where it is heading.
 * gdot[] holds the dot of every minute as it was computed then (baseline:
 * the minutes before it). The analog forecast for h minutes ahead weighs
 * every past minute by how close its dot and 10-minute slope are to now's
 * (Gaussian, 0.05 wide in each) and counts the band its dot was in h minutes
 * after, only over pairs whose outcome was already known. */
static double gdot[MAXMIN]; static long gdot_ep[MAXMIN]; static int gdn;
static void dots_sync(void) {
    if (gdn && sn && gdot_ep[0] != sep[0]) {   /* the series dropped its oldest minutes */
        int off = 0;
        while (off < gdn && gdot_ep[off] != sep[0]) off++;
        if (off >= gdn) gdn = 0;
        else { memmove(gdot, gdot + off, (gdn - off) * sizeof *gdot); memmove(gdot_ep, gdot_ep + off, (gdn - off) * sizeof *gdot_ep); gdn -= off; }
    }
    for (; gdn < sn; gdn++) { gdot[gdn] = dot_at(gdn + 1, 0); gdot_ep[gdn] = sep[gdn]; }
}
#define SLOPE_M 10
#define KW 0.05
/* Bands h minutes after minute T, from the minutes up to T (outcomes known
 * by then). Returns the effective number of analogs, 0 when none. */
static double analog(int t, int h, double p[NBANDS]) {
    for (int b = 0; b < NBANDS; b++) p[b] = 0;
    if (t < SLOPE_M || gdot[t] < 0 || gdot[t - SLOPE_M] < 0) return 0;
    double x = gdot[t], s = gdot[t] - gdot[t - SLOPE_M], w[NBANDS] = { 0 }, sw = 0, sw2 = 0;
    for (int i = SLOPE_M; i + h <= t; i++) {
        if (gdot[i] < 0 || gdot[i - SLOPE_M] < 0 || gdot[i + h] < 0) continue;
        double dx = (gdot[i] - x) / KW, ds = (gdot[i] - gdot[i - SLOPE_M] - s) / KW;
        double wt = exp(-0.5 * (dx * dx + ds * ds));
        w[band_of(gdot[i + h])] += wt; sw += wt; sw2 += wt * wt;
    }
    if (sw < 1e-12) return 0;
    double tot = 0;
    for (int b = 0; b < NBANDS; b++) { p[b] = w[b] + 0.05 * sw / NBANDS + 1e-6; tot += p[b]; }   /* a little mass on every band */
    for (int b = 0; b < NBANDS; b++) p[b] /= tot;
    return sw * sw / sw2;
}

/* The analog forecast anchored on persistence: the band now gets the share
 * of past pairs h minutes apart that stayed in one band, the rest goes to
 * the other bands in the analogs' proportions. On the first stored day the
 * plain analog picked the top band worse than persistence (88.8 against
 * 94.7% two minutes ahead) while catching a quarter of the band changes;
 * anchored, its top band is persistence's and its change odds the analogs'. */
static double anchored(int t, int h, double p[NBANDS]) {
    double pa[NBANDS];
    double neff = analog(t, h, pa);
    if (neff < 3) return neff;
    int same = 0, all = 0;
    for (int i = 0; i + h <= t; i++) {
        if (gdot[i] < 0 || gdot[i + h] < 0) continue;
        all++; same += band_of(gdot[i]) == band_of(gdot[i + h]);
    }
    if (!all) return 0;
    double s = (same + 1.0) / (all + 2.0);
    int nb = band_of(gdot[t]);
    double rest = 1 - pa[nb];
    for (int b = 0; b < NBANDS; b++) p[b] = b == nb ? s : (rest > 1e-9 ? (1 - s) * pa[b] / rest : (1 - s) / (NBANDS - 1));
    return neff;
}

/* Resolved forecasts (predictions.tsv: a Q line joined to the S line of its
 * song), for scoring, for refitting the blend and as feedback to Jev. */
struct res { long t; int now_b, act, has_j, has_a, has_f; double pj[NBANDS], pa[NBANDS], pf[NBANDS]; double h; };
static struct res *rs; static int rn, rcap;
static double blend_a = 0.5, blend_c = 1.0;   /* p ∝ Jev^a * analog^c until refitted */
static int band_idx(const char *s) { for (int b = 0; b < NBANDS; b++) if (!strcmp(s, band_name[b])) return b; return -1; }
static void load_resolved(void) {
    rn = 0;
    char p[700]; snprintf(p, sizeof p, "%s/predictions.tsv", statedir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    struct res pend[64]; char pid[64][96]; int pn = 0;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char *fld[40]; int nf = 0;
        for (char *s = strtok(line, "\t\n"); s && nf < 40; s = strtok(NULL, "\t\n")) fld[nf++] = s;
        if (nf >= 15 && !strcmp(fld[0], "Q")) {
            struct res r; memset(&r, 0, sizeof r);
            r.t = atol(fld[1]); r.h = atof(fld[3]); r.now_b = band_idx(fld[5]);
            r.has_j = !strcmp(fld[6], "jev");
            for (int b = 0; b < NBANDS; b++) r.pj[b] = atof(fld[7 + b]);
            if (nf >= 28) { r.has_a = 1; r.has_f = 1; for (int b = 0; b < NBANDS; b++) { r.pa[b] = atof(fld[16 + b]); r.pf[b] = atof(fld[22 + b]); } }
            if (r.has_a) { double t = 0; for (int b = 0; b < NBANDS; b++) t += r.pa[b]; if (t <= 0) r.has_a = r.has_f = 0; }
            if (pn == 64) { memmove(pend, pend + 1, 63 * sizeof *pend); memmove(pid, pid + 1, 63 * sizeof *pid); pn = 63; }
            int k = pn++;
            pend[k] = r; snprintf(pid[k], sizeof pid[k], "%s", fld[2]);
        } else if (nf >= 5 && !strcmp(fld[0], "S")) {
            for (int k = pn - 1; k >= 0; k--) if (!strcmp(pid[k], fld[2])) {
                struct res r = pend[k]; r.act = band_idx(fld[4]);
                if (r.act >= 0 && r.now_b >= 0) {
                    if (rn == rcap) { rcap = rcap ? rcap * 2 : 256; rs = realloc(rs, rcap * sizeof *rs); }
                    rs[rn++] = r;
                }
                memmove(pend + k, pend + k + 1, (pn - k - 1) * sizeof *pend); memmove(pid + k, pid + k + 1, (pn - k - 1) * sizeof *pid); pn--;
                break;
            }
        }
    }
    fclose(f);
}
static void pool(const double pj[NBANDS], int has_j, const double pa[NBANDS], int has_a, double a, double c, double out[NBANDS]) {
    double t = 0;
    for (int b = 0; b < NBANDS; b++) {
        double lj = has_j ? log(pj[b] > 1e-4 ? pj[b] : 1e-4) : 0, la = has_a ? log(pa[b] > 1e-4 ? pa[b] : 1e-4) : 0;
        out[b] = exp((has_j ? a : 0) * lj + (has_a ? c : 0) * la);
        t += out[b];
    }
    for (int b = 0; b < NBANDS; b++) out[b] = t > 0 ? out[b] / t : 1.0 / NBANDS;
}
/* The exponents that gave the lowest mean log loss on every resolved
 * forecast holding both parts (grid 0..2 by 0.25); defaults below 10. */
static void refit(void) {
    int n = 0;
    for (int i = 0; i < rn; i++) if (rs[i].has_j && rs[i].has_a) n++;
    if (n < 10) return;
    double best = 1e9, ba = blend_a, bc = blend_c;
    for (int ai = 0; ai <= 8; ai++) for (int ci = 0; ci <= 8; ci++) {
        if (!ai && !ci) continue;
        double a = ai * 0.25, c = ci * 0.25, loss = 0, q[NBANDS];
        for (int i = 0; i < rn; i++) if (rs[i].has_j && rs[i].has_a) {
            pool(rs[i].pj, 1, rs[i].pa, 1, a, c, q);
            loss -= log(q[rs[i].act] > 1e-6 ? q[rs[i].act] : 1e-6);
        }
        if (loss < best) { best = loss; ba = a; bc = c; }
    }
    blend_a = ba; blend_c = bc;
    say("blend refitted on %d resolved forecasts: Jev^%.2f * analog^%.2f, mean log loss %.3f", n, ba, bc, best / n);
}
/* Jev's recent record, for its state: its top band and probability, and
 * the band at the song's start. */
static void feedback(char *out, size_t n) {
    out[0] = 0;
    int k = 0, o = 0;
    for (int i = rn - 1; i >= 0 && k < 8; i--) {
        if (!rs[i].has_j) continue;
        int top = 0; for (int b = 1; b < NBANDS; b++) if (rs[i].pj[b] > rs[i].pj[top]) top = b;
        o += snprintf(out + o, n - o, "%s %.0f min ahead: predicted %s %.2f, actual %s", k ? ";" : "", rs[i].h, band_name[top], rs[i].pj[top], band_name[rs[i].act]);
        k++;
    }
}

/* ---- Jev ---- */
/* S as one single-quoted shell word. */
static void shq(const char *s, char *out, size_t n) {
    size_t o = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[o++] = '\'';
    for (; *s && o + 6 < n; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    out[o++] = '\''; out[o] = 0;
}
/* DEADLINE_S: Jev must answer within it (the song playing ends soon after),
 * else the caller queues on the dot's band now. */
static int ask_jev(double horizon_min, int deadline_s, double probs[NBANDS]) {
    if (sn < 120) return -1;
    char state[8192]; int o = 0;
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
    /* The reference: this machine's hardware RNG through the same math, at
     * the same minutes (independent of the network: chance agreement). */
    double rnow = egg_dot_at(sep[sn - 1]);
    if (rnow >= 0) {
        o += snprintf(state + o, sizeof state - o, " Reference (this machine's own hardware random generator, one 200-bit trial a second, the same index with an exact chi-square; independent of the network) every 10 minutes over the same two hours:");
        for (int k = 0; k < 12; k++) {
            double x = egg_dot_at(sep[sn - 111 + k * 10]);
            if (x >= 0) o += snprintf(state + o, sizeof state - o, "%s %.2f", k ? "," : "", x);
            else o += snprintf(state + o, sizeof state - o, "%s none", k ? "," : "");
        }
        o += snprintf(state + o, sizeof state - o, ". Reference now: %.2f (%s).", rnow, band_name[band_of(rnow)]);
    }
    /* Its own recent record, so a miss can move the next forecast. */
    char fb[1500]; feedback(fb, sizeof fb);
    if (fb[0]) o += snprintf(state + o, sizeof state - o, " Your recent forecasts of this (your top band and its probability, then the band when that song started):%s.", fb);
    char choice[1024]; int c = 0;
    c += snprintf(choice + c, sizeof choice - c, "band=Which band will the gcpdot index be in %.0f minutes from now?|", horizon_min < 1 ? 1 : horizon_min);
    for (int b = 0; b < NBANDS; b++) c += snprintf(choice + c, sizeof choice - c, "%s%s:%s", b ? ";" : "", band_name[b], band_desc[b]);
    char cmd[16000];
    /* Single-quoted for the shell, a quote inside as '\'' (the state's
     * "machine's" broke every question for an hour without it). */
    char qs[12000], qc[2400];
    shq(state, qs, sizeof qs); shq(choice, qc, sizeof qc);
    snprintf(cmd, sizeof cmd, "timeout %d '%s/.local/bin/mjev' query --state %s --choice %s 2>/dev/null", deadline_s, home, qs, qc);
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

/* GCP 2.0's dot against the reference egg's, minute by minute
 * (compare.tsv: minute end, GCP netvar, GCP dot, reference dot, both bands),
 * over the last 24 h: correlation, band agreement against what chance gives
 * from the two sets of band shares, mean difference. */
static int cmd_compare(void) {
    char p[700]; snprintf(p, sizeof p, "%s/compare.tsv", statedir);
    FILE *f = fopen(p, "r");
    if (!f) { printf("no comparison yet (%s): the daemon writes it once the reference has an hour\n", p); return 1; }
    static double g[1440], r[1440]; long last = 0; int n = 0, head = 0;
    long e; double nv, gd, rd; char gb[16], rb[16];
    while (fscanf(f, "%ld %lf %lf %lf %15s %15s", &e, &nv, &gd, &rd, gb, rb) == 6) {
        g[head] = gd; r[head] = rd; head = (head + 1) % 1440; if (n < 1440) n++; last = e;
    }
    fclose(f);
    if (n < 2) { printf("only %d minutes compared\n", n); return 1; }
    double mg = 0, mr = 0; for (int i = 0; i < n; i++) { mg += g[i]; mr += r[i]; } mg /= n; mr /= n;
    double sgg = 0, srr = 0, sgr = 0, ad = 0; int agree = 0, cg[NBANDS] = { 0 }, cr[NBANDS] = { 0 };
    for (int i = 0; i < n; i++) {
        sgg += (g[i] - mg) * (g[i] - mg); srr += (r[i] - mr) * (r[i] - mr); sgr += (g[i] - mg) * (r[i] - mr);
        ad += fabs(g[i] - r[i]); agree += band_of(g[i]) == band_of(r[i]); cg[band_of(g[i])]++; cr[band_of(r[i])]++;
    }
    double chance = 0; for (int b = 0; b < NBANDS; b++) chance += (double) cg[b] / n * cr[b] / n;
    time_t lt = last; char ts[32]; strftime(ts, sizeof ts, "%F %H:%M", localtime(&lt));
    printf("minutes compared: %d (up to %s)\n", n, ts);
    printf("dot correlation (Pearson r): %.3f\n", (sgg > 0 && srr > 0) ? sgr / sqrt(sgg * srr) : 0);
    printf("same band: %.1f%% (chance from the band shares: %.1f%%)\n", 100.0 * agree / n, 100 * chance);
    printf("mean |GCP dot - reference dot|: %.3f\n", ad / n);
    printf("band shares  GCP:"); for (int b = 0; b < NBANDS; b++) printf(" %s %.1f%%", band_name[b], 100.0 * cg[b] / n);
    printf("\n             reference:"); for (int b = 0; b < NBANDS; b++) printf(" %s %.1f%%", band_name[b], 100.0 * cr[b] / n);
    printf("\nThe minutes overlap (a 1 h window moves one minute at a time), so the\nindependent samples are far fewer than the minutes: read r and agreement\nas a running picture, not a significance test.\n");
    return 0;
}

static int top_of(const double p[NBANDS]) { int t = 0; for (int b = 1; b < NBANDS; b++) if (p[b] > p[t]) t = b; return t; }
static double ll_of(const double p[NBANDS], int a) { return -log(p[a] > 1e-6 ? p[a] : 1e-6); }

/* Every resolved forecast scored: the top band right, and the mean log loss
 * (lower is better; a fair six-way guess is 1.79), for persistence (the band
 * when asked), Jev, the analog model and the blend; and the same on the
 * songs where the band changed, the only ones persistence gets wrong. */
/* ---- the display: the GCP dot now and the predicted one ----
 * The daemon writes dots (key=value) every loop; `gcpdot-music show` draws
 * it in a terminal, the dots in the old gcpdot's colours (quartzjer's table
 * of gcpdot.com's: by index, red for a low one through green to blue). */
static const struct { double at; unsigned rgb; } dot_col[] = {
    { 0.00, 0xCDCDCD }, { 0.01, 0xFFA8C0 }, { 0.05, 0xFF1E1E }, { 0.08, 0xFFB82E }, { 0.15, 0xFFD517 }, { 0.23, 0xFFFA40 },
    { 0.30, 0xF9FA00 }, { 0.40, 0xAEFA00 }, { 0.90, 0x64FA64 }, { 0.9125, 0x64FAAB }, { 0.93, 0xACF2FF }, { 0.95, 0x0EEEFF }, { 1.00, 0x24CBFD },
};
static unsigned colour_of(double x) {
    unsigned c = dot_col[0].rgb;
    for (size_t i = 0; i < sizeof dot_col / sizeof *dot_col; i++) if (x >= dot_col[i].at) c = dot_col[i].rgb;
    return c;
}
/* A band's colour: the colour at the middle of its index range. */
static const double band_mid[NBANDS] = { 0.03, 0.07, 0.25, 0.65, 0.925, 0.97 };

static void write_dots(double gcp, double ref, const double pf[NBANDS], int have_pred, long pred_at, int pred_src_jev) {
    char p[700], t[720]; snprintf(p, sizeof p, "%s/dots", statedir); snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w");
    if (!f) return;
    fprintf(f, "time=%ld\ngcp=%.4f\nlatest_minute=%ld\nref=%.4f\n", (long) time(NULL), gcp, sn ? sep[sn - 1] : 0L, ref);
    if (have_pred) {
        fprintf(f, "pred_at=%ld\npred_src=%s\npred=", pred_at, pred_src_jev ? "Jev + analog" : "analog");
        for (int b = 0; b < NBANDS; b++) fprintf(f, "%s%.4f", b ? " " : "", pf[b]);
        fprintf(f, "\n");
    }
    if (rn) {
        struct res *r = &rs[rn - 1];
        fprintf(f, "last_pred=%s\nlast_actual=%s\n", band_name[r->has_f ? top_of(r->pf) : r->has_j ? top_of(r->pj) : r->now_b], band_name[r->act]);
    }
    fclose(f);
    rename(t, p);
}

/* A filled circle, D pixels across, two pixels a cell (upper half block),
 * drawn at the cursor's row and column COL. */
static void draw_disc(int row, int col, int d, unsigned rgb, unsigned bg) {
    double r = d / 2.0;
    for (int y = 0; y < d; y += 2) {
        printf("\033[%d;%dH", row + y / 2, col);
        for (int x = 0; x < d; x++) {
            double dx = x + 0.5 - r, dy0 = y + 0.5 - r, dy1 = y + 1.5 - r;
            unsigned top = dx * dx + dy0 * dy0 <= r * r ? rgb : bg, bot = dx * dx + dy1 * dy1 <= r * r ? rgb : bg;
            printf("\033[38;2;%u;%u;%um\033[48;2;%u;%u;%um▀", top >> 16, (top >> 8) & 255, top & 255, bot >> 16, (bot >> 8) & 255, bot & 255);
        }
    }
    printf("\033[0m");
}

static int cmd_show(void) {
    char p[700]; snprintf(p, sizeof p, "%s/dots", statedir);
    printf("\033]0;GCP dot\007\033[?25l");   /* the window title (the KWin rule matches it), cursor off */
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    const unsigned bg = 0x0A0A0F;
    while (!stop) {
        double gcp = -1, ref = -1, pred[NBANDS] = { 0 }; long t = 0, latest = 0, pred_at = 0; int havep = 0;
        char src[32] = "", lastp[16] = "", lasta[16] = "";
        FILE *f = fopen(p, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                if (sscanf(line, "time=%ld", &t) == 1 || sscanf(line, "gcp=%lf", &gcp) == 1 || sscanf(line, "latest_minute=%ld", &latest) == 1 ||
                    sscanf(line, "ref=%lf", &ref) == 1 || sscanf(line, "pred_at=%ld", &pred_at) == 1) continue;
                if (sscanf(line, "pred_src=%31[^\n]", src) == 1) continue;
                if (sscanf(line, "last_pred=%15s", lastp) == 1 || sscanf(line, "last_actual=%15s", lasta) == 1) continue;
                if (!strncmp(line, "pred=", 5)) havep = sscanf(line + 5, "%lf %lf %lf %lf %lf %lf", &pred[0], &pred[1], &pred[2], &pred[3], &pred[4], &pred[5]) == 6;
            }
            fclose(f);
        }
        long now = time(NULL);
        printf("\033[48;2;10;10;15m\033[2J\033[H\033[38;2;196;148;90m gcp dot\033[0m");
        if (!f || now - t > 30) printf("\033[38;2;138;106;62m  (daemon not writing: systemctl --user status gcpdot-music)\033[0m");
        /* the two dots */
        int pb = havep ? top_of(pred) : -1;
        draw_disc(3, 3, 12, gcp >= 0 ? colour_of(gcp) : 0x333333, bg);
        draw_disc(3, 20, 12, pb >= 0 ? colour_of(band_mid[pb]) : 0x333333, bg);
        printf("\033[48;2;10;10;15m\033[38;2;220;220;220m");
        printf("\033[10;3H now  %s", gcp >= 0 ? "" : "-");
        if (gcp >= 0) printf("%.3f %s", gcp, band_name[band_of(gcp)]);
        printf("\033[10;20H next ");
        if (pb >= 0) printf("%s %.0f%%", band_name[pb], 100 * pred[pb]); else printf("-");
        printf("\033[11;3H\033[38;2;138;106;62m%ld min old", latest ? (now - latest) / 60 : 0);
        if (pb >= 0) {
            long in = pred_at - now;
            printf("\033[11;20H%s", in > 0 ? "" : "due ");
            if (in > 0) printf("in %ld:%02ld", in / 60, in % 60);
            /* the forecast per band */
            for (int b = 0; b < NBANDS; b++) {
                unsigned c = colour_of(band_mid[b]); int w = (int) (pred[b] * 20 + 0.5);
                printf("\033[%d;3H\033[38;2;150;150;150m%-6s \033[38;2;%u;%u;%um", 13 + b, band_name[b], c >> 16, (c >> 8) & 255, c & 255);
                for (int i = 0; i < w; i++) printf("█");
                printf("\033[38;2;150;150;150m %.2f", pred[b]);
            }
            printf("\033[19;3H\033[38;2;138;106;62m%s", src);
        }
        printf("\033[20;3H\033[38;2;150;150;150mreference ");
        if (ref >= 0) { unsigned c = colour_of(ref); printf("\033[38;2;%u;%u;%um●\033[38;2;150;150;150m %.3f %s", c >> 16, (c >> 8) & 255, c & 255, ref, band_name[band_of(ref)]); }
        else printf("warming up");
        if (lastp[0]) printf("\033[21;3Hlast: %s, was %s %s", lastp, lasta, strcmp(lastp, lasta) ? "\033[38;2;255;80;80mmiss" : "\033[38;2;100;250;100mhit");
        printf("\033[0m");
        fflush(stdout);
        for (int i = 0; i < 20 && !stop; i++) usleep(100000);
    }
    printf("\033[0m\033[?25h\033[2J\033[H");
    return 0;
}

static int cmd_score(void) {
    load_resolved();
    if (!rn) { printf("no resolved forecasts yet (%s/predictions.tsv)\n", statedir); return 1; }
    struct { const char *name; int n, ok, xn, xok; double ll; } m[4] = { { "persistence" }, { "Jev" }, { "analog" }, { "blend" } };
    for (int i = 0; i < rn; i++) {
        struct res *r = &rs[i];
        int cross = r->now_b != r->act;
        m[0].n++; m[0].ok += r->now_b == r->act; m[0].xn += cross;
        if (r->has_j) { int ok = top_of(r->pj) == r->act; m[1].n++; m[1].ok += ok; m[1].ll += ll_of(r->pj, r->act); m[1].xn += cross; m[1].xok += cross && ok; }
        if (r->has_a) { int ok = top_of(r->pa) == r->act; m[2].n++; m[2].ok += ok; m[2].ll += ll_of(r->pa, r->act); m[2].xn += cross; m[2].xok += cross && ok; }
        if (r->has_f) { int ok = top_of(r->pf) == r->act; m[3].n++; m[3].ok += ok; m[3].ll += ll_of(r->pf, r->act); m[3].xn += cross; m[3].xok += cross && ok; }
    }
    printf("%-12s %6s %10s %9s %15s\n", "forecaster", "songs", "top right", "log loss", "band changes");
    for (int k = 0; k < 4; k++) {
        if (!m[k].n) { printf("%-12s %6d\n", m[k].name, 0); continue; }
        char ll[16]; if (k) snprintf(ll, sizeof ll, "%.3f", m[k].ll / m[k].n); else snprintf(ll, sizeof ll, "-");
        printf("%-12s %6d %9.1f%% %9s %8d/%d right\n", m[k].name, m[k].n, 100.0 * m[k].ok / m[k].n, ll, m[k].xok, m[k].xn);
    }
    refit();
    printf("blend now: Jev^%.2f * analog^%.2f\n", blend_a, blend_c);
    prefs_load();
    int sk = 0, rp = 0;
    for (int i = 0; i < npref; i++) { sk += prefs[i].skips; rp += prefs[i].replays; }
    printf("skips %d and replays %d over %d songs (prefs.tsv); in the draw a song weighs exp(0.6 replays - 0.8 skips), at least 0.05\n", sk, rp, npref);
    return 0;
}

/* The analog model on the stored series: at every minute, the forecast for
 * H minutes on from the minutes before it only, against persistence. */
static int cmd_backtest(int h) {
    series_load();
    dots_sync();
    int n = 0, pok = 0, aok = 0, cok = 0, xn = 0, xa = 0, xp = 0; double all = 0, cll = 0, pll = 0;
    for (int t = 360; t + h < gdn; t++) {
        if (gdot[t] < 0 || gdot[t + h] < 0) continue;
        double p[NBANDS], c[NBANDS];
        if (analog(t, h, p) < 3 || anchored(t, h, c) < 3) continue;
        int act = band_of(gdot[t + h]), now_b = band_of(gdot[t]), top = top_of(p);
        /* persistence as a distribution: the stay rate on the band now
         * (the anchored model's), the rest even */
        double ps = c[now_b], pp = act == now_b ? ps : (1 - ps) / (NBANDS - 1);
        n++; pok += now_b == act; aok += top == act; cok += top_of(c) == act;
        all += ll_of(p, act); cll += ll_of(c, act); pll += -log(pp > 1e-6 ? pp : 1e-6);
        if (now_b != act) { xn++; xa += top == act; }
        else xp++;
    }
    if (!n) { printf("too little series for a backtest (%d minutes)\n", gdn); return 1; }
    printf("backtest, %d minutes ahead, %d minutes forecast (of %d kept):\n", h, n, gdn);
    printf("  persistence: %.1f%% right, mean log loss %.3f (stay rate on the band now, the rest even)\n", 100.0 * pok / n, pll / n);
    printf("  analog:      %.1f%% right, mean log loss %.3f; on the %d band changes %d right (%.1f%%)\n", 100.0 * aok / n, all / n, xn, xa, xn ? 100.0 * xa / xn : 0);
    printf("  anchored:    %.1f%% right, mean log loss %.3f\n", 100.0 * cok / n, cll / n);
    printf("Consecutive minutes overlap, so the independent tests are far fewer than %d.\n", n);
    (void) xp;
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    paths();
    if (argc > 1 && !strcmp(argv[1], "score")) return cmd_score();
    if (argc > 1 && !strcmp(argv[1], "show")) return cmd_show();
    if (argc > 1 && !strcmp(argv[1], "backtest")) return cmd_backtest(argc > 2 ? atoi(argv[2]) : 4);
    if (argc > 1 && !strcmp(argv[1], "dot")) return cmd_dot();
    if (argc > 1 && !strcmp(argv[1], "compare")) return cmd_compare();
    if (argc > 1 && !strcmp(argv[1], "status")) {
        char p[700]; snprintf(p, sizeof p, "%s/status", statedir);
        FILE *f = fopen(p, "r"); if (!f) { printf("not running yet (no %s)\n", p); return 1; }
        char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); fputs(b, stdout);
        return 0;
    }
    if (argc > 1) { fprintf(stderr, "gcpdot-music [show|dot|compare|score|backtest [MIN]|status]\n"); return 2; }

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    load_conf();
    scan_tracks();
    series_load();
    egg_load();
    dots_sync();
    load_resolved();
    refit();
    say("forecasts: %d resolved so far; blend Jev^%.2f * analog^%.2f", rn, blend_a, blend_c);
    prefs_load();
    say("skips and replays known for %d songs", npref);
    char prev_id[96] = "", prev_url[4096] = "", *played[50] = { 0 };
    int64_t prev_len = 0, prev_pos = 0, prev_obs = 0; int prev_playing = 0;
    double last_pf[NBANDS] = { 0 }; long last_pred_at = 0; int last_pred = 0, last_pred_jev = 0;   /* for the display */
    {   /* the last forecast logged, so the display has one at once after a restart */
        char lpp[700]; snprintf(lpp, sizeof lpp, "%s/predictions.tsv", statedir);
        FILE *q = fopen(lpp, "r");
        char line[8192];
        while (q && fgets(line, sizeof line, q)) {
            char *fld[40]; int nf = 0;
            for (char *s = strtok(line, "\t\n"); s && nf < 40; s = strtok(NULL, "\t\n")) fld[nf++] = s;
            if (nf >= 28 && !strcmp(fld[0], "Q")) {
                for (int b = 0; b < NBANDS; b++) last_pf[b] = atof(fld[22 + b]);
                last_pred_at = atol(fld[1]) + (long) (atof(fld[3]) * 60); last_pred = 1; last_pred_jev = !strcmp(fld[6], "jev");
            }
        }
        if (q) fclose(q);
    }
    pthread_t egg;
    if (pthread_create(&egg, NULL, egg_thread, NULL)) { say("the reference egg did not start"); return 1; }
    say("reference egg: %d minutes kept, one 200-bit RDSEED trial a second from now", esn);
    /* The last minute compared (compare.tsv), so a restart goes on from it. */
    char cp[700]; snprintf(cp, sizeof cp, "%s/compare.tsv", statedir);
    long last_cmp = 0;
    {
        FILE *c = fopen(cp, "r");
        if (c) { long e; char rest[256]; while (fscanf(c, "%ld%255[^\n]", &e, rest) == 2) last_cmp = e; fclose(c); }
    }
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
        /* Each GCP minute not yet compared, against the reference egg's dot
         * at that minute (minutes before the egg had an hour are skipped). */
        {
            int i0 = sn;
            while (i0 > 0 && sep[i0 - 1] > last_cmp) i0--;
            FILE *c = i0 < sn ? fopen(cp, "a") : NULL;
            for (int i = i0; i < sn; i++) {
                double rd = egg_dot_at(sep[i]);
                if (rd >= 0 && c) {
                    double gd = dot_at(i + 1, 0);
                    if (gd >= 0) fprintf(c, "%ld %.4f %.4f %.4f %s %s\n", sep[i], sval[i], gd, rd, band_name[band_of(gd)], band_name[band_of(rd)]);
                }
                last_cmp = sep[i];
            }
            if (c) fclose(c);
        }
        if (!have) {
            queued_id[0] = 0; last_track[0] = 0;   /* VLC gone: its ids start again */
        } else {
            long cur = id_num(md.trackid);
            if (cur > max_seen_id) max_seen_id = cur;
            /* Skips and replays. A skip: the song changed while more than
             * 15 s of the last one was left, its position carried on by the
             * time since it was read (the loop can sit 40 s in a Jev
             * question). A replay: the position jumped back over 10 s within
             * a song, or a song played within the last 50 starts again. */
            {
                int64_t pos = vlc_position();
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                int64_t now_us = (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
                if (prev_id[0] && !strcmp(md.trackid, prev_id)) {
                    int64_t expect = prev_pos + (prev_playing ? now_us - prev_obs : 0);
                    if (pos + 10000000 < expect && expect > 15000000) {
                        struct pref *q = pref_get(md.url, 1); q->replays++; prefs_save();
                        logline("R\t%ld\t%s\t%.0f\t%.0f\t%s\n", (long) now, md.trackid, expect / 1e6, pos / 1e6, md.url);
                        say("replayed (back from %.0f s to %.0f s): %s", expect / 1e6, pos / 1e6, md.url);
                    }
                } else if (md.trackid[0]) {
                    if (prev_id[0] && prev_len > 0) {
                        int64_t expect = prev_pos + (prev_playing ? now_us - prev_obs : 0);
                        if (prev_len - expect > 15000000) {
                            struct pref *q = pref_get(prev_url, 1); q->skips++; prefs_save();
                            logline("K\t%ld\t%s\t%.0f\t%.0f\t%s\n", (long) now, prev_id, expect / 1e6, prev_len / 1e6, prev_url);
                            say("skipped at %.0f of %.0f s: %s", expect / 1e6, prev_len / 1e6, prev_url);
                        }
                    }
                    int again = 0;
                    for (int i = 0; i < 50; i++) if (played[i] && !strcmp(played[i], md.url)) again = 1;
                    if (again && md.url[0]) {
                        struct pref *q = pref_get(md.url, 1); q->replays++; prefs_save();
                        logline("R\t%ld\t%s\t0\t0\t%s\n", (long) now, md.trackid, md.url);
                        say("played again: %s", md.url);
                    }
                    free(played[49]); memmove(&played[1], &played[0], 49 * sizeof *played); played[0] = strdup(md.url);
                }
                snprintf(prev_id, sizeof prev_id, "%s", md.trackid);
                snprintf(prev_url, sizeof prev_url, "%s", md.url);
                prev_len = md.length; prev_pos = pos; prev_obs = now_us; prev_playing = !strcmp(st, "Playing");
            }
            /* Our song started: its band at the start goes in the log. */
            if (queued_id[0] && !strcmp(md.trackid, queued_id)) {
                logline("S\t%ld\t%s\t%.4f\t%s\t%s\n", (long) now, queued_id, dot, band_name[band], band_name[queued_band]);
                say("started (%s, dot now %.3f %s)", band_name[queued_band], dot, band_name[band]);
                queued_id[0] = 0;
                unlink(qp);   /* played: a restart must not take it over */
                load_resolved();   /* the outcome counts at once: the blend refitted */
                refit();
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
                    prev_id[0] = 0;   /* a cut of ours, not a skip of yours */
                    free(u);
                    queued_id[0] = 0;
                }
            }
            if (!queued_id[0] && md.trackid[0] && strcmp(st, "Stopped") != 0 && sn >= 240) {
                double left = md.length > 0 ? (double) (md.length - vlc_position()) / 60e6 : 4;
                if (left < 0) left = 0;
                double probs[NBANDS] = { 0 };
                /* Jev answers 25 s before the song ends at the latest (about
                 * 40 s on the 35B with CLAUDE.md; longer while the machine
                 * trains or the subject loads), at most 300 s; under 35 s
                 * left it is not asked. A song is queued either way. */
                int deadline = (int) (left * 60) - 25;
                if (deadline > 300) deadline = 300;
                int from_jev = use_jev && deadline >= 10 && ask_jev(left, deadline, probs) == 0;
                if (use_jev && !from_jev) say("Jev %s: queued on the dot's band now", deadline >= 10 ? "did not answer in time" : "not asked (the song ends too soon)");
                if (!from_jev) { for (int b = 0; b < NBANDS; b++) probs[b] = 0; probs[band] = 1; }
                /* The analog forecast from the dot's own history, and the
                 * blend of the two with the exponents refitted on every
                 * resolved forecast; the band is drawn from the blend. */
                dots_sync();
                int hmin = (int) (left + 0.5); if (hmin < 1) hmin = 1;
                double pa[NBANDS], pf[NBANDS];
                double neff = anchored(sn - 1, hmin, pa);
                int has_a = neff >= 3;
                if (from_jev || has_a) pool(probs, from_jev, pa, has_a, blend_a, blend_c, pf);
                else for (int b = 0; b < NBANDS; b++) pf[b] = probs[b];
                if (!has_a) for (int b = 0; b < NBANDS; b++) pa[b] = 0;
                int nb = draw_band(pf);
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
                        memcpy(last_pf, pf, sizeof last_pf); last_pred_at = (long) now + (long) (left * 60); last_pred = 1; last_pred_jev = from_jev;
                        FILE *q = fopen(qp, "w");
                        if (q) { fprintf(q, "%s\t%s\t%d\n", id, u, nb); fclose(q); }
                        logline("Q\t%ld\t%s\t%.1f\t%.4f\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%s\t%s\t%ld"
                                "\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.2f\t%.2f\t%.1f\n", (long) now, id, left, dot,
                                band_name[band], from_jev ? "jev" : "dot", probs[0], probs[1], probs[2], probs[3], probs[4], probs[5], band_name[nb], t,
                                sn ? (long) (now - sep[sn - 1]) : -1L,
                                pa[0], pa[1], pa[2], pa[3], pa[4], pa[5], pf[0], pf[1], pf[2], pf[3], pf[4], pf[5], blend_a, blend_c, neff);
                        say("queued at the bottom (%s plays, %.1f min left): band %s; Jev%s [%.2f %.2f %.2f %.2f %.2f %.2f], analog (%.0f) [%.2f %.2f %.2f %.2f %.2f %.2f], "
                            "blend ^%.2f ^%.2f [%.2f %.2f %.2f %.2f %.2f %.2f] %s", md.trackid, left, band_name[nb], from_jev ? "" : " (none)",
                            probs[0], probs[1], probs[2], probs[3], probs[4], probs[5], neff, pa[0], pa[1], pa[2], pa[3], pa[4], pa[5],
                            blend_a, blend_c, pf[0], pf[1], pf[2], pf[3], pf[4], pf[5], t);
                    } else say("could not queue %s", t);
                    free(u);
                }
            }
            snprintf(last_track, sizeof last_track, "%s", md.trackid);
        }
        last_band = band;
        long em = egg_latest();
        double rdot = em ? egg_dot_at(em) : -1;
        write_dots(dot, rdot, last_pf, last_pred, last_pred_at, last_pred_jev);
        char ref[160];
        if (rdot >= 0) snprintf(ref, sizeof ref, "%.3f (%s) at the minute ending %ld", rdot, band_name[band_of(rdot)], em);
        else snprintf(ref, sizeof ref, "under an hour of trials yet (%ld trials, last z %+.2f)", egg_trials, egg_last_z);
        write_status("dot %.3f (%s)%s, latest netvar %.4f, %d minutes kept\nreference egg: %s\nVLC: %s%s\nqueued: %s%s%s\nswitch %s, Jev %s\n",
                     dot, band_name[band], stale ? " STALE" : "", sn ? sval[sn - 1] : 0, sn, ref,
                     have ? st : "not running", have ? "" : "", queued_id[0] ? queued_id : "nothing", queued_id[0] ? " band " : "",
                     queued_id[0] ? band_name[queued_band] : "", switch_now ? "now" : "next", use_jev ? "on" : "off");
        for (int i = 0; i < LOOP_S * 10 && !stop; i++) usleep(100000);
    }
    say("stopping");
    sd_bus_unref(bus);
    return 0;
}
