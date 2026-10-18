/*
 * Test runner for qsp.
 *
 * Runs scenario files (*.qspt) against the public API of the library:
 * builds a game from qsps-like text, executes commands, records the
 * callbacks of the engine as events and checks the state of the engine.
 * See tests/README.md for the description of the format.
 *
 * Ported from the runner of qsp-legacy and adapted to the 5.9 API.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qsp_default.h"

#define MAX_LINE 4096
#define MAX_TEXT (64 * 1024)
#define MAX_LIST_ITEMS 1000
#define SAVE_BUF_SIZE (64 * 1024)
#define RESULT_VAR "QSPT_EXPECT_RESULT"

/* ------------------------------------------------------------ */
/* Small helpers */

static void *xmalloc(size_t size)
{
    void *ptr = malloc(size ? size : 1);
    if (!ptr)
    {
        fputs("out of memory\n", stderr);
        exit(2);
    }
    return ptr;
}

static void *xrealloc(void *ptr, size_t size)
{
    ptr = realloc(ptr, size ? size : 1);
    if (!ptr)
    {
        fputs("out of memory\n", stderr);
        exit(2);
    }
    return ptr;
}

static char *xstrdup(const char *s)
{
    size_t len = strlen(s);
    char *ret = xmalloc(len + 1);
    memcpy(ret, s, len + 1);
    return ret;
}

typedef struct
{
    char *Data;
    size_t Len;
    size_t Size;
    int Lines; /* lines added with addTextLine() */
} StrBuf;

static void sbAddN(StrBuf *sb, const char *s, size_t len)
{
    if (sb->Len + len + 1 > sb->Size)
    {
        sb->Size = (sb->Len + len + 1) * 2;
        sb->Data = xrealloc(sb->Data, sb->Size);
    }
    memcpy(sb->Data + sb->Len, s, len);
    sb->Len += len;
    sb->Data[sb->Len] = 0;
}

static void sbAdd(StrBuf *sb, const char *s)
{
    sbAddN(sb, s, strlen(s));
}

static void sbAddf(StrBuf *sb, const char *fmt, ...)
{
    char buf[MAX_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    sbAdd(sb, buf);
}

static void sbClear(StrBuf *sb)
{
    sb->Len = 0;
    sb->Lines = 0;
    if (sb->Data) sb->Data[0] = 0;
}

static const char *sbStr(StrBuf *sb)
{
    return sb->Data ? sb->Data : "";
}

/* Takes the buffer of the StrBuf as a string, never NULL */
static char *sbTake(StrBuf *sb)
{
    char *ret = sb->Data ? sb->Data : xstrdup("");
    memset(sb, 0, sizeof(StrBuf));
    return ret;
}

/* ------------------------------------------------------------ */
/* UTF-8 <-> QSP_CHAR (wchar_t: UTF-16 or UTF-32 depending on the platform) */

static QSP_CHAR *toQsp(const char *s)
{
    size_t len = strlen(s), i = 0, n = 0;
    QSP_CHAR *ret = xmalloc((len + 1) * 2 * sizeof(QSP_CHAR));
    while (i < len)
    {
        unsigned long cp;
        unsigned char c = (unsigned char)s[i];
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else { cp = c & 0x07; extra = 3; }
        ++i;
        while (extra-- > 0 && i < len)
            cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
        if (cp >= 0x10000 && sizeof(QSP_CHAR) == 2)
        {
            cp -= 0x10000;
            ret[n++] = (QSP_CHAR)(0xD800 + (cp >> 10));
            ret[n++] = (QSP_CHAR)(0xDC00 + (cp & 0x3FF));
        }
        else
            ret[n++] = (QSP_CHAR)cp;
    }
    ret[n] = 0;
    return ret;
}

static void addUtf8(StrBuf *sb, unsigned long cp)
{
    char buf[4];
    if (cp < 0x80)
    {
        buf[0] = (char)cp;
        sbAddN(sb, buf, 1);
    }
    else if (cp < 0x800)
    {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        sbAddN(sb, buf, 2);
    }
    else if (cp < 0x10000)
    {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        sbAddN(sb, buf, 3);
    }
    else
    {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        sbAddN(sb, buf, 4);
    }
}

/* Appends a QSP string escaping \r, \n and \ so that every event fits into a line */
static void addEscaped(StrBuf *sb, QSPString s)
{
    const QSP_CHAR *pos = s.Str, *end = s.End;
    if (!pos) return;
    while (pos < end)
    {
        unsigned long cp = (unsigned long)*pos++;
        if (sizeof(QSP_CHAR) == 2 && cp >= 0xD800 && cp < 0xDC00 && pos < end &&
            (unsigned long)*pos >= 0xDC00 && (unsigned long)*pos < 0xE000)
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned long)*pos++ - 0xDC00);
        if (cp == '\r') sbAdd(sb, "\\r");
        else if (cp == '\n') sbAdd(sb, "\\n");
        else if (cp == '\\') sbAdd(sb, "\\\\");
        else addUtf8(sb, cp);
    }
}

static int isEmptyStr(QSPString s)
{
    return !s.Str || s.Str == s.End;
}

static char *toUtf8(QSPString s)
{
    StrBuf sb = {0};
    const QSP_CHAR *pos;
    for (pos = s.Str; pos && pos < s.End; ++pos) addUtf8(&sb, (unsigned long)*pos);
    return sbTake(&sb);
}

/* Converts \r, \n, \\ escapes in expectations to the real characters */
static char *unescape(const char *s)
{
    char *ret = xmalloc(strlen(s) + 1), *dst = ret;
    while (*s)
    {
        if (*s == '\\' && s[1])
        {
            ++s;
            if (*s == 'r') *dst++ = '\r';
            else if (*s == 'n') *dst++ = '\n';
            else *dst++ = *s;
            ++s;
        }
        else
            *dst++ = *s++;
    }
    *dst = 0;
    return ret;
}

/* ------------------------------------------------------------ */
/* Game description and the game file writer */

typedef struct
{
    char *Name;
    char *Image;
    StrBuf Code;
} Action;

typedef struct
{
    char *Name;
    StrBuf Desc;
    StrBuf Code;
    Action *Actions;
    int ActionsCount;
} Location;

typedef struct
{
    char *Name; /* module name for INCLIB/OPENQST, NULL for the main game */
    Location *Locs;
    int LocsCount;
} Game;

static Location *gameLoc(Game *game, const char *name)
{
    int i;
    Location *loc;
    for (i = 0; i < game->LocsCount; ++i)
        if (!strcmp(game->Locs[i].Name, name)) return game->Locs + i;
    game->Locs = xrealloc(game->Locs, (game->LocsCount + 1) * sizeof(Location));
    loc = game->Locs + game->LocsCount++;
    memset(loc, 0, sizeof(Location));
    loc->Name = xstrdup(name);
    return loc;
}

static void freeGame(Game *game)
{
    int i, j;
    for (i = 0; i < game->LocsCount; ++i)
    {
        Location *loc = game->Locs + i;
        free(loc->Name);
        free(loc->Desc.Data);
        free(loc->Code.Data);
        for (j = 0; j < loc->ActionsCount; ++j)
        {
            free(loc->Actions[j].Name);
            free(loc->Actions[j].Image);
            free(loc->Actions[j].Code.Data);
        }
        free(loc->Actions);
    }
    free(game->Locs);
    free(game->Name);
    memset(game, 0, sizeof(Game));
}

/* Adds a line of the text joining lines with \r\n like the editors do, empty lines count too */
static void addTextLine(StrBuf *sb, const char *line)
{
    if (sb->Lines++) sbAdd(sb, "\r\n");
    sbAdd(sb, line);
}

/* Parses locations in the qsps format: "# name", code lines, a line starting with "--" */
static void parseQsps(Game *game, char **lines, int count)
{
    int i;
    Location *loc = 0;
    for (i = 0; i < count; ++i)
    {
        const char *line = lines[i];
        if (!loc)
        {
            if (line[0] == '#')
            {
                const char *name = line + 1;
                while (*name == ' ') ++name;
                loc = gameLoc(game, name);
                sbClear(&loc->Code);
            }
        }
        else if (line[0] == '-' && line[1] == '-')
            loc = 0;
        else
            addTextLine(&loc->Code, line);
    }
}

typedef struct
{
    unsigned char *Data;
    size_t Len;
    size_t Size;
} ByteBuf;

static void bbAddUnit(ByteBuf *bb, unsigned int unit)
{
    if (bb->Len + 2 > bb->Size)
    {
        bb->Size = (bb->Len + 2) * 2;
        bb->Data = xrealloc(bb->Data, bb->Size);
    }
    bb->Data[bb->Len++] = (unsigned char)(unit & 0xFF);
    bb->Data[bb->Len++] = (unsigned char)((unit >> 8) & 0xFF);
}

