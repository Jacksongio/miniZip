/*
 * miniZip - a small, fast, standards-correct ZIP archiver.
 *
 * Originally written for a Software Reverse Engineering course as a
 * reimplementation of WinZip's archive writer. This version keeps the
 * "write every byte of the format by hand" spirit but adds the machinery a
 * real archiver needs:
 *
 *   - Streaming I/O            : constant memory, files larger than RAM are fine
 *   - Parallel DEFLATE         : pigz-style independent blocks primed with a
 *                                32 KiB dictionary, concatenated via Z_SYNC_FLUSH
 *   - ZIP64                    : > 4 GiB members, > 4 GiB archives, > 65535 entries
 *   - Incompressibility probe  : trial-deflates a sample and falls back to STORE
 *   - Recursion, symlinks, Unix permissions, UTF-8 names
 *
 * Build: cc -O2 -o minizip src/minizip.c -lz -lpthread
 */

#define _FILE_OFFSET_BITS 64
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  define _DARWIN_C_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>

#if !defined(MZ_NO_THREADS)
#  include <pthread.h>
#endif

#define MZ_VERSION "2.0.0"

/* ------------------------------------------------------------------ */
/* ZIP format constants                                                */
/* ------------------------------------------------------------------ */

#define SIG_LOCAL      0x04034b50u
#define SIG_CENTRAL    0x02014b50u
#define SIG_EOCD       0x06054b50u
#define SIG_EOCD64     0x06064b50u
#define SIG_EOCD64_LOC 0x07064b50u

#define METHOD_STORE   0
#define METHOD_DEFLATE 8

#define FLAG_UTF8      0x0800

#define U32_MAX_V      0xFFFFFFFFull
#define U16_MAX_V      0xFFFFu

#define DICT_SIZE      32768u          /* DEFLATE window                    */
#define PROBE_BYTES    (128u * 1024u)  /* sample size for the STORE probe   */

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

static const char *g_prog = "minizip";
static int g_verbosity = 1;            /* 0 quiet, 1 normal, 2 verbose      */
static int g_warnings = 0;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: fatal: ", g_prog);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* Command-line problems exit 2, so scripts can tell "you invoked me wrong"
   apart from "I tried and failed". */
static void bad_usage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", g_prog);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\nTry '%s --help'.\n", g_prog);
    exit(2);
}

static void warn_msg(const char *fmt, ...) {
    va_list ap;
    g_warnings++;
    if (g_verbosity < 1) return;
    va_start(ap, fmt);
    fprintf(stderr, "%s: warning: ", g_prog);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory (%zu bytes)", n);
    return q;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* Little-endian serialisation into a byte buffer                      */
/* ------------------------------------------------------------------ */

static void put16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put64(unsigned char *p, uint64_t v) {
    put32(p, (uint32_t)(v & U32_MAX_V));
    put32(p + 4, (uint32_t)((v >> 32) & U32_MAX_V));
}

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int    level;          /* 0..9, 0 == store everything              */
    int    strategy;       /* Z_DEFAULT_STRATEGY etc.                  */
    int    threads;        /* worker threads, 0 == serial              */
    size_t block;          /* parallel block size in bytes             */
    int    recurse;
    int    junk_paths;
    int    probe;          /* trial-deflate before committing          */
    int    follow_links;
} mz_opts;

/* ------------------------------------------------------------------ */
/* Parallel DEFLATE engine                                             */
/*                                                                     */
/* The input is cut into fixed-size blocks. Each block is deflated by  */
/* an independent z_stream primed with the last 32 KiB of the previous */
/* block, so the compression ratio stays within a fraction of a per    */
/* cent of a single serial stream. Non-final blocks end with           */
/* Z_SYNC_FLUSH, which terminates the current DEFLATE block on a byte  */
/* boundary; the pieces therefore concatenate into one valid stream.   */
/* The final block uses Z_FINISH to set BFINAL. Per-block CRC32s are   */
/* folded into the running CRC by the producer.                        */
/* ------------------------------------------------------------------ */

typedef struct mz_job {
    struct mz_job *next;
    uint64_t       seq;
    unsigned char *in;
    size_t         in_len;
    unsigned char  dict[DICT_SIZE];
    size_t         dict_len;
    int            last;
    unsigned char *out;
    size_t         out_cap;
    size_t         out_len;
    uint32_t       crc;        /* CRC of this block alone               */
    int            store;      /* emit raw bytes, no deflate            */
    int            err;
    int            done;
} mz_job;

typedef struct {
    int      nthreads;
    int      shutdown;
    mz_job  *head, *tail;      /* pending queue                          */
    mz_job **ring;             /* in-flight slots, indexed by seq % n     */
    size_t   ring_n;
    int      level, strategy;
#if !defined(MZ_NO_THREADS)
    pthread_mutex_t mu;
    pthread_cond_t  work_cv;
    pthread_cond_t  done_cv;
    pthread_t      *tids;
#endif
    z_stream serial;           /* used when nthreads == 0                */
    int      serial_ready;
} mz_pool;

static int deflate_block(z_stream *s, mz_job *j) {
    int ret;

    if (deflateReset(s) != Z_OK) return -1;
    if (j->dict_len && deflateSetDictionary(s, j->dict, (uInt)j->dict_len) != Z_OK)
        return -1;

    s->next_in   = j->in;
    s->avail_in  = (uInt)j->in_len;
    s->next_out  = j->out;
    s->avail_out = (uInt)j->out_cap;

    ret = deflate(s, j->last ? Z_FINISH : Z_SYNC_FLUSH);
    if (j->last) {
        if (ret != Z_STREAM_END) return -1;
    } else {
        if (ret != Z_OK || s->avail_in != 0) return -1;
    }
    j->out_len = j->out_cap - s->avail_out;
    return 0;
}

