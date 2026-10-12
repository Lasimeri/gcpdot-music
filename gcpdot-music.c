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
#define NQ 21   /* quantiles through which a forecast's uncertainty is passed */
static const char *band_name[NBANDS] = { "red", "orange", "yellow", "green", "teal", "blue" };
static const char *band_desc[NBANDS] = { "index below 0.05", "0.05 to 0.10", "0.10 to 0.40",
                                         "0.40 to 0.90", "0.90 to 0.95", "above 0.95" };

#define MAXMIN (7 * 1440)          /* minutes of series kept */
#define FETCH_S 120                /* besides each song change, the 24 h history fetched every 2 min (the change alert needs fresh minutes; gcp2.net's own gauge polls every 60 s) */
#define LOOP_S 5
#define DECIDE_S 60   /* the next song is decided this long before this one ends */
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
static int src_local;              /* source=local: the dot from this PC's RNG (below) */
static const char *sfx = "";       /* "-local": its own predictions and alerts files */
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
    "# gcp: the dot from GCP 2.0's live netvar (2.5 to 4 minutes late, rate\n"
    "# limited); local: rebuilt from this PC's RNG (RDSEED, GCP 1.0's math,\n"
    "# under a minute late)\n"
    "source=gcp\n"
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
        else if (!strcmp(k, "source")) { src_local = !strcmp(v, "local"); sfx = src_local ? "-local" : ""; }
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
static int scount[MAXMIN];   /* local source: the trials in each minute (sval is their mean z^2) */
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
/* What the main loop is waiting on (busy: start time, what), for the
 * display: a long wait (a Jev question, the feed's rate limit) is not a
 * daemon that stopped. */
static void busy(const char *what) {
    char p[700]; snprintf(p, sizeof p, "%s/busy", statedir);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%ld\t%s\n", (long) time(NULL), what); fclose(f); }
}
static void unbusy(void) { char p[700]; snprintf(p, sizeof p, "%s/busy", statedir); unlink(p); }