/* Writes a field of the game file in UCS-2LE, coded fields are shifted by 5 like the engine expects */
static void writeField(ByteBuf *bb, const char *text, int isCoded, int isFirst)
{
    QSP_CHAR *str = toQsp(text), *ptr;
    if (!isFirst)
    {
        bbAddUnit(bb, '\r');
        bbAddUnit(bb, '\n');
    }
    for (ptr = str; *ptr; ++ptr)
    {
        unsigned long cp = (unsigned long)*ptr;
        unsigned int units[2];
        int i, count = 1;
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            units[0] = (unsigned int)(0xD800 + (cp >> 10));
            units[1] = (unsigned int)(0xDC00 + (cp & 0x3FF));
            count = 2;
        }
        else
            units[0] = (unsigned int)cp;
        for (i = 0; i < count; ++i)
        {
            unsigned int ch = units[i];
            if (isCoded) ch = (ch == 5 ? (unsigned int)(0x10000 - 5) : (ch - 5) & 0xFFFF);
            bbAddUnit(bb, ch);
        }
    }
    free(str);
}

static void writeCodedInt(ByteBuf *bb, int value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    writeField(bb, buf, 1, 0);
}

static ByteBuf buildGameFile(Game *game)
{
    int i, j;
    ByteBuf bb = {0};
    writeField(&bb, "QSPGAME", 0, 1);
    writeField(&bb, "qsp-tests", 0, 0);
    writeField(&bb, "", 1, 0);
    writeCodedInt(&bb, game->LocsCount);
    for (i = 0; i < game->LocsCount; ++i)
    {
        Location *loc = game->Locs + i;
        writeField(&bb, loc->Name, 1, 0);
        writeField(&bb, sbStr(&loc->Desc), 1, 0);
        writeField(&bb, sbStr(&loc->Code), 1, 0);
        writeCodedInt(&bb, loc->ActionsCount);
        for (j = 0; j < loc->ActionsCount; ++j)
        {
            writeField(&bb, loc->Actions[j].Image ? loc->Actions[j].Image : "", 1, 0);
            writeField(&bb, loc->Actions[j].Name, 1, 0);
            writeField(&bb, sbStr(&loc->Actions[j].Code), 1, 0);
        }
    }
    /* Every field ends with a line break like in the files of the editors, the engine expects the last one */
    bbAddUnit(&bb, '\r');
    bbAddUnit(&bb, '\n');
    return bb;
}

/* ------------------------------------------------------------ */
/* State of the running test */

typedef struct
{
    char *Name;
    void *Data;
    int Size;
} SaveSlot;

typedef struct
{
    char *Name;
    int IsStatement;
    int IsDb; /* one of the functions of qsp.db */
    char **Replies; /* actions of the next calls */
    int RepliesCount;
} HostFunc;

static struct
{
    const char *FileName;
    int Line; /* line of the current command, for messages */
    char *TestName;
    int IsSkipped;
    int IsFailed;
    int Passed, Failed, Skipped;

    Game Main;
    Game *Modules;
    int ModulesCount;
    int IsGameDirty;
    int IsGameLoaded;

    char **Events;
    int EventsCount;

    char *PendingError; /* error that hasn't been checked by the test yet */
    char *LastErrorDesc; /* description of the last error */

    char **InputReplies;
    int InputRepliesCount;
    char **VersionReplies;
    int VersionRepliesCount;
    int MenuReplies[64];
    int MenuRepliesCount;
    int IsPlayReplies[64];
    int IsPlayRepliesCount;
    int MsCount;
    char **MsgWatches; /* variables read while the MSG callback runs */
    int MsgWatchesCount;
    char **MsgEvents; /* incoming events tried while the MSG callback runs */
    int MsgEventsCount;
    char **QueuedEvents; /* incoming events scheduled by the host functions */
    int QueuedEventsCount;

    HostFunc *HostFuncs; /* functions & statements registered by the test */
    int HostFuncsCount;
    char **DbKeys; /* the store of qsp.db: formatted keys & values, it survives @reinit */
    char **DbValues;
    int DbCount;

    SaveSlot *Saves;
    int SavesCount;
} T;

static void fail(const char *fmt, ...)
{
    va_list args;
    if (T.IsFailed) return; /* report only the first problem of a test */
    T.IsFailed = 1;
    fprintf(stderr, "FAIL %s:%d [%s]\n  ", T.FileName, T.Line, T.TestName ? T.TestName : "");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static void addEvent(const char *event)
{
    T.Events = xrealloc(T.Events, (T.EventsCount + 1) * sizeof(char *));
    T.Events[T.EventsCount++] = xstrdup(event);
}

static void clearEvents(void)
{
    int i;
    for (i = 0; i < T.EventsCount; ++i) free(T.Events[i]);
    free(T.Events);
    T.Events = 0;
    T.EventsCount = 0;
}

static void dumpEvents(void)
{
    int i;
    fprintf(stderr, "  events so far:\n");
    for (i = 0; i < T.EventsCount; ++i) fprintf(stderr, "    %s\n", T.Events[i]);
    if (!T.EventsCount) fprintf(stderr, "    (none)\n");
}

/* Removes the first item of a queue of replies */
static void shiftStrings(char **items, int *count)
{
    int i;
    free(items[0]);
    for (i = 1; i < *count; ++i) items[i - 1] = items[i];
    --*count;
}

static int shiftInt(int *items, int *count, int defValue)
{
    int i, ret;
    if (!*count) return defValue;
    ret = items[0];
    for (i = 1; i < *count; ++i) items[i - 1] = items[i];
    --*count;
    return ret;
}

/* ------------------------------------------------------------ */
/* Callbacks: every call becomes an event named like in qsp-wasm-engine */

static void addListItem(StrBuf *sb, QSPString name, QSPString image)
{
    addEscaped(sb, name);
    if (!isEmptyStr(image))
    {
        sbAdd(sb, "|");
        addEscaped(sb, image);
    }
}

static void listState(StrBuf *sb, int isActions)
{
    int i, count;
    sbAdd(sb, "[");
    if (isActions)
    {
        QSPListItem items[MAX_LIST_ITEMS];
        count = QSPGetActions(items, MAX_LIST_ITEMS);
        if (count > MAX_LIST_ITEMS) count = MAX_LIST_ITEMS;
        for (i = 0; i < count; ++i)
        {
            if (i) sbAdd(sb, "; ");
            addListItem(sb, items[i].Name, items[i].Image);
        }
    }
    else
    {
        QSPObjectItem items[MAX_LIST_ITEMS];
        count = QSPGetObjects(items, MAX_LIST_ITEMS);
        if (count > MAX_LIST_ITEMS) count = MAX_LIST_ITEMS;
        for (i = 0; i < count; ++i)
        {
            if (i) sbAdd(sb, "; ");
            addListItem(sb, items[i].Name, items[i].Image);
            /* The title is shown only when it differs from the name (MODOBJ) */
            if (items[i].Title.Str != items[i].Name.Str)
            {
                sbAdd(sb, " => ");
                addEscaped(sb, items[i].Title);
            }
        }
    }
    sbAdd(sb, "]");
}

static void eventWithText(const char *name, QSPString text, int isNullable)
{
    StrBuf sb = {0};
    sbAdd(&sb, name);
    /* An empty text is logged without the text, so the event has no trailing space */
    if (!isNullable || !isEmptyStr(text))
    {
        sbAdd(&sb, " ");
        addEscaped(&sb, text);
    }
    addEvent(sbStr(&sb));
    free(sb.Data);
}

static void eventWithList(const char *name, int isActions)
{
    StrBuf sb = {0};
    sbAdd(&sb, name);
    sbAdd(&sb, " ");
    listState(&sb, isActions);
    addEvent(sbStr(&sb));
    free(sb.Data);
}

/* Like onRefresh() of qsp-wasm-engine: isForced updates every window */
static void refreshState(QSP_BOOL isForced)
{
    int changed = QSPGetWindowsChangedState();
    if (isForced || (changed & QSP_WIN_MAIN)) eventWithText("main_changed", QSPGetMainDesc(), 1);
    if (isForced || (changed & QSP_WIN_VARS)) eventWithText("stats_changed", QSPGetVarsDesc(), 1);
    if (isForced || (changed & QSP_WIN_ACTS)) eventWithList("actions_changed", 1);
    if (isForced || (changed & QSP_WIN_OBJS)) eventWithList("objects_changed", 0);
}

static void cbRefresh(QSP_BOOL isForced, QSP_BOOL QSP_UNUSED_isNewDesc)
{
    (void)QSP_UNUSED_isNewDesc;
    refreshState(isForced);
}

static void cbDebug(QSPString str)
{
    /* debug LOCATION:LINE:ACTION CODE */
    StrBuf sb = {0};
    QSPString loc = {0, 0};
    int actIndex = 0, line = 0;
    QSPGetCurStateData(&loc, &actIndex, &line);
    sbAdd(&sb, "debug ");
    addEscaped(&sb, loc);
    sbAddf(&sb, ":%d:%d ", line, actIndex);
    addEscaped(&sb, str);
    addEvent(sbStr(&sb));
    free(sb.Data);
}

static void formatVariant(StrBuf *sb, const QSPVariant *v);

/* Reads a variable with QSPGetVarValue, a type prefix of the name gives the typed view */
static void readVariable(StrBuf *sb, const char *name)
{
    QSPVariant value, empty;
    QSP_CHAR *qName = toQsp(*name == '$' || *name == '#' || *name == '%' ? name + 1 : name), *pos;
    for (pos = qName; *pos; ++pos)
        if (*pos >= 'a' && *pos <= 'z') *pos -= 'a' - 'A';
    QSPGetVarValue(QSPStringFromC(qName), 0, &value, 0);
    memset(&empty, 0, sizeof(empty));
    switch (*name)
    {
    case '$': empty.Type = QSP_TYPE_STR; break;
    case '#': empty.Type = QSP_TYPE_NUM; break;
    case '%': empty.Type = QSP_TYPE_TUPLE; break;
    }
    if (QSP_ISDEF(empty.Type) && QSP_BASETYPE(value.Type) != empty.Type)
        formatVariant(sb, &empty);
    else
        formatVariant(sb, &value);
    free(qName);
}

static int runEvent(const char *spec, QSP_BOOL toRefreshUI, QSP_BOOL *isOk);

static void cbMsg(QSPString str)
{
    int i;
    QSP_BOOL isOk;
    eventWithText("msg", str, 0);
    for (i = 0; i < T.MsgWatchesCount; ++i)
    {
        StrBuf sb = {0};
        sbAddf(&sb, "read %s => ", T.MsgWatches[i]);
        readVariable(&sb, T.MsgWatches[i]);
        addEvent(sbStr(&sb));
        free(sb.Data);
    }
    for (i = 0; i < T.MsgEventsCount; ++i)
    {
        /* The callbacks can't run the incoming events */
        if (runEvent(T.MsgEvents[i], QSP_FALSE, &isOk))
            addEvent(isOk ? "msg-event 1" : "msg-event 0");
    }
}
static void cbView(QSPString file) { eventWithText("view", file, 1); }
static void cbSetInput(QSPString text) { eventWithText("user_input", text, 1); }
static void cbCloseFile(QSPString file) { eventWithText("close_file", file, 1); }

static void cbSystem(QSPString str)
{
    /* qsp-wasm-engine updates the UI before it reports the command */
    refreshState(QSP_FALSE);
    eventWithText("system_cmd", str, 0);
}

static void cbPlayFile(QSPString file, int volume)
{
    StrBuf sb = {0};
    sbAdd(&sb, "play_file ");
    addEscaped(&sb, file);
    sbAddf(&sb, " %d", volume);
    addEvent(sbStr(&sb));
    free(sb.Data);
}

static QSP_BOOL cbIsPlay(QSPString file)
{
    eventWithText("is_play", file, 0);
    return (QSP_BOOL)(shiftInt(T.IsPlayReplies, &T.IsPlayRepliesCount, 0) != 0);
}

static void cbShowWindow(int type, QSP_BOOL isShow)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "panel_visibility %d %d", type, isShow ? 1 : 0);
    addEvent(buf);
}