/* Runs on a worker thread. The CRC is computed here rather than in the
   producer so that checksumming scales with the thread count too --
   otherwise it becomes the serial bottleneck past about four workers. */
static void job_run(mz_job *j, z_stream *s) {
    j->crc = (uint32_t)crc32(crc32(0L, Z_NULL, 0), j->in, (uInt)j->in_len);
    if (j->store) { j->out_len = 0; j->err = 0; return; }  /* writer emits j->in */
    j->err = deflate_block(s, j) ? 1 : 0;
}

#if !defined(MZ_NO_THREADS)
typedef struct {
    void    *pool;
    z_stream strm;
} mz_worker;

static mz_worker *g_workers;

static void *worker_main(void *arg) {
    mz_worker *w = (mz_worker *)arg;
    mz_pool   *p = (mz_pool *)w->pool;

    for (;;) {
        mz_job *j;
        pthread_mutex_lock(&p->mu);
        while (!p->head && !p->shutdown)
            pthread_cond_wait(&p->work_cv, &p->mu);
        if (!p->head && p->shutdown) {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        j = p->head;
        p->head = j->next;
        if (!p->head) p->tail = NULL;
        pthread_mutex_unlock(&p->mu);

        job_run(j, &w->strm);

        pthread_mutex_lock(&p->mu);
        j->done = 1;
        pthread_cond_broadcast(&p->done_cv);
        pthread_mutex_unlock(&p->mu);
    }
    return NULL;
}
#endif

static int init_stream(z_stream *s, int level, int strategy) {
    memset(s, 0, sizeof(*s));
    /* memLevel 9 gives zlib the full hash space: slightly better ratio and
       fewer hash collisions than the historical default of 8. */
    return deflateInit2(s, level, Z_DEFLATED, -MAX_WBITS, 9, strategy) == Z_OK ? 0 : -1;
}

static void pool_init(mz_pool *p, const mz_opts *o, size_t block) {
    size_t i;

    memset(p, 0, sizeof(*p));
    p->level    = o->level ? o->level : 1;  /* level 0 is handled via STORE */
    p->strategy = o->strategy;
    p->nthreads = o->threads;
    p->ring_n   = (size_t)(p->nthreads > 0 ? p->nthreads * 2 + 2 : 1);

    p->ring = xmalloc(p->ring_n * sizeof(*p->ring));
    for (i = 0; i < p->ring_n; i++) {
        mz_job *j = xmalloc(sizeof(*j));
        memset(j, 0, sizeof(*j));
        j->in      = xmalloc(block);
        j->out_cap = (size_t)compressBound((uLong)block) + 64;
        j->out     = xmalloc(j->out_cap);
        p->ring[i] = j;
    }

#if !defined(MZ_NO_THREADS)
    if (p->nthreads > 0) {
        int t;
        pthread_mutex_init(&p->mu, NULL);
        pthread_cond_init(&p->work_cv, NULL);
        pthread_cond_init(&p->done_cv, NULL);
        p->tids   = xmalloc((size_t)p->nthreads * sizeof(*p->tids));
        g_workers = xmalloc((size_t)p->nthreads * sizeof(*g_workers));
        for (t = 0; t < p->nthreads; t++) {
            g_workers[t].pool = p;
            if (init_stream(&g_workers[t].strm, p->level, p->strategy) != 0)
                die("deflateInit2 failed");
            if (pthread_create(&p->tids[t], NULL, worker_main, &g_workers[t]) != 0)
                die("cannot create worker thread %d", t);
        }
        return;
    }
#endif
    if (init_stream(&p->serial, p->level, p->strategy) != 0)
        die("deflateInit2 failed");
    p->serial_ready = 1;
}

static void pool_free(mz_pool *p) {
    size_t i;
#if !defined(MZ_NO_THREADS)
    if (p->nthreads > 0) {
        int t;
        pthread_mutex_lock(&p->mu);
        p->shutdown = 1;
        pthread_cond_broadcast(&p->work_cv);
        pthread_mutex_unlock(&p->mu);
        for (t = 0; t < p->nthreads; t++) {
            pthread_join(p->tids[t], NULL);
            deflateEnd(&g_workers[t].strm);
        }
        free(p->tids);
        free(g_workers);
        g_workers = NULL;
        pthread_mutex_destroy(&p->mu);
        pthread_cond_destroy(&p->work_cv);
        pthread_cond_destroy(&p->done_cv);
    }
#endif
    if (p->serial_ready) deflateEnd(&p->serial);
    for (i = 0; i < p->ring_n; i++) {
        free(p->ring[i]->in);
        free(p->ring[i]->out);
        free(p->ring[i]);
    }
    free(p->ring);
}

static void pool_submit(mz_pool *p, mz_job *j) {
    j->next = NULL;
#if !defined(MZ_NO_THREADS)
    if (p->nthreads > 0) {
        pthread_mutex_lock(&p->mu);
        j->done = 0;
        if (p->tail) p->tail->next = j; else p->head = j;
        p->tail = j;
        pthread_cond_signal(&p->work_cv);
        pthread_mutex_unlock(&p->mu);
        return;
    }
#endif
    j->done = 0;
    job_run(j, &p->serial);
    j->done = 1;
}

static mz_job *pool_await(mz_pool *p, uint64_t seq) {
    mz_job *j = p->ring[seq % p->ring_n];
#if !defined(MZ_NO_THREADS)
    if (p->nthreads > 0) {
        pthread_mutex_lock(&p->mu);
        while (!j->done)
            pthread_cond_wait(&p->done_cv, &p->mu);
        pthread_mutex_unlock(&p->mu);
    }
#endif
    return j;
}

/* ------------------------------------------------------------------ */
/* Archive writer                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char    *name;
    size_t   name_len;
    uint16_t method;
    uint16_t flags;
    uint16_t mod_time, mod_date;
    uint32_t crc;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint64_t local_offset;
    uint32_t ext_attr;
    int      zip64;        /* local header carries a ZIP64 extra field */
} entry_t;