static int fetch_inner(void);
/* New minutes from the 24 h history appended (memory and disk). */
static int fetch(void) {
    busy("reading the GCP feed");
    int r = fetch_inner();
    unbusy();
    return r;
}
static int fetch_inner(void) {
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
            if (attempt < 3) { busy(limited ? "waiting out the GCP feed's rate limit" : "reading the GCP feed again"); sleep(limited ? wait_s[attempt] : 3); continue; }
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
static double local_dot_at(int end);
static double dot_at(int end, int b0) {
    if (src_local) return local_dot_at(end);   /* the exact chi-square index */
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

/* ---- the local source: the dot rebuilt from this PC's RNG ----
 * With source=local the series the dot, the forecasts and the scoring run
 * on is the reference egg's: one value a minute, the mean z^2 of its
 * trials that minute (GCP 1.0's per-second netvar for a one-egg network,
 * averaged), appended as each minute closes (under a minute late, against
 * GCP 2.0's 2.5 to 4 minutes and its rate limit). GCP is not read then. A
 * minute with fewer than 30 trials (a restart) is left out. */
static int local_sync(void) {
    int added = 0;
    pthread_mutex_lock(&emu);
    for (int i = 0; i < esn; i++) {
        if (en[i] < 30 || (sn && emin[i] <= sep[sn - 1])) continue;
        if (sn == MAXMIN) { memmove(sep, sep + 1, (MAXMIN - 1) * sizeof *sep); memmove(sval, sval + 1, (MAXMIN - 1) * sizeof *sval); memmove(scount, scount + 1, (MAXMIN - 1) * sizeof *scount); sn--; }
        sep[sn] = emin[i]; sval[sn] = ez2[i] / en[i]; scount[sn] = en[i]; sn++; added++;
    }
    pthread_mutex_unlock(&emu);
    return added;
}
/* The series for the source in use: GCP 2.0's minutes kept on disk, or the
 * egg's (its minutes kept on disk, egg-min.tsv). */
static void series_open(void) {
    if (src_local) { egg_load(); local_sync(); }
    else series_load();
}

/* The local dot, exact: the egg's minutes are independent, so a window's
 * summed z^2 over k trials is chi-square with k degrees of freedom and the
 * index is its upper-tail probability (the original dot's math), calibrated
 * from the first hour with no baseline; the 1 h and 2 h windows, the one
 * farther from 0.5. Windows are by time: the minutes ending in (E - w, E]. */
static void local_known(long e, int w, int upto, double *s2, long *k) {
    *s2 = 0; *k = 0;
    for (int i = upto - 1; i >= 0 && sep[i] > e - (long) w * 60; i--)
        if (sep[i] <= e) { *s2 += sval[i] * scount[i]; *k += scount[i]; }
}
static double farther(double a, double b) {
    if (a < 0) return b;
    if (b < 0) return a;
    return fabs(b - 0.5) > fabs(a - 0.5) ? b : a;
}
static double local_dot_at(int end) {
    if (end < 1 || end > sn) return -1;
    long e = sep[end - 1];
    double s2a, s2b; long ka, kb;
    local_known(e, 60, end, &s2a, &ka); local_known(e, 120, end, &s2b, &kb);
    double a = ka >= 0.8 * 3600 ? chi2_upper(s2a, (double) ka) : -1;
    double b = kb >= 0.8 * 7200 ? chi2_upper(s2b, (double) kb) : -1;
    return farther(a, b);
}
/* The standard normal quantile (Acklam's rational approximation, relative
 * error under 1.2e-9), and the chi-square quantile by Wilson-Hilferty. */
static double norm_q(double p) {
    static const double a[] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
    static const double b[] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01 };
    static const double c[] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
    static const double d[] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00 };
    double q, r;
    if (p < 0.02425) { q = sqrt(-2 * log(p)); return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    if (p > 1 - 0.02425) { q = sqrt(-2 * log(1 - p)); return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    q = p - 0.5; r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
}
static double chi2_q(double k, double p) {
    double h = 2.0 / (9.0 * k), v = 1 - h + norm_q(p) * sqrt(h);
    return k * v * v * v;
}
/* The exact forecast of the local dot H minutes after minute T: the known
 * minutes of each window plus the future ones' summed z^2, which is
 * chi-square with 60 H degrees of freedom; NQ quantiles of it through the
 * index give the band probabilities. Nothing to fit. Method 3 (the oracle
 * check) puts the actual future minutes in instead. */
static double local_forecast(int method, int t, int h, double p[NBANDS]) {
    long e = sep[t] + (long) h * 60;
    for (int b = 0; b < NBANDS; b++) p[b] = 0;
    if (method == 3) {
        int end = t + 1; while (end < sn && sep[end] <= e) end++;
        double d = end - 1 > t && sep[end - 1] == e ? local_dot_at(end) : -1;
        if (d >= 0) p[band_of(d)] = 1;
        return d;
    }
    double s2a, s2b, kf = 60.0 * h; long ka, kb;
    local_known(e, 60, t + 1, &s2a, &ka); local_known(e, 120, t + 1, &s2b, &kb);
    double point = -1;
    for (int q = 0; q < NQ; q++) {
        double x = chi2_q(kf, (q + 0.5) / NQ);
        double a = ka + kf >= 0.8 * 3600 ? chi2_upper(s2a + x, ka + kf) : -1;
        double b = kb + kf >= 0.8 * 7200 ? chi2_upper(s2b + x, kb + kf) : -1;
        double d = farther(a, b);
        if (d < 0) return -1;
        p[band_of(d)] += 1.0 / NQ;
        if (q == NQ / 2) point = d;
    }
    double tot = 0; for (int b = 0; b < NBANDS; b++) { p[b] = p[b] * 0.98 + 0.02 / NBANDS; tot += p[b]; }
    for (int b = 0; b < NBANDS; b++) p[b] /= tot;
    return point;
}

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
struct res { long t, target; int now_b, act, act2, has_j, has_a, has_f; double pj[NBANDS], pa[NBANDS], pf[NBANDS]; double h; };
/* The series index of minute E (an end_epoch), -1 when it is not there. */
static int minute_index(long e) {
    int lo = 0, hi = sn - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (sep[m] == e) return m; if (sep[m] < e) lo = m + 1; else hi = m - 1; }
    return -1;
}
static struct res *rs; static int rn, rcap;
static double blend_a = 0.5, blend_c = 1.0;   /* p ∝ Jev^a * analog^c until refitted */
static int band_idx(const char *s) { for (int b = 0; b < NBANDS; b++) if (!strcmp(s, band_name[b])) return b; return -1; }
static void load_resolved(void) {
    rn = 0;
    char p[700]; snprintf(p, sizeof p, "%s/predictions%s.tsv", statedir, sfx);
    FILE *f = fopen(p, "r");
    if (!f) return;
    struct res pend[64]; char pid[64][96]; int pn = 0;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char *fld[40]; int nf = 0;
        for (char *s = strtok(line, "\t\n"); s && nf < 40; s = strtok(NULL, "\t\n")) fld[nf++] = s;
        if (nf >= 15 && !strcmp(fld[0], "Q")) {
            struct res r; memset(&r, 0, sizeof r);
            r.t = atol(fld[1]); r.h = atof(fld[3]); r.now_b = band_idx(fld[5]); r.act2 = -1;
            /* older lines (no target): the minute they were for, rebuilt from
             * the time, the newest minute's age then and the horizon, to see
             * how often scoring at the song's start judged another minute */
            if (nf >= 16 && nf < 32) {
                int h = (int) (r.h + 0.5); if (h < 1) h = 1;
                long tg = atol(fld[1]) - atol(fld[15]) + (long) h * 60;   /* the newest minute then, plus h */
                int i = minute_index(tg);
                if (i >= 0 && i < gdn && gdot[i] >= 0) r.act2 = band_of(gdot[i]);
            }
            r.has_j = !strcmp(fld[6], "jev");
            for (int b = 0; b < NBANDS; b++) r.pj[b] = atof(fld[7 + b]);
            if (nf >= 28) { r.has_a = 1; r.has_f = 1; for (int b = 0; b < NBANDS; b++) { r.pa[b] = atof(fld[16 + b]); r.pf[b] = atof(fld[22 + b]); } }
            if (r.has_a) { double t = 0; for (int b = 0; b < NBANDS; b++) t += r.pa[b]; if (t <= 0) r.has_a = r.has_f = 0; }
            /* A forecast for a set GCP minute (the newest at the time plus
             * its horizon) is scored on that minute's live dot once the
             * minute has arrived, whenever that is: not on whatever minute
             * was newest when the song started, whose lag differs. */
            if (nf >= 32) {
                r.target = atol(fld[31]);
                int i = minute_index(r.target);
                if (i >= 0 && i < gdn && gdot[i] >= 0 && r.now_b >= 0) {
                    r.act = band_of(gdot[i]);
                    if (rn == rcap) { rcap = rcap ? rcap * 2 : 256; rs = realloc(rs, rcap * sizeof *rs); }
                    rs[rn++] = r;
                }
                continue;
            }
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
static int ask_jev(double horizon_min, int deadline_s, const double *suggest, double probs[NBANDS]) {
    if (sn < 120) return -1;
    char state[8192]; int o = 0;
    if (src_local) o += snprintf(state + o, sizeof state - o, "This PC's hardware random generator (RDSEED, one 200-bit trial a second, scored as z = (sum - 100) / sqrt(50)), its network variance (mean z squared per minute, expected 1), the mean of each 10 minutes over the last two hours, oldest first:");
    else o += snprintf(state + o, sizeof state - o, "GCP 2.0 global network variance (netvar), the mean of each 10 minutes over the last two hours, oldest first:");
    for (int k = 0; k < 12; k++) {
        double s = 0; for (int i = sn - 120 + k * 10; i < sn - 120 + (k + 1) * 10; i++) s += sval[i];
        o += snprintf(state + o, sizeof state - o, src_local ? "%s %.3f" : "%s %.1f", k ? "," : "", s / 10);
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
    if (rnow >= 0 && !src_local) {   /* local: the series is the egg, no separate reference */
        o += snprintf(state + o, sizeof state - o, " Reference (this machine's own hardware random generator, one 200-bit trial a second, the same index with an exact chi-square; independent of the network) every 10 minutes over the same two hours:");
        for (int k = 0; k < 12; k++) {
            double x = egg_dot_at(sep[sn - 111 + k * 10]);
            if (x >= 0) o += snprintf(state + o, sizeof state - o, "%s %.2f", k ? "," : "", x);
            else o += snprintf(state + o, sizeof state - o, "%s none", k ? "," : "");
        }
        o += snprintf(state + o, sizeof state - o, ". Reference now: %.2f (%s).", rnow, band_name[band_of(rnow)]);
    }
    /* The rebuilt-dot model's forecast for the same minute, as a suggestion
     * (the dot recomputed from the known minutes and a forecast of the next
     * ones; exact when that forecast is), with its chance of a band change. */
    if (suggest) {
        int nb = band_of(dot_now());
        o += snprintf(state + o, sizeof state - o, " Suggestion from the rebuilt-dot model (the index recomputed from the known minutes and a forecast of the next ones, ranked exactly as the live index; in a backtest 98 percent right 2 minutes ahead and 88 percent of band changes caught) for the same minute:");
        for (int b = 0; b < NBANDS; b++) o += snprintf(state + o, sizeof state - o, "%s %s %.2f", b ? "," : "", band_name[b], suggest[b]);
        o += snprintf(state + o, sizeof state - o, "; chance of a change from the band now (%s): %.0f percent.", band_name[nb], 100 * (1 - suggest[nb]));
    }
    /* Its own recent record, so a miss can move the next forecast. */
    char fb[1500]; feedback(fb, sizeof fb);
    if (fb[0]) o += snprintf(state + o, sizeof state - o, " Your recent forecasts of this (your top band and its probability, then the band the live index was in at that minute):%s.", fb);
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
    series_open();
    int f = src_local ? 0 : fetch();
    if (f < 0) say("the feed did not answer; the minutes kept on disk only");
    if (sn < 240) { say("only %d minutes of the series", sn); return 1; }
    double x = dot_now();
    printf("minutes kept: %d (%.1f days), latest netvar %.4f at %ld\n", sn, sn / 1440.0, sval[sn - 1], sep[sn - 1]);
    if (src_local) printf("source local (this PC's RNG): exact chi-square dot %.3f: %s\n", x, band_name[band_of(x)]);
    else printf("1 h index %.3f, 2 h index %.3f -> dot %.3f: %s\n", index_w(60, 0, sn), index_w(120, 0, sn), x, band_name[band_of(x)]);
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

/* The change alert: the mechanistic forecast 2 minutes past the newest
 * minute, made whenever new minutes arrive (not only per song). */
static double alert_p[NBANDS]; static long alert_at; static int alert_ok;

/* Live scoring of the alert: every alert (alerts.tsv: made, target minute,
 * band then, six probabilities) is scored once its target minute is in the
 * series, over the last 24 h: the top band right, band changes caught by an
 * alert (P(change) >= ALERT_P, the right band), alerts right, log loss. */
#define ALERT_P (src_local ? 0.5 : 0.6)
struct live { int n, ok, ch, caught, calls, callok; double ll; };
static struct live live24;
/* The latest band-change events for the display's log: a change that
 * happened (caught or missed) or an alert that was raised (a false alarm
 * when no change came): target minute, band then, band at the target,
 * whether an alert was raised, the band it named, its probability. */
struct cev { long target; int nb, act, call, ob; double pc; };
#define NCEV 5
static struct cev cev[NCEV]; static int ncev;
static int minute_index(long e);
static void alert_log(long made, long target, int now_b, const double p[NBANDS]) {
    char p2[700]; snprintf(p2, sizeof p2, "%s/alerts%s.tsv", statedir, sfx);
    FILE *f = fopen(p2, "a");
    if (!f) return;
    fprintf(f, "%ld\t%ld\t%d", made, target, now_b);
    for (int b = 0; b < NBANDS; b++) fprintf(f, "\t%.4f", p[b]);
    fprintf(f, "\n");
    fclose(f);
}
static void alert_score(void) {
    char p2[700]; snprintf(p2, sizeof p2, "%s/alerts%s.tsv", statedir, sfx);
    memset(&live24, 0, sizeof live24); ncev = 0;
    FILE *f = fopen(p2, "r");
    if (!f || !sn) { if (f) fclose(f); return; }
    long newest = sep[sn - 1], made, target, last_target = 0; int nb; double p[NBANDS];
    while (fscanf(f, "%ld %ld %d %lf %lf %lf %lf %lf %lf", &made, &target, &nb, &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]) == 9) {
        if (target > newest || target < newest - 86400 || nb < 0 || nb >= NBANDS) continue;
        /* one alert per target minute: a restart logs the current one again */
        if (target == last_target) continue;
        last_target = target;
        int i = minute_index(target);
        if (i < 0 || i >= gdn || gdot[i] < 0) continue;
        int act = band_of(gdot[i]), top = 0, ob = -1;
        for (int b = 1; b < NBANDS; b++) if (p[b] > p[top]) top = b;
        for (int b = 0; b < NBANDS; b++) if (b != nb && (ob < 0 || p[b] > p[ob])) ob = b;
        int call = 1 - p[nb] >= ALERT_P;
        live24.n++; live24.ok += top == act; live24.ll -= log(p[act] > 1e-6 ? p[act] : 1e-6);
        if (act != nb) { live24.ch++; live24.caught += call && ob == act; }
        if (call) { live24.calls++; live24.callok += ob == act; }
        if (act != nb || call) {   /* newest last; the oldest dropped */
            if (ncev == NCEV) { memmove(cev, cev + 1, (NCEV - 1) * sizeof *cev); ncev--; }
            cev[ncev++] = (struct cev) { target, nb, act, call, ob, 1 - p[nb] };
        }
    }
    fclose(f);
}
static void write_dots(double gcp, double ref, const double pf[NBANDS], int have_pred, long pred_at, int pred_src_jev) {
    char p[700], t[720]; snprintf(p, sizeof p, "%s/dots", statedir); snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w");
    if (!f) return;
    fprintf(f, "time=%ld\ngcp=%.4f\nlatest_minute=%ld\nref=%.4f\n", (long) time(NULL), gcp, sn ? sep[sn - 1] : 0L, ref);
    fprintf(f, "source=%s\n", src_local ? "local" : "gcp");
    if (have_pred) {
        fprintf(f, "pred_at=%ld\npred_src=%s\npred=", pred_at, pred_src_jev ? "Jev + rebuilt dot" : "rebuilt dot");
        for (int b = 0; b < NBANDS; b++) fprintf(f, "%s%.4f", b ? " " : "", pf[b]);
        fprintf(f, "\n");
    }
    if (live24.n) fprintf(f, "live=%d %d %d %d %d %d %.4f\n", live24.n, live24.ok, live24.ch, live24.caught, live24.calls, live24.callok, live24.ll / live24.n);
    for (int i = 0; i < ncev; i++) fprintf(f, "event=%ld %d %d %d %d %.3f\n", cev[i].target, cev[i].nb, cev[i].act, cev[i].call, cev[i].ob, cev[i].pc);
    if (alert_ok) {
        fprintf(f, "alert_at=%ld\nalert=", alert_at);
        for (int b = 0; b < NBANDS; b++) fprintf(f, "%s%.4f", b ? " " : "", alert_p[b]);
        fprintf(f, "\n");
    }
    if (rn) {
        struct res *r = &rs[rn - 1];
        fprintf(f, "last_target=%ld\nlast_pred=%s\nlast_actual=%s\n", r->target, band_name[r->has_f ? top_of(r->pf) : r->has_j ? top_of(r->pj) : r->now_b], band_name[r->act]);
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
    printf("\033]0;GCP dot\007\033[?25l\033[?7l");   /* the window title (gcpdot-show finds the window by it), cursor off, no line wrap: an overlong line is cut, it never scrolls the display */
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    const unsigned bg = 0x0A0A0F;
    while (!stop) {
        double gcp = -1, ref = -1, pred[NBANDS] = { 0 }; long t = 0, latest = 0, pred_at = 0; int havep = 0;
        char src[32] = "", lastp[16] = "", lasta[16] = ""; long last_tgt = 0, alert_at2 = 0; double al[NBANDS] = { 0 }; int haveal = 0;
        char srcmode[16] = "gcp";
        int ln = 0, lok = 0, lch = 0, lcaught = 0, lcalls = 0, lcallok = 0; double lll = 0;
        struct cev ev[NCEV]; int nev = 0;
        FILE *f = fopen(p, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                if (sscanf(line, "time=%ld", &t) == 1 || sscanf(line, "gcp=%lf", &gcp) == 1 || sscanf(line, "latest_minute=%ld", &latest) == 1 ||
                    sscanf(line, "ref=%lf", &ref) == 1 || sscanf(line, "pred_at=%ld", &pred_at) == 1) continue;
                if (sscanf(line, "pred_src=%31[^\n]", src) == 1) continue;
                if (sscanf(line, "last_target=%ld", &last_tgt) == 1 || sscanf(line, "last_pred=%15s", lastp) == 1 || sscanf(line, "last_actual=%15s", lasta) == 1) continue;
                if (sscanf(line, "alert_at=%ld", &alert_at2) == 1) continue;
                if (sscanf(line, "source=%15s", srcmode) == 1) continue;
                if (sscanf(line, "live=%d %d %d %d %d %d %lf", &ln, &lok, &lch, &lcaught, &lcalls, &lcallok, &lll) == 7) continue;
                if (nev < NCEV && sscanf(line, "event=%ld %d %d %d %d %lf", &ev[nev].target, &ev[nev].nb, &ev[nev].act, &ev[nev].call, &ev[nev].ob, &ev[nev].pc) == 6) { if (ev[nev].nb >= 0 && ev[nev].nb < NBANDS && ev[nev].act >= 0 && ev[nev].act < NBANDS && ev[nev].ob >= 0 && ev[nev].ob < NBANDS) nev++; continue; }
                if (!strncmp(line, "alert=", 6)) { haveal = sscanf(line + 6, "%lf %lf %lf %lf %lf %lf", &al[0], &al[1], &al[2], &al[3], &al[4], &al[5]) == 6; continue; }
                if (!strncmp(line, "pred=", 5)) havep = sscanf(line + 5, "%lf %lf %lf %lf %lf %lf", &pred[0], &pred[1], &pred[2], &pred[3], &pred[4], &pred[5]) == 6;
            }
            fclose(f);
        }
        long now = time(NULL);
        int local = !strcmp(srcmode, "local");
        printf("\033[48;2;10;10;15m\033[2J\033[H\033[38;2;196;148;90m %s\033[0m", local ? "this PC's RNG dot and its forecast" : "GCP 2.0 live dot and its forecast");
        /* Stale: busy on a long wait (the busy note says which), or, five
         * minutes on without a note, not writing at all. */
        if (!f || now - t > 30) {
            char bp[700], what[128] = ""; long bt = 0; snprintf(bp, sizeof bp, "%s/busy", statedir);
            FILE *b = fopen(bp, "r");
            if (b) { if (fscanf(b, "%ld\t%127[^\n]", &bt, what) < 2) what[0] = 0; fclose(b); }
            if (what[0]) printf("\033[s\033[2;3H\033[38;2;138;106;62m%s, %ld s\033[u\033[0m", what, now - bt);
            else if (!f || now - t > 300) printf("\033[s\033[2;3H\033[38;2;255;80;80mdaemon not writing: systemctl --user status gcpdot-music\033[u\033[0m");
        }
        /* the two dots */
        /* no song forecast yet (a fresh source): the 2-minute alert forecast */
        if ((!havep || (latest && pred_at < latest)) && haveal) { for (int b = 0; b < NBANDS; b++) pred[b] = al[b]; pred_at = alert_at2; havep = 1; snprintf(src, sizeof src, "2-minute forecast"); }
        int pb = havep ? top_of(pred) : -1;
        draw_disc(3, 3, 12, gcp >= 0 ? colour_of(gcp) : 0x333333, bg);
        draw_disc(3, 27, 12, pb >= 0 ? colour_of(band_mid[pb]) : 0x333333, bg);
        printf("\033[48;2;10;10;15m\033[38;2;220;220;220m");
        printf("\033[10;3H live %s", gcp >= 0 ? "" : "-");
        if (gcp >= 0) printf("%.3f %s", gcp, band_name[band_of(gcp)]);
        printf("\033[10;27Hforecast ");
        if (pb >= 0) printf("%s %.0f%%", band_name[pb], 100 * pred[pb]); else printf("-");
        { time_t lt = latest; char lm[8]; strftime(lm, sizeof lm, "%H:%M", localtime(&lt));
          printf("\033[11;3H\033[38;2;138;106;62m%s, %ld min old", latest ? lm : "-", latest ? (now - latest) / 60 : 0); }
        if (pb >= 0) {
            /* the GCP minute it is for (it resolves when that minute arrives) */
            time_t tt = pred_at; char hm[8]; strftime(hm, sizeof hm, "%H:%M", localtime(&tt));
            printf("\033[11;27Hfor %s", hm);
            /* The change alert: a band other than the live one at ALERT_P or
             * more by that minute, the balanced point of the backtest: GCP
             * 0.6 (at 2 min, 84% of changes caught, 84% of alerts right);
             * local 0.5, a change more likely than not (2 min: 55% caught,
             * 67% right; at 0.4, 65% and 61%). */
            if (gcp >= 0) {
                /* the 2-minute alert forecast (made on every new minute) when
                 * there is one, else the song's forecast */
                const double *pp = haveal ? al : pred;
                char am[8] = ""; time_t at = haveal ? alert_at2 : pred_at; strftime(am, sizeof am, "%H:%M", localtime(&at));
                int nb = band_of(gcp), ob = -1;
                for (int b = 0; b < NBANDS; b++) if (b != nb && (ob < 0 || pp[b] > pp[ob])) ob = b;
                double pc = 1 - pp[nb];
                if (pc >= (local ? 0.5 : 0.6)) {
                    unsigned c = colour_of(band_mid[ob]);
                    printf("\033[12;3H\033[38;2;%u;%u;%um▲ change %s -> %s %.0f%% by %s", c >> 16, (c >> 8) & 255, c & 255, band_name[nb], band_name[ob], 100 * pc, am);
                } else printf("\033[12;3H\033[38;2;100;100;100mno change by %s (%.0f%% chance)", am, 100 * pc);
            }
            /* the forecast per band */
            for (int b = 0; b < NBANDS; b++) {
                unsigned c = colour_of(band_mid[b]); int w = (int) (pred[b] * 20 + 0.5);
                printf("\033[%d;3H\033[38;2;150;150;150m%-6s \033[38;2;%u;%u;%um", 13 + b, band_name[b], c >> 16, (c >> 8) & 255, c & 255);
                for (int i = 0; i < w; i++) printf("█");
                printf("\033[38;2;150;150;150m %.2f", pred[b]);
            }
            printf("\033[19;3H\033[38;2;138;106;62m%s", src);
        }
        printf("\033[20;3H\033[38;2;150;150;150m%s", local ? "exact chi-square check " : "this PC (reference) ");
        if (ref >= 0) { unsigned c = colour_of(ref); printf("\033[38;2;%u;%u;%um●\033[38;2;150;150;150m %.3f %s", c >> 16, (c >> 8) & 255, c & 255, ref, band_name[band_of(ref)]); }
        else printf("warming up");
        char ltm[8] = "-"; if (last_tgt) { time_t tt2 = last_tgt; strftime(ltm, sizeof ltm, "%H:%M", localtime(&tt2)); }
        if (lastp[0]) printf("\033[21;3Hlast %s: forecast %s, live %s %s", ltm, lastp, lasta, strcmp(lastp, lasta) ? "\033[38;2;255;80;80mmiss" : "\033[38;2;100;250;100mhit");
        /* the alert's live record, last 24 h */
        if (ln) printf("\033[22;3H\033[38;2;138;106;62mlive 24h %d%% right, changes %d/%d, alerts %d/%d", 100 * lok / ln, lcaught, lch, lcallok, lcalls);
        else printf("\033[22;3H\033[38;2;100;100;100mlive record: waiting for scored alerts");
        /* The band-change log: hits and misses over the band changes of the
         * last 24 h, false alarms over the alerts, then the latest events
         * (newest first): caught (green), missed (red), false alarm (amber). */
        printf("\033[24;3H\033[38;2;196;148;90mband changes, last 24 h");
        if (lch) printf("\033[25;3H\033[38;2;100;250;100mhits %d%%\033[38;2;150;150;150m (%d/%d)  \033[38;2;255;80;80mmisses %d%%\033[38;2;150;150;150m (%d/%d)",
                        100 * lcaught / lch, lcaught, lch, 100 * (lch - lcaught) / lch, lch - lcaught, lch);
        else printf("\033[25;3H\033[38;2;100;100;100mno band change scored yet");
        if (lcalls) printf("\033[26;3H\033[38;2;255;190;60mfalse alarms %d%%\033[38;2;150;150;150m (%d of %d alerts)", 100 * (lcalls - lcallok) / lcalls, lcalls - lcallok, lcalls);
        else printf("\033[26;3H\033[38;2;100;100;100mno alert raised yet");
        for (int i = 0; i < nev; i++) {
            const struct cev *e = &ev[nev - 1 - i];
            time_t et = e->target; char em[8]; strftime(em, sizeof em, "%H:%M", localtime(&et));
            int changed = e->act != e->nb, hit = changed && e->call && e->ob == e->act;
            const char *what = hit ? "caught" : changed ? "missed" : "false alarm";
            unsigned c = hit ? 0x64FA64 : changed ? 0xFF5050 : 0xFFBE3C;
            printf("\033[%d;3H\033[38;2;150;150;150m%s %s->%s \033[38;2;%u;%u;%um%s\033[38;2;150;150;150m (alert %.0f%%%s%s)", 27 + i, em,
                   band_name[e->nb], changed ? band_name[e->act] : band_name[e->ob], c >> 16, (c >> 8) & 255, c & 255, what, 100 * e->pc,
                   e->call && changed && !hit ? ", named " : "", e->call && changed && !hit ? band_name[e->ob] : "");
        }
        printf("\033[0m");
        fflush(stdout);
        for (int i = 0; i < 20 && !stop; i++) usleep(100000);
    }
    printf("\033[0m\033[?25h\033[?7h\033[2J\033[H");
    return 0;
}

static int cmd_score(void) {
    series_open(); dots_sync(); load_resolved();
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
    /* How often scoring at the song's start judged another minute than the
     * one forecast (older lines; newer ones are scored on their minute). */
    int nk = 0, dis = 0, flip_hit = 0, flip_miss = 0;
    for (int i = 0; i < rn; i++) {
        struct res *r = &rs[i];
        if (r->target || r->act2 < 0) continue;
        int top = r->has_f ? top_of(r->pf) : r->has_j ? top_of(r->pj) : r->now_b;
        nk++;
        if (r->act2 != r->act) {
            dis++;
            if (top == r->act && top != r->act2) flip_hit++;    /* counted a hit, was a miss */
            if (top != r->act && top == r->act2) flip_miss++;   /* counted a miss, was a hit */
        }
    }
    int nt = 0; for (int i = 0; i < rn; i++) nt += rs[i].target != 0;
    printf("scored on their own minute: %d; older ones scored at the song's start: %d, of which %d judged another band than their minute had (%d false hits, %d false misses)\n",
           nt, nk, dis, flip_hit, flip_miss);
    prefs_load();
    int sk = 0, rp = 0;
    for (int i = 0; i < npref; i++) { sk += prefs[i].skips; rp += prefs[i].replays; }
    alert_score();
    if (live24.n) printf("live 2-minute alerts, last 24 h: %d scored, %.1f%% right; band changes %d, caught %d (%.1f%%); alerts %d, right %d (%.1f%%); log loss %.3f\n", live24.n, 100.0 * live24.ok / live24.n, live24.ch, live24.caught, live24.ch ? 100.0 * live24.caught / live24.ch : 0, live24.calls, live24.callok, live24.calls ? 100.0 * live24.callok / live24.calls : 0, live24.ll / live24.n);
    else printf("live 2-minute alerts: none scored yet (alerts.tsv)\n");
    printf("skips %d and replays %d over %d songs (prefs.tsv); in the draw a song weighs exp(0.6 replays - 0.8 skips), at least 0.05\n", sk, rp, npref);
    return 0;
}

/* The analog model on the stored series: at every minute, the forecast for
 * H minutes on from the minutes before it only, against persistence. */
/* ---- the mechanistic forecast ----
 * The dot is a function of the netvar series: the rank of the 1 h and 2 h
 * window means. H minutes on, each window has dropped its H oldest minutes
 * (known) and gained H new ones (forecast), so the future window sums are
 * rebuilt from the known minutes plus a netvar forecast and ranked exactly
 * as the live dot is; the netvar forecaster's past H-step errors, replayed
 * through the same ranking, give the band probabilities. */
static double *psum; static int psn;   /* prefix sums of sval: psum[i] = sum of the first i minutes */
static void psum_sync(void) {
    if (psn == sn + 1) return;
    psum = realloc(psum, (sn + 1) * sizeof *psum);
    psum[0] = 0; for (int i = 0; i < sn; i++) psum[i + 1] = psum[i] + sval[i];
    psn = sn + 1;
}
/* The share of W-minute windows ending before minute END (exclusive) whose
 * sum is at least CUR: the live dot's ranking, with the window given. */
static double rank_sum(int w, int end, double cur) {
    int tot = 0, ge = 0;
    for (int i = 0; i + w <= end; i++) { tot++; if (psum[i + w] - psum[i] >= cur - 1e-9) ge++; }
    return tot ? (double) ge / tot : -1;
}
/* Netvar forecasts for the H minutes after minute T (index, inclusive):
 * 0 last value held, 1 the 5-minute trend carried on, 2 AR(2) on the
 * minute-to-minute changes fitted on the last 600 minutes up to T,
 * 4 veridical mapping (after Mottron's model of autistic pattern detection:
 * the exact shape of the last 10 changes matched against all of history,
 * the changes that followed the 15 closest matches averaged), 5 AR(2) with
 * hypo-priors (after Pellicano and Burr: fitted on the last 120 minutes
 * only, fresh evidence over the long-run prior), 6 AR(2) as 2 with its
 * error spread taken from the same hours of the day (GCP: the effect size
 * varies with time of day). */
static void ar2(int t, int h, int win, double *out) {
    double s11 = 0, s12 = 0, s22 = 0, b1 = 0, b2 = 0;
    for (int i = (t > win + 3 ? t - win : 3); i <= t; i++) {
        double d = sval[i] - sval[i - 1], d1 = sval[i - 1] - sval[i - 2], d2 = sval[i - 2] - sval[i - 3];
        s11 += d1 * d1; s12 += d1 * d2; s22 += d2 * d2; b1 += d * d1; b2 += d * d2;
    }
    double det = s11 * s22 - s12 * s12, a1 = 0, a2 = 0;
    if (fabs(det) > 1e-12) { a1 = (b1 * s22 - b2 * s12) / det; a2 = (b2 * s11 - b1 * s12) / det; }
    double x = sval[t], d1 = sval[t] - sval[t - 1], d2 = sval[t - 1] - sval[t - 2];
    for (int k = 0; k < h; k++) { double d = a1 * d1 + a2 * d2; x += d; out[k] = x; d2 = d1; d1 = d; }
}
#define VM_M 10
#define VM_K 15
static void veridical(int t, int h, double *out) {
    double bd[VM_K]; int bs[VM_K], nb = 0;
    for (int s = VM_M + 1; s + h <= t; s++) {
        double dist = 0;
        for (int j = 0; j < VM_M && dist < 1e300; j++) {
            double a = sval[s - j] - sval[s - j - 1], b = sval[t - j] - sval[t - j - 1];
            dist += (a - b) * (a - b);
        }
        if (nb < VM_K) { bd[nb] = dist; bs[nb] = s; nb++; continue; }
        int worst = 0; for (int k = 1; k < VM_K; k++) if (bd[k] > bd[worst]) worst = k;
        if (dist < bd[worst]) { bd[worst] = dist; bs[worst] = s; }
    }
    double x = sval[t];
    for (int k = 0; k < h; k++) {
        double d = 0;
        for (int q = 0; q < nb; q++) d += sval[bs[q] + 1 + k] - sval[bs[q] + k];
        x += nb ? d / nb : 0;
        out[k] = x;
    }
}
static void nv_forecast(int method, int t, int h, double *out) {
    if (method == 3) { for (int k = 0; k < h; k++) out[k] = t + 1 + k < sn ? sval[t + 1 + k] : sval[sn - 1]; return; }   /* the oracle: the actual minutes (backtest check only) */
    if (method == 0 || t < 6) { for (int k = 0; k < h; k++) out[k] = sval[t]; return; }
    if (method == 1) { double s = (sval[t] - sval[t - 5]) / 5; for (int k = 0; k < h; k++) out[k] = sval[t] + (k + 1) * s; return; }
    if (method == 4 && t > VM_M + 30) { veridical(t, h, out); return; }
    if (method == 7 && t > VM_M + 30) {   /* the two averaged: AR(2) and veridical mapping */
        double o2[64]; ar2(t, h, 600, out); veridical(t, h, o2);
        for (int k = 0; k < h; k++) out[k] = 0.5 * (out[k] + o2[k]);
        return;
    }
    ar2(t, h, method == 5 ? 120 : 600, out);
}
/* The dot H minutes after minute T, from the known minutes and FUT (the
 * forecast of the next H minutes, summed into each window), ranked against
 * the windows complete by minute T. */
/* The sum of the W-minute window ending at minute E (which may lie up to H
 * minutes past T): known minutes plus forecast ones, the summed error
 * EXTRA spread over the forecast minutes in step (k of H gets k/H of it). */
static double win_sum(int w, int t, int h, int e, double extra, const double *fut) {
    int from = e - w + 1;
    double s = 0;
    if (from <= t) s += psum[(e < t ? e : t) + 1] - psum[from];
    for (int k = 0; k < h; k++) { int m = t + 1 + k; if (m >= from && m <= e) s += fut[k]; }
    if (e > t) s += extra * (double) (e - t) / h;
    return s;
}
static double mech_dot(int t, int h, double futsum_extra, const double *fut) {
    double a = -1, b = -1;
    int e = t + h;   /* the last minute of the future windows */
    for (int wi = 0; wi < 2; wi++) {
        int w = wi ? 120 : 60;
        if (e + 1 < 2 * w) continue;
        /* exactly the live ranking at minute E: every window ending up to E,
         * the known ones and the H forecast ones, the target among them */
        double cur = win_sum(w, t, h, e, futsum_extra, fut);
        int tot = 0, ge = 0;
        for (int i = 0; i + w <= t + 1; i++) { tot++; if (psum[i + w] - psum[i] >= cur - 1e-9) ge++; }
        for (int k = 1; k <= h; k++) { tot++; if (win_sum(w, t, h, t + k, futsum_extra, fut) >= cur - 1e-9) ge++; }
        double r = (double) ge / tot;
        if (wi) b = r; else a = r;
    }
    if (a < 0) return b;
    if (b < 0) return a;
    return fabs(b - 0.5) > fabs(a - 0.5) ? b : a;
}
/* Band probabilities for H minutes after minute T: the point forecast's
 * dot shifted by NQ quantiles of the forecaster's past H-step summed errors
 * (pairs whose outcome was known by T). Returns the point forecast's dot. */
static double local_forecast(int method, int t, int h, double p[NBANDS]);
static double mech_forecast(int method, int t, int h, double p[NBANDS]) {
    if (src_local) return local_forecast(method, t, h, p);   /* exact: future minutes chi-square */
    double fut[64], e[1200]; int ne = 0;
    if (h > 64) h = 64;
    for (int b = 0; b < NBANDS; b++) p[b] = 0;
    nv_forecast(method, t, h, fut);
    /* the veridical matcher is slow: its error history thinned (every 4th
     * minute, at most 300); method 6 keeps only the same hours of the day
     * (within 2 h, circular), falling back to all when under 50 */
    int step = method == 4 || method == 7 ? 4 : 1, cap = method == 4 || method == 7 ? 300 : 1200;
    for (int pass = 0; pass < 2 && ne < 50; pass++) {
        ne = 0;
        for (int s = t - h; s >= 130 && ne < cap; s -= step) {
            if (method == 6 && !pass) {
                long dh = labs((sep[s] % 86400) - (sep[t] % 86400)); if (dh > 43200) dh = 86400 - dh;
                if (dh > 7200) continue;
            }
            double f2[64], sf = 0, sa = 0;
            nv_forecast(method, s, h, f2);
            for (int k = 0; k < h; k++) { sf += f2[k]; sa += sval[s + 1 + k]; }
            e[ne++] = sa - sf;
        }
        if (method != 6) break;
    }
    double point = mech_dot(t, h, 0, fut);
    if (ne < 50) { if (point >= 0) p[band_of(point)] = 1; return point; }
    /* quantiles of the errors (sorted copy) */
    for (int i = 1; i < ne; i++) { double v = e[i]; int j = i - 1; while (j >= 0 && e[j] > v) { e[j + 1] = e[j]; j--; } e[j + 1] = v; }
    for (int q = 0; q < NQ; q++) {
        double err = e[(int) ((q + 0.5) / NQ * ne)];
        double d = mech_dot(t, h, err, fut);   /* the error added once: every window holds all H future minutes */
        if (d >= 0) p[band_of(d)] += 1.0 / NQ;
    }
    double tot = 0; for (int b = 0; b < NBANDS; b++) { p[b] = p[b] * 0.98 + 0.02 / NBANDS; tot += p[b]; }
    for (int b = 0; b < NBANDS; b++) p[b] /= tot;
    return point;
}
/* The mechanistic forecast on the stored series against persistence: the
 * point band right overall, on the band changes (recall) and among the
 * changes it called (precision), and the probabilities' log loss. */
static int cmd_mech(int h, int only) {
    series_open(); dots_sync(); psum_sync();
    for (int m = 0; m < 8; m++) {
        if (only >= 0 && m != only) continue;
        int n = 0, ok = 0, pok = 0, ch = 0, chok = 0, called = 0, calledok = 0, tok = 0, tch = 0; double ll = 0, ep = 0;
        for (int t = 300; t + h < gdn; t += 2) {
            if (gdot[t] < 0 || gdot[t + h] < 0) continue;
            double p[NBANDS];
            double d = mech_forecast(m, t, h, p);
            if (d < 0) continue;
            int act = band_of(gdot[t + h]), now_b = band_of(gdot[t]), pr = band_of(d);
            n++; ok += pr == act; pok += now_b == act; ll += ll_of(p, act);
            int tp = top_of(p); tok += tp == act; ep += p[tp]; if (act != now_b) tch += tp == act;
            if (act != now_b) { ch++; chok += pr == act; }
            if (pr != now_b) { called++; calledok += pr == act; }
        }
        static const char *nm[8] = { "last value", "5-min trend", "AR(2) on changes", "ORACLE (actual)", "veridical mapping", "AR(2) hypo-priors", "AR(2) + time of day", "AR(2) + veridical" };
        printf("%d min ahead, netvar by %-17s %d minutes: right %.1f%% (persistence %.1f%%); band changes %d, caught %d (%.1f%%); changes called %d, right %d (%.1f%%); log loss %.3f\n",
               h, src_local ? (m == 3 ? "ORACLE (actual)" : "local exact chi2") : nm[m], n, 100.0 * ok / n, 100.0 * pok / n, ch, chok, ch ? 100.0 * chok / ch : 0, called, calledok, called ? 100.0 * calledok / called : 0, ll / n);
        printf("    most probable band: right %.1f%%, changes caught %d (%.1f%%); its own expected accuracy %.1f%%\n", 100.0 * tok / n, tch, ch ? 100.0 * tch / ch : 0, 100.0 * ep / n);
    }
    /* A change called only when the AR(2) forecast gives the band now at
     * most 1 - THETA (a change at least THETA), the band called being the
     * likeliest other one: catches against false alarms. */
    static const double th[] = { 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9 };
    for (size_t k = 0; k < sizeof th / sizeof *th; k++) {
        int ch = 0, chok = 0, called = 0, calledok = 0;
        for (int t = 300; t + h < gdn; t += 2) {
            if (gdot[t] < 0 || gdot[t + h] < 0) continue;
            double p[NBANDS];
            if (mech_forecast(only >= 0 ? only : 2, t, h, p) < 0) continue;
            int act = band_of(gdot[t + h]), now_b = band_of(gdot[t]);
            int other = -1; for (int b = 0; b < NBANDS; b++) if (b != now_b && (other < 0 || p[b] > p[other])) other = b;
            int call = 1 - p[now_b] >= th[k];
            if (act != now_b) { ch++; chok += call && other == act; }
            if (call) { called++; calledok += other == act; }
        }
        printf("  change called at P(change) >= %.1f: caught %d of %d (%.1f%%), called %d, right %d (%.1f%%)\n", th[k], chok, ch, ch ? 100.0 * chok / ch : 0, called, calledok, called ? 100.0 * calledok / called : 0);
    }
    return 0;
}

static int cmd_backtest(int h) {
    series_open();
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
    load_conf();   /* the source (gcp or local) decides the series for every command */
    if (argc > 1 && !strcmp(argv[1], "score")) return cmd_score();
    if (argc > 1 && !strcmp(argv[1], "mech")) return cmd_mech(argc > 2 ? atoi(argv[2]) : 2, argc > 3 ? atoi(argv[3]) : -1);
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
    series_open();   /* local: the egg's minutes (egg_load inside) */
    if (!src_local) egg_load();
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
        char lpp[700]; snprintf(lpp, sizeof lpp, "%s/predictions%s.tsv", statedir, sfx);
        FILE *q = fopen(lpp, "r");
        char line[8192];
        while (q && fgets(line, sizeof line, q)) {
            char *fld[40]; int nf = 0;
            for (char *s = strtok(line, "\t\n"); s && nf < 40; s = strtok(NULL, "\t\n")) fld[nf++] = s;
            if (nf >= 28 && !strcmp(fld[0], "Q")) {
                for (int b = 0; b < NBANDS; b++) last_pf[b] = atof(fld[22 + b]);
                last_pred_at = nf >= 32 ? atol(fld[31]) : atol(fld[1]) + (long) (atof(fld[3]) * 60); last_pred = 1; last_pred_jev = !strcmp(fld[6], "jev");
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
    char lp[700]; snprintf(lp, sizeof lp, "%s/predictions%s.tsv", statedir, sfx);
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
        /* local: the egg's closed minutes, every pass (no network); gcp: the
         * feed on each song change, every FETCH_S, and every 30 s while it
         * has not answered */
        int due = src_local || (song_changed && now - last_fetch >= 10) || !last_fetch || (!last_ok && now - last_fetch >= 30) || now - last_fetch >= FETCH_S;
        if (due) {
            int a = src_local ? local_sync() : fetch();
            last_fetch = now;
            if (a >= 0) {
                last_ok = now;
                if (!src_local) say("feed%s: %d new minutes, latest netvar %.4f, %ld s old", song_changed ? " (song change)" : "", a, sval[sn - 1], (long) (now - sep[sn - 1]));
            } else say("feed: no answer (keeping the series as it is)");
            /* New minutes resolve the forecasts made for them: scored, and
             * the blend refitted. */
            if (a > 0) { dots_sync(); int before = rn; load_resolved(); if (rn != before) refit(); }
            if (a > 0 || !alert_ok) {
                psum_sync();
                if (sn > 300 && mech_forecast(2, sn - 1, 2, alert_p) >= 0) {
                    if (!alert_ok || alert_at != sep[sn - 1] + 120) alert_log((long) now, sep[sn - 1] + 120, band_of(dot_now()), alert_p);
                    alert_at = sep[sn - 1] + 120; alert_ok = 1;
                }
                alert_score();
            }
        }
        double dot = dot_now();
        int band = dot >= 0 ? band_of(dot) : 3;
        int stale = !last_ok || now - last_ok > 3 * FETCH_S;
        /* Each GCP minute not yet compared, against the reference egg's dot
         * at that minute (minutes before the egg had an hour are skipped). */
        if (!src_local) {   /* local: the series is the egg itself, nothing to compare */
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
            /* The next song is decided DECIDE_S before this one ends, not when
             * it starts: the forecast's horizon is then about a minute
             * instead of the whole song (local backtest: 85.6% right 1
             * minute ahead, 77.2% at 4). A song with less left (a skip into
             * its end, a short one) is decided at once. */
            double left_now = md.length > 0 ? (double) (md.length - vlc_position()) / 60e6 : 4;
            int decide = left_now * 60 <= DECIDE_S || md.length <= 0;
            if (decide && !queued_id[0] && md.trackid[0] && strcmp(st, "Stopped") != 0 && sn >= 240) {
                double left = left_now;
                if (left < 0) left = 0;
                /* The forecast is for one minute of the series. Local: the
                 * last minute closed when this song ends (its minutes close
                 * under a second late), the dot the next song starts on.
                 * GCP: the newest now plus the time left, whole minutes (its
                 * minutes arrive 2.5 to 4 minutes late). The same minute for
                 * Jev, the forecast and the score. */
                int hmin;
                if (src_local) {
                    long end_min = ((long) now + (long) (left * 60)) / 60 * 60;
                    hmin = (int) ((end_min - sep[sn - 1]) / 60);
                } else hmin = (int) (left + 0.5);
                if (hmin < 1) hmin = 1;
                long target = sep[sn - 1] + (long) hmin * 60;
                double probs[NBANDS] = { 0 };
                /* The mechanistic forecast first: the dot rebuilt from the
                 * known minutes and an AR(2) netvar forecast, ranked as the
                 * live dot is (oracle check 100%; backtest at 2 min 98%
                 * right, band changes caught 88%, log loss 0.062). It goes
                 * to Jev as a suggestion and into the blend. */
                dots_sync();
                double pa[NBANDS], pf[NBANDS];
                psum_sync();
                double neff = mech_forecast(2, sn - 1, hmin, pa) >= 0 ? 100 : 0;
                int has_a = neff >= 3;
                /* Jev answers 25 s before the song ends at the latest (about
                 * 40 s on the 35B with CLAUDE.md; longer while the machine
                 * trains or the subject loads), at most 300 s; under 35 s
                 * left it is not asked. A song is queued either way. */
                int deadline = (int) (left * 60) - 25;
                if (deadline > 300) deadline = 300;
                if (use_jev && deadline >= 10) busy("asking Jev");
                int from_jev = use_jev && deadline >= 10 && ask_jev((double) hmin, deadline, has_a ? pa : NULL, probs) == 0;
                unbusy();
                if (use_jev && !from_jev) say("Jev %s: queued on the dot's band now", deadline >= 10 ? "did not answer in time" : "not asked (the song ends too soon)");
                if (!from_jev) { for (int b = 0; b < NBANDS; b++) probs[b] = 0; probs[band] = 1; }
                /* the blend of the two, exponents refitted on every resolved
                 * forecast; the band is drawn from the blend */
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
                        memcpy(last_pf, pf, sizeof last_pf); last_pred_at = target; last_pred = 1; last_pred_jev = from_jev;
                        FILE *q = fopen(qp, "w");
                        if (q) { fprintf(q, "%s\t%s\t%d\n", id, u, nb); fclose(q); }
                        logline("Q\t%ld\t%s\t%.1f\t%.4f\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%s\t%s\t%ld"
                                "\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.2f\t%.2f\t%.1f\t%ld\n", (long) now, id, left, dot,
                                band_name[band], from_jev ? "jev" : "dot", probs[0], probs[1], probs[2], probs[3], probs[4], probs[5], band_name[nb], t,
                                sn ? (long) (now - sep[sn - 1]) : -1L,
                                pa[0], pa[1], pa[2], pa[3], pa[4], pa[5], pf[0], pf[1], pf[2], pf[3], pf[4], pf[5], blend_a, blend_c, neff, target);
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