static int cbMenu(QSPListItem *items, int count)
{
    StrBuf sb = {0};
    int i;
    sbAdd(&sb, "menu [");
    for (i = 0; i < count; ++i)
    {
        if (i) sbAdd(&sb, "; ");
        addListItem(&sb, items[i].Name, items[i].Image);
    }
    sbAdd(&sb, "]");
    addEvent(sbStr(&sb));
    free(sb.Data);
    return shiftInt(T.MenuReplies, &T.MenuRepliesCount, -1);
}

/* Copies the first queued reply to the buffer of the engine */
static void replyText(char **replies, int *count, QSP_CHAR *buffer, int maxLen)
{
    buffer[0] = 0;
    if (*count)
    {
        QSP_CHAR *reply = toQsp(replies[0]);
        int len = 0;
        while (reply[len] && len < maxLen) ++len;
        memcpy(buffer, reply, len * sizeof(QSP_CHAR));
        buffer[len] = 0;
        free(reply);
        shiftStrings(replies, count);
    }
}

static void cbInput(QSPString text, QSP_CHAR *buffer, int maxLen)
{
    eventWithText("input", text, 0);
    replyText(T.InputReplies, &T.InputRepliesCount, buffer, maxLen);
}

static void cbVersion(QSPString param, QSP_CHAR *buffer, int maxLen)
{
    eventWithText("version", param, 1);
    replyText(T.VersionReplies, &T.VersionRepliesCount, buffer, maxLen);
}

static void cbSleep(int msecs)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "wait %d", msecs);
    addEvent(buf);
}

static void cbSetTimer(int msecs)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "timer %d", msecs);
    addEvent(buf);
}

static int cbGetMsCount(void)
{
    /* Not logged: the engine asks for the time all the time */
    return T.MsCount;
}

static void loadGame(Game *game, QSP_BOOL isNewGame);
static void afterCall(QSP_BOOL isOk);

static void cbOpenGame(QSPString file, QSP_BOOL isNewGame)
{
    StrBuf sb = {0};
    char *name = toUtf8(file);
    int i;
    sbAdd(&sb, "open_game ");
    addEscaped(&sb, file);
    sbAddf(&sb, " %d", isNewGame ? 1 : 0);
    addEvent(sbStr(&sb));
    free(sb.Data);
    for (i = 0; i < T.ModulesCount; ++i)
    {
        if (!strcmp(T.Modules[i].Name, name))
        {
            loadGame(T.Modules + i, isNewGame);
            break;
        }
    }
    free(name);
}

static SaveSlot *findSave(const char *name, int toCreate)
{
    int i;
    for (i = 0; i < T.SavesCount; ++i)
        if (!strcmp(T.Saves[i].Name, name)) return T.Saves + i;
    if (!toCreate) return 0;
    T.Saves = xrealloc(T.Saves, (T.SavesCount + 1) * sizeof(SaveSlot));
    T.Saves[T.SavesCount].Name = xstrdup(name);
    T.Saves[T.SavesCount].Data = 0;
    T.Saves[T.SavesCount].Size = 0;
    return T.Saves + T.SavesCount++;
}

/* Saves the state like saveGameData() of qsp-wasm-engine, returns the data or NULL */
static void *saveState(int *size)
{
    int bufSize = SAVE_BUF_SIZE;
    void *buf = xmalloc(bufSize);
    if (!QSPSaveGameAsData(buf, &bufSize, QSP_FALSE))
    {
        /* The required size comes back when the buffer is too small */
        while (bufSize)
        {
            buf = xrealloc(buf, bufSize);
            if (QSPSaveGameAsData(buf, &bufSize, QSP_FALSE)) break;
        }
        if (!bufSize)
        {
            free(buf);
            /* $ONGSAVE that moves to another location cancels the save without an error */
            if (QSPGetLastErrorData().ErrorNum)
                afterCall(QSP_FALSE);
            else
                addEvent("save_cancelled");
            return 0;
        }
    }
    *size = bufSize;
    return buf;
}

static void saveToSlot(const char *name)
{
    int size = 0;
    void *data = saveState(&size);
    if (data)
    {
        SaveSlot *slot = findSave(name, 1);
        free(slot->Data);
        slot->Data = data;
        slot->Size = size;
    }
}

static void loadFromSlot(const char *name)
{
    SaveSlot *slot = findSave(name, 0);
    if (slot && slot->Data) afterCall(QSPOpenSavedGameFromData(slot->Data, slot->Size, QSP_FALSE));
}

/* The 5.9 API has no file functions, the runner writes and reads the files itself */
static void saveToFile(const char *fileName)
{
    int size = 0;
    void *data = saveState(&size);
    if (data)
    {
        FILE *f = fopen(fileName, "wb");
        if (!f || fwrite(data, 1, (size_t)size, f) != (size_t)size)
            fail("can't write %s", fileName);
        if (f) fclose(f);
        free(data);
    }
}

static void loadFromFile(const char *fileName)
{
    FILE *f = fopen(fileName, "rb");
    long size;
    void *data;
    if (!f)
    {
        fail("can't read %s", fileName);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = xmalloc(size > 0 ? (size_t)size : 1);
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size)
        fail("can't read %s", fileName);
    else
        afterCall(QSPOpenSavedGameFromData(data, (int)size, QSP_FALSE));
    fclose(f);
    free(data);
}

static void cbSaveGame(QSPString file)
{
    char *name = toUtf8(file);
    eventWithText("save_game", file, 1);
    saveToSlot(name);
    free(name);
}

static void cbOpenSave(QSPString file)
{
    char *name = toUtf8(file);
    eventWithText("load_save", file, 1);
    loadFromSlot(name);
    free(name);
}