typedef struct {
    FILE     *fp;
    const char *path;
    entry_t  *entries;
    size_t    n, cap;
    uint64_t  offset;      /* current write offset                     */
    dev_t     dev;         /* identity of the archive itself, so that  */
    ino_t     ino;         /* -r never swallows its own output         */
    int       have_id;
    mz_pool   pool;
    mz_opts   opts;
    uint64_t  total_in, total_out;
} zipw;

static void zw_write(zipw *z, const void *buf, size_t n) {
    if (n && fwrite(buf, 1, n, z->fp) != n)
        die("write to '%s' failed: %s", z->path, strerror(errno));
    z->offset += n;
}

static void zw_seek(zipw *z, uint64_t off) {
    if (fseeko(z->fp, (off_t)off, SEEK_SET) != 0)
        die("seek in '%s' failed: %s", z->path, strerror(errno));
    z->offset = off;
}

static entry_t *zw_new_entry(zipw *z) {
    if (z->n == z->cap) {
        z->cap = z->cap ? z->cap * 2 : 64;
        z->entries = xrealloc(z->entries, z->cap * sizeof(*z->entries));
    }
    memset(&z->entries[z->n], 0, sizeof(entry_t));
    return &z->entries[z->n++];
}

/* ------------------------------------------------------------------ */
/* Names, timestamps, attributes                                       */
/* ------------------------------------------------------------------ */

static void dos_datetime(time_t t, uint16_t *date, uint16_t *tm_out) {
    struct tm tmv;
    int year;

#if defined(_WIN32)
    {
        struct tm *tp = localtime(&t);
        if (!tp) { *date = 0x21; *tm_out = 0; return; }
        tmv = *tp;
    }
#else
    if (!localtime_r(&t, &tmv)) { *date = 0x21; *tm_out = 0; return; }
#endif

    /* MS-DOS timestamps span 1980-01-01 .. 2107-12-31. Clamp rather than let
       the bit fields wrap, which is what produced nonsense dates before. */
    year = tmv.tm_year + 1900;
    if (year < 1980) {
        year = 1980; tmv.tm_mon = 0; tmv.tm_mday = 1;
        tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
    } else if (year > 2107) {
        year = 2107; tmv.tm_mon = 11; tmv.tm_mday = 31;
        tmv.tm_hour = 23; tmv.tm_min = 59; tmv.tm_sec = 58;
    }

    *tm_out = (uint16_t)(((tmv.tm_hour & 0x1F) << 11) |
                         ((tmv.tm_min  & 0x3F) << 5)  |
                         ((tmv.tm_sec / 2) & 0x1F));
    *date   = (uint16_t)((((year - 1980) & 0x7F) << 9) |
                         (((tmv.tm_mon + 1) & 0x0F) << 5) |
                         (tmv.tm_mday & 0x1F));
}

/* Turn a filesystem path into a safe, portable archive name: forward
   slashes, no drive letter, no leading '/', no '.' or '..' components. */
static char *archive_name(const char *path, int junk, int is_dir) {
    const char *s = path;
    char *out, *w, *p;
    size_t len;

    if (junk) {
        const char *b1 = strrchr(s, '/');
        const char *b2 = strrchr(s, '\\');
        const char *b  = b1 > b2 ? b1 : b2;
        if (b) s = b + 1;
    }

    len = strlen(s);
    if (len >= 2 && isalpha((unsigned char)s[0]) && s[1] == ':') { s += 2; len -= 2; }

    out = xmalloc(len + 2);
    w   = out;
    while (*s) {
        char c = (*s == '\\') ? '/' : *s;
        /* collapse repeated separators and never emit a leading slash */
        if (c == '/' && (w == out || w[-1] == '/')) { s++; continue; }
        *w++ = c;
        s++;
    }
    *w = '\0';

    /* Strip any leading "./" and "../" components. */
    p = out;
    for (;;) {
        if (p[0] == '.' && p[1] == '/')                    memmove(p, p + 2, strlen(p + 2) + 1);
        else if (p[0] == '.' && p[1] == '.' && p[2] == '/') memmove(p, p + 3, strlen(p + 3) + 1);
        else break;
    }

    if (!*out) { free(out); return xstrdup(is_dir ? "_/" : "_"); }

    if (is_dir) {
        size_t n = strlen(out);
        if (out[n - 1] != '/') { out[n] = '/'; out[n + 1] = '\0'; }
    }
    return out;
}

static int name_needs_utf8(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return 1;
    return 0;
}

static uint32_t external_attrs(const struct stat *st) {
    uint32_t unix_mode = (uint32_t)(st->st_mode & 0xFFFFu);
    uint32_t dos = S_ISDIR(st->st_mode) ? 0x10u : 0x00u;   /* FILE_ATTRIBUTE_DIRECTORY */
    if (!(st->st_mode & S_IWUSR)) dos |= 0x01u;            /* FILE_ATTRIBUTE_READONLY  */
    return (unix_mode << 16) | dos;
}

/* ------------------------------------------------------------------ */
/* Incompressibility probe                                             */
/* ------------------------------------------------------------------ */

