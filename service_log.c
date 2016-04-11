#define LOKI_MODULE
#include "loki_services.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <time.h>
#else
#include <sys/time.h>
#endif

#ifndef LK_DEFAULT_LOGPATH
# define LK_DEFAULT_LOGPATH "logs/%S_%Y%M%D.%I.log"
#endif
#define LK_MAX_CONFIGNAME  64
#define LK_MAX_CONFIGPATH  256

typedef struct lk_LogHeader {
    const char *level;
    const char *service;
    const char *tag;
    const char *key;
    const char *msg;
    size_t      msglen;
    struct tm   tm;
    lk_Buffer   buff;
} lk_LogHeader;

typedef enum lk_ConfigMaskFlag {
    lk_Mreload   = 1 << 0,
    lk_Mcolor    = 1 << 1,
    lk_Mscreen   = 1 << 2,
    lk_Minterval = 1 << 3,
    lk_Mfilepath = 1 << 4
} lk_ConfigMaskFlag;

typedef struct lk_Dumper {
    char   name[LK_MAX_CONFIGPATH];
    int    index;
    int    interval;
    time_t next_update;
    FILE  *fp;
} lk_Dumper;

typedef struct lk_LogConfig {
    char name[LK_MAX_CONFIGNAME];
    char filepath[LK_MAX_CONFIGPATH];
    unsigned mask;
    int color; /* 1248:RGBI, low 4bit: fg, high 4bit: bg */
    int screen;
    int interval;
    lk_Dumper *dumper;
} lk_LogConfig;

typedef struct lk_LogState {
    lk_State *S;
    lk_Table  config;
    lk_Table  dump;
    lk_MemPool configs;
    lk_MemPool dumpers;
} lk_LogState;

#define lkX_readinteger(ls, buff, config, key)   do { \
    char *s;                                           \
    lk_resetbuffer(buff);                              \
    lk_addfstring(buff, "log.%s." #key, config->name); \
    config->mask &= ~lk_M##key;                        \
    if ((s = lk_getconfig(ls->S, lk_buffer(buff)))) {  \
        config->key = atoi(s);                         \
        config->mask |= lk_M##key;                     \
        lk_delstring(ls->S, (lk_String*)s); }        } while(0)

#define lkX_readstring(ls, buff, config, key)    do { \
    char *s;                                           \
    lk_resetbuffer(buff);                              \
    lk_addfstring(buff, "log.%s." #key, config->name); \
    config->mask &= ~lk_M##key;                        \
    if ((s = lk_getconfig(ls->S, lk_buffer(buff)))) {  \
        lk_strcpy(config->key, lk_buffer(buff),        \
                LK_MAX_CONFIGPATH);                    \
        config->mask |= lk_M##key;                     \
        lk_delstring(ls->S, (lk_String*)s); }        } while(0)


/* config dumper */

static void lkX_localtime(time_t t, struct tm *tm)
#ifdef _MSC_VER
{ localtime_s(tm, &t); }
#elif _POSIX_SOURCE
{ localtime_r(&t, tm); }
#else
{ *tm = *localtime(&t); }
#endif 