static void setCallbacks(void)
{
    QSPSetCallback(QSP_CALL_DEBUG, (QSP_CALLBACK)cbDebug);
    QSPSetCallback(QSP_CALL_ISPLAYINGFILE, (QSP_CALLBACK)cbIsPlay);
    QSPSetCallback(QSP_CALL_PLAYFILE, (QSP_CALLBACK)cbPlayFile);
    QSPSetCallback(QSP_CALL_CLOSEFILE, (QSP_CALLBACK)cbCloseFile);
    QSPSetCallback(QSP_CALL_SHOWIMAGE, (QSP_CALLBACK)cbView);
    QSPSetCallback(QSP_CALL_SHOWWINDOW, (QSP_CALLBACK)cbShowWindow);
    QSPSetCallback(QSP_CALL_SHOWMENU, (QSP_CALLBACK)cbMenu);
    QSPSetCallback(QSP_CALL_SHOWMSGSTR, (QSP_CALLBACK)cbMsg);
    QSPSetCallback(QSP_CALL_REFRESHINT, (QSP_CALLBACK)cbRefresh);
    QSPSetCallback(QSP_CALL_SETTIMER, (QSP_CALLBACK)cbSetTimer);
    QSPSetCallback(QSP_CALL_SETINPUTSTRTEXT, (QSP_CALLBACK)cbSetInput);
    QSPSetCallback(QSP_CALL_SYSTEM, (QSP_CALLBACK)cbSystem);
    QSPSetCallback(QSP_CALL_OPENGAME, (QSP_CALLBACK)cbOpenGame);
    QSPSetCallback(QSP_CALL_OPENGAMESTATUS, (QSP_CALLBACK)cbOpenSave);
    QSPSetCallback(QSP_CALL_SAVEGAMESTATUS, (QSP_CALLBACK)cbSaveGame);
    QSPSetCallback(QSP_CALL_SLEEP, (QSP_CALLBACK)cbSleep);
    QSPSetCallback(QSP_CALL_GETMSCOUNT, (QSP_CALLBACK)cbGetMsCount);
    QSPSetCallback(QSP_CALL_INPUTBOX, (QSP_CALLBACK)cbInput);
    QSPSetCallback(QSP_CALL_VERSION, (QSP_CALLBACK)cbVersion);
}

/* ------------------------------------------------------------ */
/* Errors */

static const char *errorName(int num)
{
    static const char *names[] = {
        "INTERNAL", "DIVBYZERO", "TYPEMISMATCH", "LOCCALLDEPTH", "COMPLEXEXPRESSION",
        "CANTLOADFILE", "GAMENOTLOADED", "COLONNOTFOUND", "ENDNOTFOUND", "CANTINCFILE",
        "CANTADDACTION", "CANTADDOBJECT", "CANTADDMENUITEM", "LOCNOTFOUND", "LABELNOTFOUND",
        "INCORRECTNAME", "QUOTNOTFOUND", "BRACKETNOTFOUND", "SYNTAX", "UNKNOWNACTION",
        "ARGSCOUNT", "TOOMANYVARS", "INCORRECTREGEXP", "CODENOTFOUND", "LOOPWHILENOTFOUND",
        "HOSTFUNCFAILED", "HOSTFUNCNOTFOUND", "INCORRECTVERSION"
    };
    static char buf[32];
    if (num >= QSP_ERR_INTERNAL && num <= QSP_ERR_INCORRECTVERSION) return names[num - QSP_ERR_INTERNAL];
    snprintf(buf, sizeof(buf), "%d", num);
    return buf;
}

static void checkNoPendingError(void)
{
    if (T.PendingError)
    {
        fail("unexpected error %s", T.PendingError);
        free(T.PendingError);
        T.PendingError = 0;
    }
}

/* Records the result of an API call that executes code */
static void afterCall(QSP_BOOL isOk)
{
    if (!isOk)
    {
        QSPErrorInfo info = QSPGetLastErrorData();
        StrBuf sb = {0};
        sbAddf(&sb, "error %s", errorName(info.ErrorNum));
        addEvent(sbStr(&sb));
        free(T.PendingError);
        sbClear(&sb);
        /* NAME (location 'LOC', line TOP, local INT, action ACT) LINE */
        sbAddf(&sb, "%s (location '", errorName(info.ErrorNum));
        addEscaped(&sb, info.LocName);
        sbAddf(&sb, "', line %d, local %d, action %d) ", info.TopLineNum, info.IntLineNum, info.ActIndex);
        addEscaped(&sb, info.IntLine);
        T.PendingError = sbTake(&sb);
        free(T.LastErrorDesc);
        addEscaped(&sb, info.ErrorDesc);
        T.LastErrorDesc = sbTake(&sb);
    }
}

/* ------------------------------------------------------------ */
/* Loading games */

static void loadGame(Game *game, QSP_BOOL isNewGame)
{
    ByteBuf bb = buildGameFile(game);
    if (!QSPLoadGameWorldFromData(bb.Data, (int)bb.Len, isNewGame))
        afterCall(QSP_FALSE);
    free(bb.Data);
}

static void ensureGameLoaded(void)
{
    if (T.IsGameDirty || !T.IsGameLoaded)
    {
        if (!T.Main.LocsCount) gameLoc(&T.Main, "start");
        loadGame(&T.Main, QSP_TRUE);
        T.IsGameDirty = 0;
        T.IsGameLoaded = 1;
    }
}

/* ------------------------------------------------------------ */
/* Values: both sides of "@expect EXPR => VALUE" are formatted as QSP literals */

static void formatString(StrBuf *sb, QSPString s)
{
    const char *c;
    StrBuf text = {0};
    addEscaped(&text, s);
    sbAdd(sb, "'");
    for (c = sbStr(&text); *c; ++c)
    {
        if (*c == '\'') sbAdd(sb, "''");
        else sbAddN(sb, c, 1);
    }
    sbAdd(sb, "'");
    free(text.Data);
}

static void formatVariant(StrBuf *sb, const QSPVariant *v)
{
    int i;
    switch (QSP_BASETYPE(v->Type))
    {
    case QSP_TYPE_UNDEF:
        sbAdd(sb, "?");
        break;
    case QSP_TYPE_TUPLE:
        sbAdd(sb, "[");
        for (i = 0; i < v->Val.Tuple.ValsCount; ++i)
        {
            if (i) sbAdd(sb, ", ");
            formatVariant(sb, v->Val.Tuple.Vals + i);
        }
        sbAdd(sb, "]");
        break;
    case QSP_TYPE_NUM:
        sbAddf(sb, "%lld", (long long)v->Val.Num);
        break;
    case QSP_TYPE_STR:
        formatString(sb, v->Val.Str);
        break;
    }
}

/* Parses a literal: a number, a string in single quotes ('' is a quote), a tuple [a, b] or ? */
static int parseLiteral(const char **str, StrBuf *out)
{
    const char *s = *str;
    while (*s == ' ') ++s;
    if (*s == '\'')
    {
        StrBuf text = {0};
        char *unescaped;
        QSP_CHAR *qText;
        QSPString qStr;
        ++s;
        while (1)
        {
            if (!*s) return 0;
            if (*s == '\'')
            {
                if (s[1] != '\'') break;
                ++s;
            }
            sbAddN(&text, s++, 1);
        }
        ++s;
        /* Normalize the escapes */
        unescaped = unescape(sbStr(&text));
        qText = toQsp(unescaped);
        qStr = QSPStringFromC(qText);
        formatString(out, qStr);
        free(qText);
        free(unescaped);
        free(text.Data);
    }
    else if (*s == '[')
    {
        int isFirst = 1;
        ++s;
        sbAdd(out, "[");
        while (*s == ' ') ++s;
        if (*s != ']')
        {
            while (1)
            {
                if (!isFirst) sbAdd(out, ", ");
                isFirst = 0;
                if (!parseLiteral(&s, out)) return 0;
                while (*s == ' ') ++s;
                if (*s == ',')
                {
                    ++s;
                    continue;
                }
                if (*s == ']') break;
                return 0;
            }
        }
        ++s;
        sbAdd(out, "]");
    }
    else if (*s == '?')
    {
        ++s;
        sbAdd(out, "?");
    }
    else
    {
        char *end;
        long long num = strtoll(s, &end, 10);
        if (end == s) return 0;
        s = end;
        sbAddf(out, "%lld", num);
    }
    *str = s;
    return 1;
}