static const char *const kIncompressibleExt[] = {
    "zip","gz","tgz","bz2","xz","zst","7z","rar","lz4","br","lzma","cab",
    "jpg","jpeg","png","gif","webp","heic","avif","jxl",
    "mp3","m4a","aac","ogg","opus","flac","wma",
    "mp4","m4v","mkv","avi","mov","webm","wmv","flv",
    "apk","jar","war","docx","xlsx","pptx","odt","ods","epub",
    "woff","woff2","crx","xpi","dmg","iso","deb","rpm", NULL
};

static int ext_is_incompressible(const char *name) {
    const char *dot = strrchr(name, '.');
    int i;
    if (!dot || !dot[1] || strchr(dot, '/')) return 0;
    for (i = 0; kIncompressibleExt[i]; i++) {
        const char *e = kIncompressibleExt[i];
        const char *p = dot + 1;
        size_t k = 0;
        while (e[k] && p[k] && tolower((unsigned char)p[k]) == e[k]) k++;
        if (!e[k] && !p[k]) return 1;
    }
    return 0;
}

/* Trial-deflate the head of the file at a cheap level. If DEFLATE cannot
   shave off at least ~3%, storing is both smaller (no per-block overhead)
   and far faster. Returns 1 when the member should be stored. */
static int probe_incompressible(FILE *in, uint64_t size, int level) {
    unsigned char *buf, *tmp;
    size_t want, got, bound;
    uLongf dlen;
    int store = 0;

    if (size < 4096) return 0;
    want = size < (uint64_t)PROBE_BYTES ? (size_t)size : (size_t)PROBE_BYTES;

    buf = xmalloc(want);
    got = fread(buf, 1, want, in);
    if (fseeko(in, 0, SEEK_SET) != 0 || got < 4096) { free(buf); return 0; }

    bound = (size_t)compressBound((uLong)got);
    tmp   = xmalloc(bound);
    dlen  = (uLongf)bound;
    if (compress2(tmp, &dlen, buf, (uLong)got, level < 3 ? level : 1) == Z_OK)
        store = ((uint64_t)dlen * 100u) >= ((uint64_t)got * 97u);

    free(tmp);
    free(buf);
    return store;
}

/* ------------------------------------------------------------------ */
/* Local / central headers                                             */
/* ------------------------------------------------------------------ */

static void write_local_header(zipw *z, entry_t *e) {
    unsigned char hdr[30];
    unsigned char extra[20];
    size_t extra_len = 0;

    if (e->zip64) {
        put16(extra + 0, 0x0001);
        put16(extra + 2, 16);
        put64(extra + 4, 0);        /* uncompressed size, patched later */
        put64(extra + 12, 0);       /* compressed size,   patched later */
        extra_len = 20;
    }

    put32(hdr + 0,  SIG_LOCAL);
    put16(hdr + 4,  (uint16_t)(e->zip64 ? 45 : 20));
    put16(hdr + 6,  e->flags);
    put16(hdr + 8,  e->method);
    put16(hdr + 10, e->mod_time);
    put16(hdr + 12, e->mod_date);
    put32(hdr + 14, 0);                                     /* CRC, patched   */
    put32(hdr + 18, e->zip64 ? (uint32_t)U32_MAX_V : 0);    /* csize, patched */
    put32(hdr + 22, e->zip64 ? (uint32_t)U32_MAX_V : 0);    /* usize, patched */
    put16(hdr + 26, (uint16_t)e->name_len);
    put16(hdr + 28, (uint16_t)extra_len);

    zw_write(z, hdr, sizeof(hdr));
    zw_write(z, e->name, e->name_len);
    if (extra_len) zw_write(z, extra, extra_len);
}

static void patch_local_header(zipw *z, entry_t *e) {
    unsigned char buf[16];
    uint64_t here = z->offset;

    /* The method is patched too: a member that was written as DEFLATE can be
       demoted to STORE afterwards when compression turned out to expand it. */
    put16(buf + 0, e->method);
    zw_seek(z, e->local_offset + 8);
    zw_write(z, buf, 2);

    put32(buf + 0, e->crc);
    put32(buf + 4, e->zip64 ? (uint32_t)U32_MAX_V : (uint32_t)e->comp_size);
    put32(buf + 8, e->zip64 ? (uint32_t)U32_MAX_V : (uint32_t)e->uncomp_size);
    zw_seek(z, e->local_offset + 14);
    zw_write(z, buf, 12);

    if (e->zip64) {
        put64(buf + 0, e->uncomp_size);
        put64(buf + 8, e->comp_size);
        zw_seek(z, e->local_offset + 30 + e->name_len + 4);
        zw_write(z, buf, 16);
    }
    zw_seek(z, here);
}