static void lkX_settime(lk_Dumper* dumper, int interval) {
    struct tm tm;
    time_t now = time(NULL), daytm;
    lkX_localtime(now, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    daytm = mktime(&tm);
    dumper->interval = interval;
    dumper->index = (int)((now - daytm) / interval);
    dumper->next_update = daytm + (dumper->index + 1) * interval;
}

static void lkX_openfile(lk_LogState *ls, lk_Dumper* dumper) {
    lk_Buffer buff;
    struct tm tm;
    const char *s;
    lkX_localtime(time(NULL), &tm);
    lk_initbuffer(ls->S, &buff);
    for (s = dumper->name; *s != '\0'; ++s) {
        if (*s != '%') {
            lk_addchar(&buff, *s);
            continue;
        }
        switch (*++s) {
        case 'Y': lk_addfstring(&buff, "%04d", tm.tm_year + 1900); break;
        case 'M': lk_addfstring(&buff, "%02d", tm.tm_mon + 1); break;
        case 'D': lk_addfstring(&buff, "%02d", tm.tm_mday); break;
        case 'I': lk_addfstring(&buff, "%d", dumper->index); break;
        default:  lk_addchar(&buff, '%'); /* FALLTHROUGH */
        case '%': lk_addchar(&buff, *s); break;
        }
    }
    lk_addchar(&buff, '\0');
#ifdef _MSC_VER
    fopen_s(&dumper->fp, lk_buffer(&buff), "a");
#else
    dumper->fp = fopen(lk_buffer(&buff), "a");
#endif
    lk_freebuffer(&buff);
}

static lk_Dumper *lkX_newdumper(lk_LogState *ls, lk_LogConfig *config, lk_LogHeader *hs) {
    const char *s;
    lk_Buffer buff;
    lk_Dumper *dumper;
    lk_Entry *e;
    if (!(config->mask & lk_Mfilepath))
        return NULL;
    lk_initbuffer(ls->S, &buff);
    for (s = config->filepath; *s != '\0'; ++s) {
        if (*s != '%') {
            lk_addchar(&buff, *s);
            continue;
        }
        switch (*++s) {
        case 'L': lk_addstring(&buff, hs->level); break;
        case 'S': lk_addstring(&buff, hs->service); break;
        case 'T': lk_addstring(&buff, hs->tag); break;
        default:  lk_addchar(&buff, '%');
                  lk_addchar(&buff, *s); break;
        }
    }
    lk_addchar(&buff, '\0');
    e = lk_setentry(ls->S, &ls->dump, lk_buffer(&buff));
    if (e->key != lk_buffer(&buff)) return (lk_Dumper*)e->key;
    dumper = (lk_Dumper*)lk_poolalloc(ls->S, &ls->dumpers);
    memset(dumper, 0, sizeof(*dumper));
    lk_strcpy(dumper->name, lk_buffer(&buff), LK_MAX_CONFIGPATH);
    lk_freebuffer(&buff);
    if (config->interval > 0)
        lkX_settime(dumper, config->interval);
    lkX_openfile(ls, dumper);
    e->key = dumper->name;
    return dumper;
}

static int lkX_wheelfile(lk_LogState* ls, lk_Dumper* dumper) {
    if (time(NULL) > dumper->next_update) {
        if (dumper->fp) fclose(dumper->fp);
        lkX_settime(dumper, dumper->interval);
        lkX_openfile(ls, dumper);
    }
    return 0;
}


/* config reader */

static lk_LogConfig *lkX_newconfig(lk_LogState *ls, const char *name) {
    lk_Entry *e = lk_setentry(ls->S, &ls->config, name);
    lk_LogConfig *config = (lk_LogConfig*)e->key;
    if (e->key != name) return config;
    config = (lk_LogConfig*)lk_poolalloc(ls->S, &ls->configs);
    memset(config, 0, sizeof(*config));
    lk_strcpy(config->name, name, LK_MAX_CONFIGNAME);
    config->mask |= lk_Mreload;
    config->color = 0x77;
    e->key = config->name;
    return config;
}

static lk_LogConfig* lkX_getconfig(lk_LogState *ls, const char *name) {
    lk_LogConfig *config = lkX_newconfig(ls, name);
    if (config->mask & lk_Mreload) {
        lk_Buffer buff;
        lk_initbuffer(ls->S, &buff);
        lkX_readinteger(ls, &buff, config, color);
        lkX_readinteger(ls, &buff, config, screen);
        lkX_readinteger(ls, &buff, config, interval);
        lkX_readstring(ls,  &buff, config, filepath);
        lk_freebuffer(&buff);
    }
    return config;
}

static int lkX_mergeconfig(lk_LogConfig *c1, lk_LogConfig *c2) {
    if (c1 == NULL || c2 == NULL) return -1;
    if (c2->mask & lk_Mcolor)    c1->color = c2->color;
    if (c2->mask & lk_Mscreen)   c1->screen = c2->screen;
    if (c2->mask & lk_Minterval) c1->interval = c2->interval;
    if (c2->mask & lk_Mfilepath) lk_strcpy(c1->filepath, c2->filepath, LK_MAX_CONFIGPATH);
    c1->mask |= c2->mask;
    return 0;
}

static lk_LogConfig* lkX_setconfig(lk_LogState *ls, lk_LogHeader *hs) {
    lk_LogConfig *config = lkX_getconfig(ls, hs->key);
    if (config->mask & lk_Mreload) {
        lk_LogConfig *other = lkX_getconfig(ls, hs->level);
        config->screen = 1;
        if (other->mask != lk_Mreload)
            lkX_mergeconfig(config, other);
        other = lkX_getconfig(ls, hs->service);
        if (other->mask == lk_Mreload)
            other = lkX_getconfig(ls, "default_service");
        if (other->mask != lk_Mreload)
            lkX_mergeconfig(config, other);
        if (hs->tag) {
            other = lkX_getconfig(ls, hs->tag);
            if (other->mask != lk_Mreload)
                lkX_mergeconfig(config, other);
        }
        if ((config->mask & lk_Mfilepath) && config->dumper == NULL)
            config->dumper = lkX_newdumper(ls, config, hs);
        config->mask &= ~lk_Mreload;
    }
    return config;
}


/* config parser */

static void lkX_parseheader(lk_LogHeader *hs, const char* service, const char* s, size_t len) {
    const char *end = s + len;
    size_t offset_key = 0; /* key struct: [level][service][tag] */
    hs->level   = "info";
    hs->service = service;
    hs->tag     = NULL;
    hs->msg = s;
    if (len >= 3 && s[1] == '[') {
        const char *start = s + 2;
        switch (*s) {
        default: goto no_tag;
        case 'I': break;
        case 'T': hs->level = "trace"; break;
        case 'V': hs->level = "verbose"; break;
        case 'W': hs->level = "warning"; break;
        case 'E': hs->level = "error"; break;
        }
        for (s = start; s < end && *s != ']'; ++s)
            ;
        if (s == end) goto no_tag;
        if (s - start != 0) {
            lk_addlstring(&hs->buff, start, s - start);
            lk_addchar(&hs->buff, '\0');
            offset_key = lk_buffsize(&hs->buff);
        }
        hs->msg = *++s == ' ' ? s + 1 : s;
    }
no_tag:
    hs->msglen = end - hs->msg;
    lk_addfstring(&hs->buff, "[%s][%s][", hs->level, service);
    if (offset_key != 0) {
        lk_prepbuffsize(&hs->buff, offset_key-1);
        lk_addlstring(&hs->buff, lk_buffer(&hs->buff), offset_key-1);
        hs->tag = lk_buffer(&hs->buff);
    }
    lk_addlstring(&hs->buff, "]\0", 2);
    hs->key = lk_buffer(&hs->buff) + offset_key;
}

static void lkX_headerdump(lk_LogState *ls, lk_LogHeader *hs, FILE *fp) {
    struct tm *tm = &hs->tm;
    (void)ls;
    if (hs->tag) fprintf(fp, "[%c][%s][%02d:%02d:%02d][%s]: ", 
            toupper(hs->level[0]), hs->service,
            tm->tm_hour, tm->tm_min, tm->tm_sec, hs->tag);
    else fprintf(fp, "[%c][%s][%02d:%02d:%02d]: ", 
            toupper(hs->level[0]), hs->service,
            tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void lkX_filedump(lk_LogState *ls, lk_LogHeader *hs, lk_Dumper *dumper) {
    if (dumper->interval > 0 || !dumper->fp)
        lkX_wheelfile(ls, dumper);
    if (dumper->fp) {
        lkX_headerdump(ls, hs, dumper->fp);
        fwrite(hs->msg, 1, hs->msglen, dumper->fp);
        fputc('\n', dumper->fp);
        fflush(dumper->fp);
    }
}

static void lkX_screendump(lk_LogState *ls, const char *s, size_t len, int color)  {
#ifdef _WIN32
    lk_Buffer B;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD attr = 0, reset = FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE;
    DWORD written;
    LPWSTR buff = NULL;
    int bytes, cp = CP_UTF8;
    if (color & 0x01) attr |= FOREGROUND_RED;
    if (color & 0x02) attr |= FOREGROUND_GREEN;
    if (color & 0x04) attr |= FOREGROUND_BLUE;
    if (color & 0x08) attr |= FOREGROUND_INTENSITY;
    if (color & 0x10) attr |= BACKGROUND_RED;
    if (color & 0x20) attr |= BACKGROUND_GREEN;
    if (color & 0x40) attr |= BACKGROUND_BLUE;
    if (color & 0x80) attr |= BACKGROUND_INTENSITY;
    bytes = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, NULL, 0);
    lk_initbuffer(ls->S, &B);
    if (bytes != 0) {
        buff = (LPWSTR)lk_prepbuffsize(&B, bytes * sizeof(WCHAR));
        bytes = MultiByteToWideChar(cp, 0, s, len, buff, bytes);
    }
    SetConsoleTextAttribute(h, attr);
    if (buff) WriteConsoleW(h, buff, bytes, &written, NULL);
    else      WriteConsoleA(h, s, len, &written, NULL);
    SetConsoleTextAttribute(h, reset);
    lk_freebuffer(&B);
    fputc('\n', stdout);
#else
    int fg = 30 + (color & 0x7), bg = 40 + ((color & 0x70)>>4);
    (void)ls;
    if (color & 0x08) fg += 60;
    if (color & 0x80) bg += 60;
    fprintf(stdout, "\e[%d;%dm%.*s\e[0m\n", fg, bg, (int)len, s);
#endif
}

static int lkX_writelog(lk_LogState *ls, const char* service_name, char* s, size_t len) {
    lk_LogConfig *config;
    lk_LogHeader hs;
    lk_initbuffer(ls->S, &hs.buff);
    lkX_parseheader(&hs, service_name, s, len);
    config = lkX_setconfig(ls, &hs);
    lkX_localtime(time(NULL), &hs.tm);
    if (config->dumper)
        lkX_filedump(ls, &hs, config->dumper);
    if (config->screen) {
        lkX_headerdump(ls, &hs, stdout);
        lkX_screendump(ls, hs.msg, hs.msglen, config->color);
    }
    lk_freebuffer(&hs.buff);
    return 0;
}


/* config initialize */

static lk_LogState *lkX_newstate(lk_State *S) {
    lk_LogState *ls = (lk_LogState*)lk_malloc(S, sizeof(lk_LogState));
    lk_LogConfig *config;
    ls->S = S;
    lk_inittable(&ls->config);
    lk_inittable(&ls->dump);
    lk_initmempool(&ls->configs, sizeof(lk_LogConfig), 0);
    lk_initmempool(&ls->dumpers, sizeof(lk_Dumper), 0);

    /* initialize config */
    config = lkX_newconfig(ls, "info");
    config->screen = 1;
    config->color = 0x07;
    config->mask = lk_Mscreen|lk_Mcolor;
    config = lkX_newconfig(ls, "trace");
    config->screen = 1;
    config->color = 0x0F;
    config->mask = lk_Mscreen|lk_Mcolor;
    config = lkX_newconfig(ls, "verbose");
    config->screen = 1;
    config->color = 0x70;
    config->mask = lk_Mscreen|lk_Mcolor;
    config = lkX_newconfig(ls, "warning");
    config->screen = 1;
    config->color = 0x0B;
    config->mask = lk_Mscreen|lk_Mcolor;
    config = lkX_newconfig(ls, "error");
    config->screen = 1;
    config->color = 0x9F;
    config->mask = lk_Mscreen|lk_Mcolor;
    config = lkX_newconfig(ls, "default_service");
    lk_strcpy(config->filepath, LK_DEFAULT_LOGPATH, LK_MAX_CONFIGPATH);
    config->interval = 3600;
    config->mask = lk_Mfilepath|lk_Minterval;

    return ls;
}

static void lkX_delstate(lk_LogState* ls) {
    lk_freetable(ls->S, &ls->config);
    lk_freetable(ls->S, &ls->dump);
    lk_freemempool(ls->S, &ls->configs);
    lk_freemempool(ls->S, &ls->dumpers);
    lk_free(ls->S, ls, sizeof(lk_LogState));
}

static int lkX_update(lk_State *S, lk_Slot *slot, lk_Signal *sig) {
    lk_LogState *ls = (lk_LogState*)lk_data(slot);
    lk_Entry *e = NULL;
    (void)sig;
    while (lk_nextentry(&ls->config, &e)) {
        lk_LogConfig *config = (lk_LogConfig*)e->key;
        config->mask |= lk_Mreload;
        config->dumper = NULL;
    }
    while (lk_nextentry(&ls->dump, &e)) {
        lk_Dumper *dumper = (lk_Dumper*)e->key;
        if (dumper->fp) fclose(dumper->fp);
        lk_poolfree(S, &ls->dumpers, dumper);
        e->key = NULL;
    }
    return LK_OK;
}

LKMOD_API int loki_service_log(lk_State *S, lk_Slot *slot, lk_Signal *sig) {
    if (slot == NULL) {
        lk_LogState *ls = lkX_newstate(S);
        lk_Service *svr = lk_self(S);
        lk_setdata((lk_Slot*)svr, ls);
        lk_newslot(S, "update", lkX_update, ls);
        return LK_WEAK;
    }
    else {
        lk_LogState *ls = (lk_LogState*)lk_data(slot);
        if (!sig) lkX_delstate(ls);
        else if (sig->data)
            lkX_writelog(ls, (const char*)sig->src, (char*)sig->data, sig->size);
        return LK_OK;
    }
}