static int parseVariant(const char **str, QSPVariant *v)
{
    StrBuf canonical = {0};
    const char *s = *str, *pos;
    int isOk;
    memset(v, 0, sizeof(QSPVariant));
    while (*s == ' ') ++s;
    if (*s == '[')
    {
        ++s;
        v->Type = QSP_TYPE_TUPLE;
        while (*s == ' ') ++s;
        if (*s != ']')
        {
            while (1)
            {
                v->Val.Tuple.Vals = xrealloc(v->Val.Tuple.Vals, (v->Val.Tuple.ValsCount + 1) * sizeof(QSPVariant));
                if (!parseVariant(&s, v->Val.Tuple.Vals + v->Val.Tuple.ValsCount)) return 0;
                ++v->Val.Tuple.ValsCount;
                while (*s == ' ') ++s;
                if (*s == ',')
                {
                    ++s;
                    continue;
                }
                if (*s == ']') break;
                return 0;
            }
        }
        *str = s + 1;
        return 1;
    }
    /* Numbers, strings & ? go through the canonical form */
    pos = s;
    isOk = parseLiteral(&pos, &canonical);
    if (isOk)
    {
        const char *text = sbStr(&canonical);
        if (*text == '\'')
        {
            /* The canonical string: quotes are doubled, the escapes are kept */
            StrBuf raw = {0};
            char *unescaped;
            QSP_CHAR *qText;
            for (++text; text[1]; ++text)
            {
                if (*text == '\'') ++text;
                sbAddN(&raw, text, 1);
            }
            unescaped = unescape(sbStr(&raw));
            qText = toQsp(unescaped);
            v->Type = QSP_TYPE_STR;
            v->Val.Str = QSPStringFromC(qText);
            free(unescaped);
            free(raw.Data);
        }
        else if (*text != '?')
        {
            v->Type = QSP_TYPE_NUM;
            v->Val.Num = (QSP_BIGINT)strtoll(text, 0, 10);
        }
        *str = pos;
    }
    free(canonical.Data);
    return isOk;
}

static void freeVariant(QSPVariant *v)
{
    int i;
    switch (v->Type)
    {
    case QSP_TYPE_STR:
        free(v->Val.Str.Str);
        break;
    case QSP_TYPE_TUPLE:
        for (i = 0; i < v->Val.Tuple.ValsCount; ++i) freeVariant(v->Val.Tuple.Vals + i);
        free(v->Val.Tuple.Vals);
        break;
    }
    memset(v, 0, sizeof(QSPVariant));
}

static void execSilently(const char *code)
{
    QSP_CHAR *qCode = toQsp(code);
    QSPExecString(QSPStringFromC(qCode), QSP_FALSE);
    free(qCode);
}

/* Evaluates the expression through a temporary variable to keep the type of the value */
static int evalExpr(const char *expr, char **res)
{
    StrBuf sb = {0};
    QSPVariant value, key;
    QSP_CHAR *qName;
    int isOk;
    sbAddf(&sb, "%s = (%s)", RESULT_VAR, expr);
    {
        QSP_CHAR *qCode = toQsp(sbStr(&sb));
        isOk = QSPExecString(QSPStringFromC(qCode), QSP_FALSE);
        free(qCode);
    }
    sbClear(&sb);
    if (!isOk)
    {
        QSPErrorInfo info = QSPGetLastErrorData();
        free(sb.Data);
        *res = xstrdup(errorName(info.ErrorNum));
        return 0;
    }
    qName = toQsp(RESULT_VAR);
    QSPGetVarValue(QSPStringFromC(qName), 0, &value, &key);
    formatVariant(&sb, &value);
    free(qName);
    *res = sbTake(&sb);
    execSilently("killvar '" RESULT_VAR "'");
    return 1;
}

/* ------------------------------------------------------------ */
/* Host functions: every call becomes the event "host NAME [ARGS]", the replies script the calls */

static const char *skipSpaces(const char *s);
static const char *command(const char *line, const char *name);
static void freeStrings(char ***items, int *count);

static int isDbFunc(const char *name)
{
    return !strcmp(name, "qsp.db.get") || !strcmp(name, "qsp.db.set") || !strcmp(name, "qsp.db.del");
}

static void formatArgs(StrBuf *sb, QSPVariant *args, int count)
{
    int i;
    sbAdd(sb, "[");
    for (i = 0; i < count; ++i)
    {
        if (i) sbAdd(sb, ", ");
        formatVariant(sb, args + i);
    }
    sbAdd(sb, "]");
}

static int runEvent(const char *spec, QSP_BOOL toRefreshUI, QSP_BOOL *isOk)
{
    char loc[MAX_LINE];
    QSPVariant args[32]; /* more than the engine takes */
    int i, count = 0, isParsed = 1;
    QSP_CHAR *qLoc;
    const char *s = skipSpaces(spec);
    if (sscanf(s, "%4095s", loc) < 1)
    {
        fail("expected 'LOCATION [ARGS]'");
        return 0;
    }
    s = skipSpaces(s + strlen(loc));
    while (*s)
    {
        if (count == (int)(sizeof(args) / sizeof(args[0])) || !parseVariant(&s, args + count))
        {
            if (count < (int)(sizeof(args) / sizeof(args[0]))) freeVariant(args + count);
            isParsed = 0;
            break;
        }
        ++count;
        s = skipSpaces(s);
        if (*s == ',')
            s = skipSpaces(s + 1);
        else if (*s)
        {
            isParsed = 0;
            break;
        }
    }
    if (isParsed)
    {
        qLoc = toQsp(loc);
        *isOk = QSPExecLocationCodeWithArgs(QSPStringFromC(qLoc), (count ? args : 0), count, toRefreshUI);
        free(qLoc);
    }
    else
        fail("incorrect arguments of the event '%s'", spec);
    for (i = 0; i < count; ++i) freeVariant(args + i);
    return isParsed;
}

static void setResult(QSPVariant *result, QSPVariant *value)
{
    switch (value->Type)
    {
    case QSP_TYPE_NUM:
        QSPSetResultNum(result, value->Val.Num);
        break;
    case QSP_TYPE_STR:
        QSPSetResultStr(result, value->Val.Str);
        break;
    case QSP_TYPE_TUPLE:
        QSPSetResultTuple(result, value->Val.Tuple.Vals, value->Val.Tuple.ValsCount);
        break;
    }
}

/* qsp.db: the formatted key of a value */
static int findDbKey(QSPVariant *key, int toCreate)
{
    StrBuf sb = {0};
    int i;
    formatVariant(&sb, key);
    for (i = 0; i < T.DbCount; ++i)
    {
        if (!strcmp(T.DbKeys[i], sbStr(&sb)))
        {
            free(sb.Data);
            return i;
        }
    }
    if (!toCreate)
    {
        free(sb.Data);
        return -1;
    }
    T.DbKeys = xrealloc(T.DbKeys, (T.DbCount + 1) * sizeof(char *));
    T.DbValues = xrealloc(T.DbValues, (T.DbCount + 1) * sizeof(char *));
    T.DbKeys[T.DbCount] = sbTake(&sb);
    T.DbValues[T.DbCount] = xstrdup("?");
    return T.DbCount++;
}

static void callDb(HostFunc *func, QSPVariant *args, QSPVariant *result)
{
    int index;
    if (!strcmp(func->Name, "qsp.db.get"))
    {
        index = findDbKey(args, 0);
        if (index >= 0)
        {
            QSPVariant value;
            const char *text = T.DbValues[index];
            if (parseVariant(&text, &value))
            {
                setResult(result, &value);
                freeVariant(&value);
            }
        }
    }
    else if (!strcmp(func->Name, "qsp.db.set"))
    {
        StrBuf sb = {0};
        index = findDbKey(args, 1);
        formatVariant(&sb, args + 1);
        free(T.DbValues[index]);
        T.DbValues[index] = sbTake(&sb);
    }
    else if ((index = findDbKey(args, 0)) >= 0)
    {
        /* qsp.db.del */
        free(T.DbKeys[index]);
        free(T.DbValues[index]);
        --T.DbCount;
        T.DbKeys[index] = T.DbKeys[T.DbCount];
        T.DbValues[index] = T.DbValues[T.DbCount];
    }
}

static QSP_BOOL callHost(int funcIndex, QSPVariant *args, int count, QSPVariant *result)
{
    HostFunc *func = T.HostFuncs + funcIndex;
    StrBuf sb = {0};
    QSP_BOOL isOk = QSP_TRUE;
    sbAddf(&sb, "host %s ", func->Name);
    formatArgs(&sb, args, count);
    addEvent(sbStr(&sb));
    sbClear(&sb);
    if (func->IsDb)
        callDb(func, args, result);
    if (func->RepliesCount)
    {
        const char *reply = func->Replies[0], *value;
        if ((value = command(reply, "=>")))
        {
            QSPVariant v;
            if (!parseVariant(&value, &v) || *skipSpaces(value))
                fail("incorrect reply '%s'", reply);
            else if (result)
                setResult(result, &v);
            freeVariant(&v);
        }
        else if (command(reply, "fail"))
            isOk = QSP_FALSE;
        else if ((value = command(reply, "exec")))
        {
            /* The host functions can't execute code */
            QSP_CHAR *qCode = toQsp(value);
            sbAddf(&sb, "host-exec %d", QSPExecString(QSPStringFromC(qCode), QSP_FALSE) ? 1 : 0);
            addEvent(sbStr(&sb));
            free(qCode);
        }
        else if ((value = command(reply, "read")))
        {
            /* Reading the state is allowed */
            sbAddf(&sb, "read %s => ", value);
            readVariable(&sb, value);
            addEvent(sbStr(&sb));
        }
        else if ((value = command(reply, "event")))
        {
            /* The incoming events are refused inside the call */
            QSP_BOOL isEventOk;
            if (runEvent(value, QSP_FALSE, &isEventOk))
                addEvent(isEventOk ? "host-event 1" : "host-event 0");
        }
        else if ((value = command(reply, "queue")))
        {
            /* The event runs later, @run-queued */
            T.QueuedEvents = xrealloc(T.QueuedEvents, (T.QueuedEventsCount + 1) * sizeof(char *));
            T.QueuedEvents[T.QueuedEventsCount++] = xstrdup(value);
        }
        else if (!command(reply, "ok"))
            fail("unknown reply '%s'", reply);
        shiftStrings(func->Replies, &func->RepliesCount);
    }
    free(sb.Data);
    return isOk;
}