static void write_central_directory(zipw *z, uint64_t *cd_start, uint64_t *cd_size) {
    unsigned char hdr[46];
    unsigned char extra[32];
    size_t i;

    *cd_start = z->offset;

    for (i = 0; i < z->n; i++) {
        entry_t *e = &z->entries[i];
        size_t   ex = 0, body = 0;
        int      z64_u = e->uncomp_size  >= U32_MAX_V;
        int      z64_c = e->comp_size    >= U32_MAX_V;
        int      z64_o = e->local_offset >= U32_MAX_V;

        if (z64_u || z64_c || z64_o) {
            /* Fields appear in a fixed order and only when the 32-bit slot
               is saturated -- exactly as APPNOTE 4.5.3 requires. */
            if (z64_u) { put64(extra + 4 + body, e->uncomp_size);  body += 8; }
            if (z64_c) { put64(extra + 4 + body, e->comp_size);    body += 8; }
            if (z64_o) { put64(extra + 4 + body, e->local_offset); body += 8; }
            put16(extra + 0, 0x0001);
            put16(extra + 2, (uint16_t)body);
            ex = body + 4;
        }

        put32(hdr + 0,  SIG_CENTRAL);
        put16(hdr + 4,  (uint16_t)((3u << 8) | 63u));  /* made by UNIX, spec 6.3 */
        put16(hdr + 6,  (uint16_t)((ex || e->zip64) ? 45 : 20));
        put16(hdr + 8,  e->flags);
        put16(hdr + 10, e->method);
        put16(hdr + 12, e->mod_time);
        put16(hdr + 14, e->mod_date);
        put32(hdr + 16, e->crc);
        put32(hdr + 20, z64_c ? (uint32_t)U32_MAX_V : (uint32_t)e->comp_size);
        put32(hdr + 24, z64_u ? (uint32_t)U32_MAX_V : (uint32_t)e->uncomp_size);
        put16(hdr + 28, (uint16_t)e->name_len);
        put16(hdr + 30, (uint16_t)ex);
        put16(hdr + 32, 0);                            /* comment length      */
        put16(hdr + 34, 0);                            /* disk number start   */
        put16(hdr + 36, 0);                            /* internal attributes */
        put32(hdr + 38, e->ext_attr);
        put32(hdr + 42, z64_o ? (uint32_t)U32_MAX_V : (uint32_t)e->local_offset);

        zw_write(z, hdr, sizeof(hdr));
        zw_write(z, e->name, e->name_len);
        if (ex) zw_write(z, extra, ex);
    }

    *cd_size = z->offset - *cd_start;
}

static void write_end_records(zipw *z, uint64_t cd_start, uint64_t cd_size) {
    unsigned char buf[56];
    int need64 = (z->n > U16_MAX_V) || (cd_size >= U32_MAX_V) || (cd_start >= U32_MAX_V);

    if (need64) {
        uint64_t eocd64_off = z->offset;

        put32(buf + 0,  SIG_EOCD64);
        put64(buf + 4,  44);                            /* size of the rest   */
        put16(buf + 12, (uint16_t)((3u << 8) | 63u));
        put16(buf + 14, 45);
        put32(buf + 16, 0);                             /* this disk          */
        put32(buf + 20, 0);                             /* disk with CD start */
        put64(buf + 24, (uint64_t)z->n);
        put64(buf + 32, (uint64_t)z->n);
        put64(buf + 40, cd_size);
        put64(buf + 48, cd_start);
        zw_write(z, buf, 56);

        put32(buf + 0,  SIG_EOCD64_LOC);
        put32(buf + 4,  0);
        put64(buf + 8,  eocd64_off);
        put32(buf + 16, 1);
        zw_write(z, buf, 20);
    }

    put32(buf + 0,  SIG_EOCD);
    put16(buf + 4,  0);
    put16(buf + 6,  0);
    put16(buf + 8,  (uint16_t)(z->n > U16_MAX_V ? U16_MAX_V : z->n));
    put16(buf + 10, (uint16_t)(z->n > U16_MAX_V ? U16_MAX_V : z->n));
    put32(buf + 12, cd_size  >= U32_MAX_V ? (uint32_t)U32_MAX_V : (uint32_t)cd_size);
    put32(buf + 16, cd_start >= U32_MAX_V ? (uint32_t)U32_MAX_V : (uint32_t)cd_start);
    put16(buf + 20, 0);
    zw_write(z, buf, 22);
}

/* ------------------------------------------------------------------ */
/* Member payload                                                      */
/* ------------------------------------------------------------------ */

/* Collect the block at `seq`, append its bytes to the archive, and fold its
   CRC into the running one. Blocks are always drained in sequence order, so
   the output stream stays deterministic regardless of thread scheduling. */
static void drain_block(zipw *z, uint64_t seq, uint32_t *crc,
                        uint64_t *nout, int *failed) {
    mz_job *d = pool_await(&z->pool, seq);
    const unsigned char *src = d->store ? d->in : d->out;
    size_t n = d->store ? d->in_len : d->out_len;

    if (d->err) *failed = 1;
    zw_write(z, src, n);
    *nout += n;
    *crc = (uint32_t)crc32_combine((uLong)*crc, (uLong)d->crc, (z_off_t)d->in_len);
}

/* Stream `in` through the pool, writing DEFLATE (or STORE) output to the
   archive. Fills in e->crc, e->comp_size and e->uncomp_size. */