static QSP_BOOL cbHostFunc(QSPVariant *args, int count, QSPVariant *result, void *userData)
{
    return callHost((int)(intptr_t)userData, args, count, result);
}

static QSP_BOOL cbHostStat(QSPVariant *args, int count, void *userData)
{
    return callHost((int)(intptr_t)userData, args, count, 0);
}

static int parseResType(const char *s)
{
    if (!*s || !strcmp(s, "?")) return QSP_TYPE_UNDEF;
    if (!strcmp(s, "#")) return QSP_TYPE_NUM;
    if (!strcmp(s, "$")) return QSP_TYPE_STR;
    if (!strcmp(s, "%")) return QSP_TYPE_TUPLE;
    return atoi(s); /* any other type by its number */
}

/* @host-func [!] NAME MIN MAX [TYPE], @host-stat [!] NAME MIN MAX, "!" expects a refusal */
static void registerHostFunc(const char *arg, int isStatement)
{
    char name[MAX_LINE], type[16] = "";
    int minArgs, maxArgs, isRefusal = 0, isOk;
    QSP_CHAR *qName;
    HostFunc *func;
    arg = skipSpaces(arg);
    if (*arg == '!')
    {
        isRefusal = 1;
        arg = skipSpaces(arg + 1);
    }
    if (sscanf(arg, "%4095s %d %d %15s", name, &minArgs, &maxArgs, type) < 3)
    {
        fail("expected 'NAME MIN MAX [TYPE]'");
        return;
    }
    T.HostFuncs = xrealloc(T.HostFuncs, (T.HostFuncsCount + 1) * sizeof(HostFunc));
    func = T.HostFuncs + T.HostFuncsCount;
    memset(func, 0, sizeof(HostFunc));
    func->Name = xstrdup(name);
    func->IsStatement = isStatement;
    func->IsDb = isDbFunc(name);
    qName = toQsp(name);
    if (isStatement)
        isOk = QSPRegisterStatement(QSPStringFromC(qName), minArgs, maxArgs, cbHostStat, (void *)(intptr_t)T.HostFuncsCount);
    else
        isOk = QSPRegisterFunction(QSPStringFromC(qName), minArgs, maxArgs, (QSP_TINYINT)parseResType(type), cbHostFunc, (void *)(intptr_t)T.HostFuncsCount);
    free(qName);
    ++T.HostFuncsCount;
    if (isOk == isRefusal)
        fail("registration of %s: expected %s", name, isRefusal ? "a refusal" : "a success");
}

/* @host-profile [!] NAME VERSION */
static void registerHostProfile(const char *arg)
{
    char name[MAX_LINE], version[MAX_LINE] = "";
    int isRefusal = 0, isOk;
    QSP_CHAR *qName, *qVersion;
    arg = skipSpaces(arg);
    if (*arg == '!')
    {
        isRefusal = 1;
        arg = skipSpaces(arg + 1);
    }
    if (sscanf(arg, "%4095s %4095s", name, version) < 1)
    {
        fail("expected 'NAME VERSION'");
        return;
    }
    qName = toQsp(name);
    qVersion = toQsp(version);
    isOk = QSPRegisterProfile(QSPStringFromC(qName), QSPStringFromC(qVersion));
    free(qName);
    free(qVersion);
    if (isOk == isRefusal)
        fail("registration of profile %s: expected %s", name, isRefusal ? "a refusal" : "a success");
}

/* @host-reply NAME ACTION */
static void addHostReply(const char *arg)
{
    char name[MAX_LINE];
    int i;
    if (sscanf(arg, "%4095s", name) < 1)
    {
        fail("expected 'NAME ACTION'");
        return;
    }
    for (i = 0; i < T.HostFuncsCount; ++i)
    {
        if (!strcmp(T.HostFuncs[i].Name, name))
        {
            HostFunc *func = T.HostFuncs + i;
            func->Replies = xrealloc(func->Replies, (func->RepliesCount + 1) * sizeof(char *));
            func->Replies[func->RepliesCount++] = xstrdup(skipSpaces(skipSpaces(arg) + strlen(name)));
            return;
        }
    }
    fail("unknown host function '%s'", name);
}

static void freeHostFuncs(void)
{
    int i;
    for (i = 0; i < T.HostFuncsCount; ++i)
    {
        free(T.HostFuncs[i].Name);
        freeStrings(&T.HostFuncs[i].Replies, &T.HostFuncs[i].RepliesCount);
    }
    free(T.HostFuncs);
    T.HostFuncs = 0;
    T.HostFuncsCount = 0;
}

/* ------------------------------------------------------------ */
/* Test lifecycle */

static void freeStrings(char ***items, int *count)
{
    int i;
    for (i = 0; i < *count; ++i) free((*items)[i]);
    free(*items);
    *items = 0;
    *count = 0;
}

static void resetEngine(void)
{
    int i;
    QSPTerminate();
    QSPInit();
    setCallbacks();
    freeGame(&T.Main);
    for (i = 0; i < T.ModulesCount; ++i) freeGame(T.Modules + i);
    free(T.Modules);
    T.Modules = 0;
    T.ModulesCount = 0;
    T.IsGameDirty = 0;
    T.IsGameLoaded = 0;
    clearEvents();
    free(T.PendingError);
    T.PendingError = 0;
    free(T.LastErrorDesc);
    T.LastErrorDesc = 0;
    freeStrings(&T.InputReplies, &T.InputRepliesCount);
    freeStrings(&T.VersionReplies, &T.VersionRepliesCount);
    freeStrings(&T.MsgWatches, &T.MsgWatchesCount);
    freeStrings(&T.MsgEvents, &T.MsgEventsCount);
    freeStrings(&T.QueuedEvents, &T.QueuedEventsCount);
    freeHostFuncs();
    for (i = 0; i < T.DbCount; ++i) free(T.DbValues[i]);
    free(T.DbValues);
    T.DbValues = 0;
    freeStrings(&T.DbKeys, &T.DbCount);
    T.MenuRepliesCount = 0;
    T.IsPlayRepliesCount = 0;
    T.MsCount = 0;
    for (i = 0; i < T.SavesCount; ++i)
    {
        free(T.Saves[i].Name);
        free(T.Saves[i].Data);
    }
    free(T.Saves);
    T.Saves = 0;
    T.SavesCount = 0;
}

static void finishTest(void)
{
    if (!T.TestName) return;
    if (!T.IsSkipped) checkNoPendingError();
    if (T.IsSkipped)
        ++T.Skipped;
    else if (T.IsFailed)
    {
        dumpEvents();
        ++T.Failed;
    }
    else
        ++T.Passed;
    free(T.TestName);
    T.TestName = 0;
}

static void startTest(const char *name)
{
    finishTest();
    resetEngine();
    T.TestName = xstrdup(name);
    T.IsSkipped = 0;
    T.IsFailed = 0;
}

/* ------------------------------------------------------------ */
/* Commands */

static const char *skipSpaces(const char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

/* Trims spaces in place */
static char *trim(char *s)
{
    size_t len;
    while (*s == ' ' || *s == '\t') ++s;
    len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = 0;
    return s;
}

static int startsWith(const char *s, const char *prefix)
{
    return !strncmp(s, prefix, strlen(prefix));
}

/* Returns the argument after the command word, or NULL if the line has another command */
static const char *command(const char *line, const char *name)
{
    size_t len = strlen(name);
    if (strncmp(line, name, len)) return 0;
    if (line[len] && line[len] != ' ' && line[len] != '\t') return 0;
    return skipSpaces(line + len);
}

static void expectText(const char *what, QSPString actual, const char *expected)
{
    StrBuf sb = {0};
    addEscaped(&sb, actual);
    if (strcmp(sbStr(&sb), expected))
        fail("%s: expected '%s', got '%s'", what, expected, sbStr(&sb));
    free(sb.Data);
}

static void expectList(const char *what, int isActions, const char *expected)
{
    StrBuf sb = {0};
    listState(&sb, isActions);
    if (strcmp(sbStr(&sb), expected))
        fail("%s: expected %s, got %s", what, expected, sbStr(&sb));
    free(sb.Data);
}

static void expectValue(const char *arg)
{
    const char *sep = strstr(arg, " => "), *literal;
    char *expr, *actual;
    StrBuf expected = {0};
    if (!sep)
    {
        fail("expected 'EXPR => VALUE'");
        return;
    }
    literal = sep + 4;
    if (!parseLiteral(&literal, &expected) || *skipSpaces(literal))
    {
        fail("incorrect value '%s'", sep + 4);
        free(expected.Data);
        return;
    }
    expr = xmalloc(sep - arg + 1);
    memcpy(expr, arg, sep - arg);
    expr[sep - arg] = 0;
    if (!evalExpr(expr, &actual))
        fail("%s: evaluation failed with %s", expr, actual);
    else if (strcmp(actual, sbStr(&expected)))
        fail("%s: expected %s, got %s", expr, sbStr(&expected), actual);
    free(actual);
    free(expected.Data);
    free(expr);
}

static int countEvents(const char *prefix, int isExact)
{
    int i, count = 0;
    for (i = 0; i < T.EventsCount; ++i)
    {
        if (isExact ? !strcmp(T.Events[i], prefix) : startsWith(T.Events[i], prefix))
            ++count;
    }
    return count;
}

static void expectEvents(char **lines, int count)
{
    int i;
    if (count != T.EventsCount)
    {
        fail("expected %d events, got %d", count, T.EventsCount);
        fprintf(stderr, "  expected:\n");
        for (i = 0; i < count; ++i) fprintf(stderr, "    %s\n", lines[i]);
        return;
    }
    for (i = 0; i < count; ++i)
    {
        if (strcmp(lines[i], T.Events[i]))
        {
            fail("event #%d: expected '%s', got '%s'", i + 1, lines[i], T.Events[i]);
            return;
        }
    }
}

static void expectError(const char *value)
{
    size_t len = strlen(value);
    if (!T.PendingError)
        fail("expected error %s, got no error", value);
    else if (!startsWith(T.PendingError, value) || (T.PendingError[len] && T.PendingError[len] != ' '))
        fail("expected error %s, got %s", value, T.PendingError);
    free(T.PendingError);
    T.PendingError = 0;
}

static void addReply(char ***replies, int *count, const char *value)
{
    *replies = xrealloc(*replies, (*count + 1) * sizeof(char *));
    (*replies)[(*count)++] = unescape(value);
}

/* Runs an API call that executes code */
static void runCall(const char *name, const char *arg)
{
    QSP_CHAR *qStr = toQsp(*arg || strcmp(name, "@run") ? arg : "test");
    QSPString str = QSPStringFromC(qStr);
    checkNoPendingError();
    ensureGameLoaded();
    if (!strcmp(name, "@run"))
        afterCall(QSPExecLocationCode(str, QSP_TRUE));
    else if (!strcmp(name, "@exec"))
        afterCall(QSPExecString(str, QSP_TRUE));
    else if (!strcmp(name, "@counter"))
        afterCall(QSPExecCounter(QSP_TRUE));
    else if (!strcmp(name, "@user-input"))
        afterCall(QSPExecUserInput(QSP_TRUE));
    else if (!strcmp(name, "@select-act"))
        afterCall(QSPSetSelActionIndex(atoi(arg), QSP_TRUE));
    else if (!strcmp(name, "@exec-act"))
        afterCall(QSPExecuteSelActionCode(QSP_TRUE));
    else if (!strcmp(name, "@select-obj"))
        afterCall(QSPSetSelObjectIndex(atoi(arg), QSP_TRUE));
    else if (!strcmp(name, "@restart"))
        afterCall(QSPRestartGame(QSP_TRUE));
    free(qStr);
}

/* Executes a command, block lines are passed for the commands that take a block */
static void runCommand(const char *line, char **block, int blockCount)
{
    static const char *calls[] = {
        "@run", "@exec", "@counter", "@user-input", "@select-act", "@exec-act", "@select-obj", "@restart"
    };
    const char *arg;
    size_t i;
    if (T.IsSkipped || T.IsFailed) return;

    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i)
    {
        if ((arg = command(line, calls[i])))
        {
            runCall(calls[i], arg);
            return;
        }
    }

    /* Game definition */
    if ((arg = command(line, "@src")))
    {
        /* The same wrapper as runTestFile() in qsp-wasm-engine */
        char **lines = xmalloc((blockCount + 5) * sizeof(char *));
        int j, n = 0;
        lines[n++] = "# start";
        lines[n++] = "---";
        lines[n++] = "# test";
        for (j = 0; j < blockCount; ++j) lines[n++] = block[j];
        lines[n++] = "---";
        parseQsps(&T.Main, lines, n);
        free(lines);
        T.IsGameDirty = 1;
    }
    else if ((arg = command(line, "@game")))
    {
        parseQsps(&T.Main, block, blockCount);
        T.IsGameDirty = 1;
    }
    else if ((arg = command(line, "@module")))
    {
        Game *mod;
        T.Modules = xrealloc(T.Modules, (T.ModulesCount + 1) * sizeof(Game));
        mod = T.Modules + T.ModulesCount++;
        memset(mod, 0, sizeof(Game));
        mod->Name = xstrdup(arg);
        parseQsps(mod, block, blockCount);
    }
    else if ((arg = command(line, "@desc")))
    {
        Location *loc = gameLoc(&T.Main, arg);
        int j;
        sbClear(&loc->Desc);
        for (j = 0; j < blockCount; ++j) addTextLine(&loc->Desc, block[j]);
        T.IsGameDirty = 1;
    }
    else if ((arg = command(line, "@action")))
    {
        /* @action LOCATION | NAME [| IMAGE] */
        char *copy = xstrdup(arg), *name, *image;
        Location *loc;
        Action *act;
        int j;
        name = strchr(copy, '|');
        if (!name)
        {
            fail("expected '@action LOCATION | NAME [| IMAGE]'");
            free(copy);
            return;
        }
        *name++ = 0;
        image = strchr(name, '|');
        if (image) *image++ = 0;
        loc = gameLoc(&T.Main, trim(copy));
        loc->Actions = xrealloc(loc->Actions, (loc->ActionsCount + 1) * sizeof(Action));
        act = loc->Actions + loc->ActionsCount++;
        memset(act, 0, sizeof(Action));
        act->Name = xstrdup(trim(name));
        act->Image = image ? xstrdup(trim(image)) : 0;
        for (j = 0; j < blockCount; ++j) addTextLine(&act->Code, block[j]);
        free(copy);
        T.IsGameDirty = 1;
    }
    /* Replies of the "user" */
    else if ((arg = command(line, "@reply")))
    {
        const char *value;
        if ((value = command(arg, "input")))
            addReply(&T.InputReplies, &T.InputRepliesCount, value);
        else if ((value = command(arg, "version")))
            addReply(&T.VersionReplies, &T.VersionRepliesCount, value);
        else if ((value = command(arg, "menu")))
        {
            if (T.MenuRepliesCount == (int)(sizeof(T.MenuReplies) / sizeof(T.MenuReplies[0])))
                fail("too many menu replies");
            else
                T.MenuReplies[T.MenuRepliesCount++] = atoi(value);
        }
        else if ((value = command(arg, "isplay")))
        {
            if (T.IsPlayRepliesCount == (int)(sizeof(T.IsPlayReplies) / sizeof(T.IsPlayReplies[0])))
                fail("too many isplay replies");
            else
                T.IsPlayReplies[T.IsPlayRepliesCount++] = atoi(value);
        }
        else if ((value = command(arg, "mscount")))
            T.MsCount = atoi(value);
        else
            fail("unknown reply '%s'", arg);
    }
    /* Other actions */
    else if ((arg = command(line, "@set-input")))
    {
        char *text = unescape(arg);
        QSP_CHAR *qStr = toQsp(text);
        QSPSetInputStrText(QSPStringFromC(qStr));
        free(qStr);
        free(text);
    }
    else if ((arg = command(line, "@save")))
    {
        ensureGameLoaded();
        saveToSlot(arg);
    }
    else if ((arg = command(line, "@load-save")))
    {
        checkNoPendingError();
        loadFromSlot(arg);
    }
    else if ((arg = command(line, "@save-file")))
    {
        ensureGameLoaded();
        saveToFile(arg);
    }
    else if ((arg = command(line, "@load-file")))
    {
        checkNoPendingError();
        loadFromFile(arg);
    }
    else if ((arg = command(line, "@host-func")))
        registerHostFunc(arg, 0);
    else if ((arg = command(line, "@host-stat")))
        registerHostFunc(arg, 1);
    else if ((arg = command(line, "@host-profile")))
        registerHostProfile(arg);
    else if ((arg = command(line, "@host-reply")))
        addHostReply(arg);
    else if ((arg = command(line, "@host-db")))
    {
        /* The standard profile qsp.db over the store of the runner */
        registerHostFunc("qsp.db.get 1 1", 0);
        registerHostFunc("qsp.db.set 2 2", 1);
        registerHostFunc("qsp.db.del 1 1", 1);
        registerHostProfile("qsp.db 1.0.0");
    }
    else if ((arg = command(line, "@run-args")) || (arg = command(line, "@run-queued")))
    {
        /* Incoming events: a failure without an error of the engine is a refusal */
        QSP_BOOL isOk;
        char **events;
        int j, count = 0;
        if (command(line, "@run-queued"))
        {
            /* Stops at the first error, the rest is dropped */
            events = T.QueuedEvents;
            count = T.QueuedEventsCount;
            T.QueuedEvents = 0;
            T.QueuedEventsCount = 0;
        }
        else
        {
            events = xmalloc(sizeof(char *));
            events[count++] = xstrdup(arg);
        }
        checkNoPendingError();
        for (j = 0; j < count && !T.IsFailed && !T.PendingError; ++j)
        {
            ensureGameLoaded();
            execSilently(""); /* resets the last error */
            if (!runEvent(events[j], QSP_TRUE, &isOk)) break;
            if (!isOk && !QSPGetLastErrorData().ErrorNum)
                addEvent("event_refused");
            else
                afterCall(isOk);
        }
        freeStrings(&events, &count);
    }
    else if ((arg = command(line, "@msg-event")))
    {
        T.MsgEvents = xrealloc(T.MsgEvents, (T.MsgEventsCount + 1) * sizeof(char *));
        T.MsgEvents[T.MsgEventsCount++] = xstrdup(arg);
    }
    else if ((arg = command(line, "@reinit")))
    {
        QSPTerminate();
        QSPInit();
        setCallbacks();
        freeHostFuncs();
        T.IsGameLoaded = 0;
    }
    else if ((arg = command(line, "@watch-msg")))
    {
        T.MsgWatches = xrealloc(T.MsgWatches, (T.MsgWatchesCount + 1) * sizeof(char *));
        T.MsgWatches[T.MsgWatchesCount++] = xstrdup(arg);
    }
    else if ((arg = command(line, "@debug")))
        QSPEnableDebugMode(atoi(arg) != 0);
    else if ((arg = command(line, "@clear-events")))
        clearEvents();
    /* Expectations */
    else if ((arg = command(line, "@expect")))
    {
        const char *value;
        if ((value = command(arg, "main")))
            expectText("main", QSPGetMainDesc(), value);
        else if ((value = command(arg, "stat")))
            expectText("stat", QSPGetVarsDesc(), value);
        else if ((value = command(arg, "acts")))
            expectList("actions", 1, value);
        else if ((value = command(arg, "objs")))
            expectList("objects", 0, value);
        else if ((value = command(arg, "selact")))
        {
            if (QSPGetSelActionIndex() != atoi(value))
                fail("selected action: expected %d, got %d", atoi(value), QSPGetSelActionIndex());
        }
        else if ((value = command(arg, "selobj")))
        {
            if (QSPGetSelObjectIndex() != atoi(value))
                fail("selected object: expected %d, got %d", atoi(value), QSPGetSelObjectIndex());
        }
        else if ((value = command(arg, "curloc")))
        {
            char *actual;
            if (!evalExpr("$CURLOC", &actual))
                fail("current location: evaluation failed with %s", actual);
            else
            {
                StrBuf expected = {0};
                QSP_CHAR *qValue = toQsp(value);
                formatString(&expected, QSPStringFromC(qValue));
                if (strcmp(actual, sbStr(&expected)))
                    fail("current location: expected %s, got %s", sbStr(&expected), actual);
                free(qValue);
                free(expected.Data);
            }
            free(actual);
        }
        else if ((value = command(arg, "error")))
            expectError(value);
        else if ((value = command(arg, "same-saves")))
        {
            char first[MAX_LINE], second[MAX_LINE];
            SaveSlot *slot1, *slot2;
            if (sscanf(value, "%4095s %4095s", first, second) < 2)
                fail("expected 'same-saves SLOT1 SLOT2'");
            else if (!(slot1 = findSave(first, 0)) || !(slot2 = findSave(second, 0)))
                fail("unknown save slot");
            else if (slot1->Size != slot2->Size || memcmp(slot1->Data, slot2->Data, (size_t)slot1->Size))
                fail("saves %s and %s differ", first, second);
        }
        else if ((value = command(arg, "error-desc")))
        {
            if (!T.LastErrorDesc || strcmp(T.LastErrorDesc, value))
                fail("error description: expected '%s', got '%s'", value, T.LastErrorDesc ? T.LastErrorDesc : "");
        }
        else if ((value = command(arg, "event")))
        {
            if (!countEvents(value, 1)) fail("expected event '%s'", value);
        }
        else if ((value = command(arg, "no-event")))
        {
            if (countEvents(value, 0)) fail("unexpected event '%s...'", value);
        }
        else if ((value = command(arg, "count")))
        {
            /* @expect count N PREFIX */
            char *end;
            long n = strtol(value, &end, 10);
            const char *prefix = skipSpaces(end);
            int actual = countEvents(prefix, 0);
            if (actual != n) fail("expected %ld events '%s...', got %d", n, prefix, actual);
        }
        else if ((value = command(arg, "events")))
            expectEvents(block, blockCount);
        else
            expectValue(arg);
    }
    else if ((arg = command(line, "@skip")))
        T.IsSkipped = 1;
    else
        fail("unknown command '%s'", line);
}