static int stream_member(zipw *z, entry_t *e, FILE *in, int store) {
    mz_pool *p = &z->pool;
    uint64_t seq_in = 0, seq_out = 0;
    uint32_t crc = (uint32_t)crc32(0L, Z_NULL, 0);
    uint64_t nin = 0, nout = 0;
    unsigned char tail[DICT_SIZE];
    size_t tail_len = 0;
    int eof = 0, failed = 0;

    while (!eof) {
        mz_job *j;

        /* Backpressure: never keep more than ring_n blocks in flight. */
        while (seq_in - seq_out >= (uint64_t)p->ring_n)
            drain_block(z, seq_out++, &crc, &nout, &failed);

        j = p->ring[seq_in % p->ring_n];
        j->in_len = fread(j->in, 1, z->opts.block, in);
        if (j->in_len < z->opts.block) {
            /* A read error is treated as end of input rather than an early
               return: the blocks already handed to workers must still be
               drained, or the next member would recycle buffers that a
               worker is still reading from. */
            if (ferror(in)) failed = 1;
            eof = 1;
        }
        nin += j->in_len;

        j->seq      = seq_in;
        j->store    = store;
        j->last     = eof;
        j->dict_len = store ? 0 : tail_len;
        if (j->dict_len) memcpy(j->dict, tail, j->dict_len);

        /* Snapshot the next block's dictionary while the producer still owns
           j->in and no worker can be reading it. */
        if (!store && j->in_len) {
            if (j->in_len >= DICT_SIZE) {
                memcpy(tail, j->in + j->in_len - DICT_SIZE, DICT_SIZE);
                tail_len = DICT_SIZE;
            } else {
                size_t keep = DICT_SIZE - j->in_len;
                if (keep > tail_len) keep = tail_len;
                memmove(tail, tail + tail_len - keep, keep);
                memcpy(tail + keep, j->in, j->in_len);
                tail_len = keep + j->in_len;
            }
        }

        pool_submit(p, j);
        seq_in++;
    }

    while (seq_out < seq_in)
        drain_block(z, seq_out++, &crc, &nout, &failed);

    if (failed) return -1;

    e->crc         = crc;
    e->uncomp_size = nin;
    e->comp_size   = nout;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static void human(uint64_t v, char *buf, size_t n) {
    static const char *const u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double d = (double)v;
    int i = 0;
    while (d >= 1024.0 && i < 4) { d /= 1024.0; i++; }
    if (i == 0) snprintf(buf, n, "%.0f %s", d, u[i]);
    else        snprintf(buf, n, "%.1f %s", d, u[i]);
}

static void report_entry(const entry_t *e) {
    double ratio;
    if (g_verbosity < 1) return;
    ratio = e->uncomp_size
          ? 100.0 * (1.0 - (double)e->comp_size / (double)e->uncomp_size)
          : 0.0;
    printf("  %-7s %5.1f%%  %12llu -> %-12llu  %s\n",
           e->method == METHOD_STORE ? "store" : "deflate",
           ratio,
           (unsigned long long)e->uncomp_size,
           (unsigned long long)e->comp_size,
           e->name);
}

/* ------------------------------------------------------------------ */
/* Adding members                                                      */
/* ------------------------------------------------------------------ */

static void add_directory_entry(zipw *z, const struct stat *st, char *name) {
    entry_t *e = zw_new_entry(z);
    e->name     = name;
    e->name_len = strlen(name);
    e->method   = METHOD_STORE;
    e->flags    = (uint16_t)(name_needs_utf8(name) ? FLAG_UTF8 : 0);
    e->ext_attr = external_attrs(st);
    dos_datetime(st->st_mtime, &e->mod_date, &e->mod_time);
    e->local_offset = z->offset;
    write_local_header(z, e);
    if (g_verbosity >= 2)
        printf("  %-7s %5s   %12s    %-12s  %s\n", "dir", "-", "-", "-", name);
}

#if !defined(_WIN32)
static void add_symlink(zipw *z, const char *fs_path, const struct stat *st, char *name) {
    entry_t *e;
    char    *target;
    ssize_t  n;
    size_t   cap = (size_t)(st->st_size > 0 ? st->st_size : 256) + 1;

    target = xmalloc(cap);
    n = readlink(fs_path, target, cap - 1);
    if (n < 0) {
        warn_msg("cannot read symlink '%s': %s", fs_path, strerror(errno));
        free(target); free(name);
        return;
    }
    target[n] = '\0';

    e = zw_new_entry(z);
    e->name        = name;
    e->name_len    = strlen(name);
    e->method      = METHOD_STORE;
    e->flags       = (uint16_t)(name_needs_utf8(name) ? FLAG_UTF8 : 0);
    e->ext_attr    = external_attrs(st);
    e->uncomp_size = (uint64_t)n;
    e->comp_size   = (uint64_t)n;
    e->crc         = (uint32_t)crc32(crc32(0L, Z_NULL, 0), (const Bytef *)target, (uInt)n);
    dos_datetime(st->st_mtime, &e->mod_date, &e->mod_time);

    e->local_offset = z->offset;
    write_local_header(z, e);
    zw_write(z, target, (size_t)n);
    patch_local_header(z, e);

    if (g_verbosity >= 2)
        printf("  %-7s %5s   %12s    %-12s  %s -> %s\n", "link", "-", "-", "-", e->name, target);
    free(target);
}
#endif

static void add_regular_file(zipw *z, const char *fs_path, const struct stat *st, char *name) {
    entry_t *e;
    FILE    *in;
    int      store;
    uint64_t data_start;

    /* Name lengths are 16-bit fields in both headers. */
    if (strlen(name) > U16_MAX_V) {
        warn_msg("skipping '%s': archive name exceeds 65535 bytes", fs_path);
        free(name);
        return;
    }

    in = fopen(fs_path, "rb");
    if (!in) {
        warn_msg("cannot open '%s': %s", fs_path, strerror(errno));
        free(name);
        return;
    }

    store = (z->opts.level == 0) || st->st_size == 0;
    if (!store && z->opts.probe)
        store = ext_is_incompressible(name)
             || probe_incompressible(in, (uint64_t)st->st_size, z->opts.level);

    e = zw_new_entry(z);
    e->name     = name;
    e->name_len = strlen(name);
    e->method   = (uint16_t)(store ? METHOD_STORE : METHOD_DEFLATE);
    e->flags    = (uint16_t)(name_needs_utf8(name) ? FLAG_UTF8 : 0);
    e->ext_attr = external_attrs(st);
    e->zip64    = (uint64_t)st->st_size >= U32_MAX_V;
    dos_datetime(st->st_mtime, &e->mod_date, &e->mod_time);

    e->local_offset = z->offset;
    write_local_header(z, e);
    data_start = z->offset;

    if (stream_member(z, e, in, store) != 0) {
        warn_msg("failed while compressing '%s'", fs_path);
        fclose(in);
        zw_seek(z, e->local_offset);   /* roll back the half-written member */
        free(name);
        z->n--;
        return;
    }

    /* If DEFLATE actually expanded the data, rewind and store it verbatim.
       The member is still the last thing in the file, so this is safe. */
    if (!store && e->comp_size >= e->uncomp_size && e->uncomp_size > 0) {
        unsigned char *buf = xmalloc(z->opts.block);
        size_t got;
        if (fseeko(in, 0, SEEK_SET) == 0) {
            zw_seek(z, data_start);
            while ((got = fread(buf, 1, z->opts.block, in)) > 0)
                zw_write(z, buf, got);
            if (!ferror(in)) {
                e->method    = METHOD_STORE;
                e->comp_size = e->uncomp_size;
            }
        }
        free(buf);
    }

    fclose(in);
    patch_local_header(z, e);

    z->total_in  += e->uncomp_size;
    z->total_out += e->comp_size;
    report_entry(e);
}

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void add_path(zipw *z, const char *fs_path);

static void add_dir_contents(zipw *z, const char *fs_path) {
    DIR    *d;
    char  **names = NULL;
    size_t  n = 0, cap = 0, i;
    struct dirent *de;

    d = opendir(fs_path);
    if (!d) {
        warn_msg("cannot open directory '%s': %s", fs_path, strerror(errno));
        return;
    }

    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; names = xrealloc(names, cap * sizeof(*names)); }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);

    /* Sort so that archives are byte-reproducible across filesystems. */
    qsort(names, n, sizeof(*names), name_cmp);

    for (i = 0; i < n; i++) {
        size_t len = strlen(fs_path) + strlen(names[i]) + 2;
        char  *child = xmalloc(len);
        snprintf(child, len, "%s/%s", fs_path, names[i]);
        add_path(z, child);
        free(child);
        free(names[i]);
    }
    free(names);
}