/* Commands that take the following lines (till the next @-line) as a block */
static int isBlockCommand(const char *line)
{
    const char *arg = command(line, "@expect");
    return command(line, "@src") || command(line, "@game") || command(line, "@module") ||
        command(line, "@desc") || command(line, "@action") ||
        (arg && command(arg, "events"));
}

/* ------------------------------------------------------------ */
/* Reading scenario files */

static char **readLines(const char *fileName, int *count)
{
    FILE *f = fopen(fileName, "rb");
    char **lines = 0, buf[MAX_LINE];
    int n = 0;
    if (!f) return 0;
    while (fgets(buf, sizeof(buf), f))
    {
        size_t len = strlen(buf);
        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
        lines = xrealloc(lines, (n + 1) * sizeof(char *));
        lines[n++] = xstrdup(buf);
    }
    fclose(f);
    *count = n;
    return lines;
}

static int runFile(const char *fileName)
{
    int count, i, j;
    char **lines = readLines(fileName, &count);
    if (!lines)
    {
        fprintf(stderr, "can't read %s\n", fileName);
        return 0;
    }
    T.FileName = fileName;
    for (i = 0; i < count; ++i)
    {
        const char *line = lines[i], *arg;
        T.Line = i + 1;
        if (!line[0] || (line[0] == '/' && line[1] == '/')) continue;
        if (line[0] != '@')
        {
            fprintf(stderr, "%s:%d: unexpected line outside of a block: %s\n", fileName, i + 1, line);
            return 0;
        }
        if ((arg = command(line, "@test")))
        {
            startTest(arg);
            continue;
        }
        if (!T.TestName)
        {
            fprintf(stderr, "%s:%d: command before @test\n", fileName, i + 1);
            return 0;
        }
        if (isBlockCommand(line))
        {
            /* The block lasts till the next @-line; "@@" at the start of a line stands for "@" */
            int start = i + 1, n;
            char **block;
            j = start;
            while (j < count && !(lines[j][0] == '@' && lines[j][1] != '@')) ++j;
            n = j - start;
            block = xmalloc((n + 1) * sizeof(char *));
            for (n = 0; start + n < j; ++n)
                block[n] = lines[start + n][0] == '@' ? lines[start + n] + 1 : lines[start + n];
            /* Trailing empty lines and // comments belong to the formatting of the file, not to the block */
            while (n > 0 && (!block[n - 1][0] || (block[n - 1][0] == '/' && block[n - 1][1] == '/'))) --n;
            runCommand(line, block, n);
            free(block);
            i = j - 1;
        }
        else
            runCommand(line, 0, 0);
    }
    finishTest();
    for (i = 0; i < count; ++i) free(lines[i]);
    free(lines);
    return 1;
}

int main(int argc, char **argv)
{
    int i, isOk = 1;
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s FILE.qspt...\n", argv[0]);
        return 2;
    }
    QSPInit();
    for (i = 1; i < argc; ++i)
        if (!runFile(argv[i])) isOk = 0;
    resetEngine();
    QSPTerminate();
    printf("%d passed, %d failed, %d skipped\n", T.Passed, T.Failed, T.Skipped);
    return (isOk && !T.Failed) ? 0 : 1;
}