static void add_path(zipw *z, const char *fs_path) {
    struct stat st;
    char *name;
    int rc;

#if defined(_WIN32)
    rc = stat(fs_path, &st);
#else
    rc = z->opts.follow_links ? stat(fs_path, &st) : lstat(fs_path, &st);
#endif
    if (rc != 0) {
        warn_msg("cannot stat '%s': %s", fs_path, strerror(errno));
        return;
    }

    if (z->have_id && st.st_dev == z->dev && st.st_ino == z->ino) {
        if (g_verbosity >= 2) printf("  skip    (the archive itself)            %s\n", fs_path);
        return;
    }

    name = archive_name(fs_path, z->opts.junk_paths, S_ISDIR(st.st_mode));

    if (S_ISDIR(st.st_mode)) {
        if (!z->opts.recurse) {
            warn_msg("'%s' is a directory; use -r to include its contents", fs_path);
            free(name);
            return;
        }
        add_directory_entry(z, &st, name);
        add_dir_contents(z, fs_path);
        return;
    }

#if !defined(_WIN32)
    if (S_ISLNK(st.st_mode)) { add_symlink(z, fs_path, &st, name); return; }
#endif

    if (!S_ISREG(st.st_mode)) {
        warn_msg("skipping '%s' (not a regular file)", fs_path);
        free(name);
        return;
    }

    add_regular_file(z, fs_path, &st, name);
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static int cpu_count(void) {
#if defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    return (int)n;
#else
    return 4;
#endif
}

static size_t parse_size(const char *s) {
    char *end;
    double v = strtod(s, &end);
    if (end == s) bad_usage("bad size '%s'", s);
    switch (*end) {
        case 'k': case 'K': v *= 1024.0;                      break;
        case 'm': case 'M': v *= 1024.0 * 1024.0;             break;
        case 'g': case 'G': v *= 1024.0 * 1024.0 * 1024.0;    break;
        case '\0':                                            break;
        default: bad_usage("bad size suffix in '%s'", s);
    }
    if (v < 65536.0)          v = 65536.0;
    if (v > 256.0 * 1048576.0) v = 256.0 * 1048576.0;
    return (size_t)v;
}

static void usage(FILE *f) {
    fprintf(f,
"miniZip %s - a small, fast ZIP archiver\n"
"\n"
"Usage: %s [options] <archive.zip> <path>...\n"
"\n"
"Compression\n"
"  -0 .. -9              compression level (0 = store, default 6)\n"
"  -l, --level N         same as -N\n"
"  -s, --store           store everything, never DEFLATE\n"
"      --strategy NAME   default | filtered | huffman | rle  (default: default)\n"
"      --no-probe        do not trial-compress before choosing STORE\n"
"\n"
"Parallelism\n"
"  -T, --threads N       worker threads (0 = serial, default: CPU count)\n"
"  -b, --block SIZE      parallel block size, e.g. 256K, 1M (default 256K)\n"
"\n"
"Input selection\n"
"  -r, --recurse         descend into directories\n"
"  -j, --junk-paths      store bare filenames, discard directory components\n"
"  -L, --follow          follow symlinks instead of storing them as links\n"
"\n"
"Output\n"
"  -q, --quiet           report errors only\n"
"  -v, --verbose         per-entry detail\n"
"  -h, --help            this message\n"
"  -V, --version         version information\n"
"\n"
"Examples\n"
"  %s -9 -r site.zip public/\n"
"  %s -T 8 -b 1M backup.zip bigfile.img\n"
"  %s -j -1 quick.zip logs/access.log\n",
    MZ_VERSION, g_prog, g_prog, g_prog, g_prog);
}

static int parse_strategy(const char *s) {
    if (!strcmp(s, "default"))  return Z_DEFAULT_STRATEGY;
    if (!strcmp(s, "filtered")) return Z_FILTERED;
    if (!strcmp(s, "huffman"))  return Z_HUFFMAN_ONLY;
    if (!strcmp(s, "rle"))      return Z_RLE;
    bad_usage("unknown strategy '%s'", s);
    return Z_DEFAULT_STRATEGY;
}

static double now_seconds(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

int main(int argc, char **argv) {
    mz_opts  o;
    zipw     z;
    const char *archive;
    int      i, first = 0;
    uint64_t cd_start = 0, cd_size = 0;
    double   t0, elapsed;
    struct stat ast;
    char hin[32], hout[32];

    if (argv[0] && *argv[0]) {
        const char *b = strrchr(argv[0], '/');
        g_prog = b ? b + 1 : argv[0];
    }

    memset(&o, 0, sizeof(o));
    o.level    = 6;
    o.strategy = Z_DEFAULT_STRATEGY;
    o.threads  = -1;                 /* auto */
    o.block    = 256u * 1024u;
    o.probe    = 1;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (a[0] != '-' || a[1] == '\0') { first = i; break; }
        if (!strcmp(a, "--")) { first = i + 1; break; }

        if (a[1] >= '0' && a[1] <= '9' && a[2] == '\0') { o.level = a[1] - '0'; continue; }

        if (!strcmp(a, "-h") || !strcmp(a, "--help"))    { usage(stdout); return 0; }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("miniZip %s (zlib %s)\n", MZ_VERSION, zlibVersion());
            return 0;
        }
        if (!strcmp(a, "-q") || !strcmp(a, "--quiet"))      { g_verbosity = 0;    continue; }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose"))    { g_verbosity = 2;    continue; }
        if (!strcmp(a, "-r") || !strcmp(a, "--recurse"))    { o.recurse = 1;      continue; }
        if (!strcmp(a, "-j") || !strcmp(a, "--junk-paths")) { o.junk_paths = 1;   continue; }
        if (!strcmp(a, "-L") || !strcmp(a, "--follow"))     { o.follow_links = 1; continue; }
        if (!strcmp(a, "-s") || !strcmp(a, "--store"))      { o.level = 0;        continue; }
        if (!strcmp(a, "--no-probe"))                       { o.probe = 0;        continue; }

        if (!strcmp(a, "-l") || !strcmp(a, "--level")) {
            if (++i >= argc) bad_usage("%s needs an argument", a);
            o.level = atoi(argv[i]);
            if (o.level < 0 || o.level > 9) bad_usage("level must be 0..9");
            continue;
        }
        if (!strcmp(a, "-T") || !strcmp(a, "--threads")) {
            if (++i >= argc) bad_usage("%s needs an argument", a);
            o.threads = atoi(argv[i]);
            if (o.threads < 0) bad_usage("thread count must be >= 0");
            continue;
        }
        if (!strcmp(a, "-b") || !strcmp(a, "--block")) {
            if (++i >= argc) bad_usage("%s needs an argument", a);
            o.block = parse_size(argv[i]);
            continue;
        }
        if (!strcmp(a, "--strategy")) {
            if (++i >= argc) bad_usage("%s needs an argument", a);
            o.strategy = parse_strategy(argv[i]);
            continue;
        }
        bad_usage("unknown option '%s'", a);
    }

    if (!first || first >= argc) { usage(stderr); return 2; }
    archive = argv[first++];
    if (first >= argc) { fprintf(stderr, "%s: no input paths given\n", g_prog); return 2; }

    if (o.threads < 0) o.threads = cpu_count();
#if defined(MZ_NO_THREADS)
    o.threads = 0;
#endif

    memset(&z, 0, sizeof(z));
    z.opts = o;
    z.path = archive;
    z.fp   = fopen(archive, "wb+");
    if (!z.fp) die("cannot create '%s': %s", archive, strerror(errno));
    setvbuf(z.fp, NULL, _IOFBF, 1u << 20);

    if (fstat(fileno(z.fp), &ast) == 0) {
        z.dev = ast.st_dev; z.ino = ast.st_ino; z.have_id = 1;
    }

    pool_init(&z.pool, &o, o.block);

    if (g_verbosity >= 1)
        printf("%s: creating '%s'  (level %d, %d thread%s, %zu KiB blocks)\n",
               g_prog, archive, o.level, o.threads,
               o.threads == 1 ? "" : "s", o.block / 1024);

    t0 = now_seconds();
    for (i = first; i < argc; i++)
        add_path(&z, argv[i]);

    write_central_directory(&z, &cd_start, &cd_size);
    write_end_records(&z, cd_start, cd_size);
    elapsed = now_seconds() - t0;

    if (fflush(z.fp) != 0) die("flush failed: %s", strerror(errno));
#if !defined(_WIN32)
    /* A STORE fallback can shrink the archive below an earlier high-water
       mark; drop anything past the end of the EOCD record. */
    if (ftruncate(fileno(z.fp), (off_t)z.offset) != 0)
        warn_msg("could not truncate '%s': %s", archive, strerror(errno));
#endif
    if (fclose(z.fp) != 0) die("close failed: %s", strerror(errno));

    pool_free(&z.pool);

    if (g_verbosity >= 1) {
        double ratio = z.total_in
                     ? 100.0 * (1.0 - (double)z.total_out / (double)z.total_in) : 0.0;
        double mibps = elapsed > 0.0
                     ? ((double)z.total_in / 1048576.0) / elapsed : 0.0;
        human(z.total_in, hin, sizeof(hin));
        human(z.offset, hout, sizeof(hout));
        printf("%s: %zu entr%s, %s -> %s (%.1f%% saved) in %.2fs, %.1f MiB/s\n",
               g_prog, z.n, z.n == 1 ? "y" : "ies", hin, hout, ratio, elapsed, mibps);
        if (g_warnings)
            printf("%s: %d warning%s\n", g_prog, g_warnings, g_warnings == 1 ? "" : "s");
    }

    {
        size_t k;
        for (k = 0; k < z.n; k++) free(z.entries[k].name);
    }
    free(z.entries);
    return g_warnings ? 1 : 0;
}
