/*
 * tg_backend_tdlib.c -- реализация tg_backend.h поверх TDLib (JSON-интерфейс).
 *
 * Этот файл -- изолированный модуль работы с внешним сервисом. Наружу он
 * отдаёт только то, что объявлено в tg_backend.h; ни FUSE, ни cJSON, ни
 * структуры TDLib за его пределы не проходят.
 *
 * Схема работы
 * ------------
 *  - td_receive() крутится в отдельном потоке (recv_thread). Больше никто
 *    td_receive() не вызывает: возвращённая строка живёт только до
 *    следующего вызова в том же потоке.
 *  - Каждый запрос получает уникальный "@extra". Поток-отправитель заводит
 *    структуру waiter, кладёт её в список и спит на pthread_cond_t, пока
 *    recv_thread не положит туда ответ с тем же "@extra". Поэтому снаружи
 *    API синхронный, хотя TDLib асинхронный.
 *  - Апдейты ("@extra" нет) обновляют внутренние таблицы: чаты,
 *    пользователи, basic-группы, супергруппы. Таблицы -- uthash.
 *  - Отправка сообщения подтверждается не ответом на sendMessage (он
 *    приходит сразу, в состоянии pending), а апдейтом
 *    updateMessageSendSucceeded / updateMessageSendFailed. Результаты этих
 *    апдейтов складываются в кольцевой буфер, чтобы отправитель не потерял
 *    апдейт, пришедший раньше, чем он успел прочитать ответ.
 *  - Участники группы: getBasicGroupFullInfo для обычных групп,
 *    getSupergroupMembers (постранично) для супергрупп. Личный диалог с
 *    участником заводится createPrivateChat.
 */
#define _DEFAULT_SOURCE  /* getpass(), usleep() */

#include "tg_backend.h"

#include <td/telegram/td_json_client.h>

#include "cJSON.h"
#include "uthash.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define REQ_TIMEOUT_SEC   30    /* ожидание ответа на обычный запрос        */
#define SEND_TIMEOUT_SEC  60    /* ожидание подтверждения отправки          */
#define LOGIN_TIMEOUT_SEC 120   /* ожидание смены состояния авторизации     */
#define HISTORY_BATCH     100   /* максимум за один getChatHistory          */
#define MEMBERS_BATCH     200   /* максимум за один getSupergroupMembers    */
#define SEND_RING         64    /* размер кольца результатов отправки       */

/* ------------------------------------------------------------------ */
/* внутренние таблицы                                                  */
/* ------------------------------------------------------------------ */

/* Статус нас самих (или участника) в чате. */
typedef enum {
    ST_UNKNOWN = 0,
    ST_CREATOR,
    ST_ADMIN,
    ST_MEMBER,
    ST_RESTRICTED,
    ST_LEFT,
    ST_BANNED
} member_status;

typedef enum { CK_PRIVATE = 0, CK_BASIC, CK_SUPER, CK_SECRET } chat_kind;

typedef struct {
    int64_t        user_id;
    char          *name;        /* "Имя Фамилия" */
    char          *username;    /* без '@', может быть NULL */
    int            is_bot;
    int            is_deleted;
    UT_hash_handle hh;
} user_rec;

typedef struct {
    int64_t        group_id;
    int32_t        member_count;
    member_status  status;
    int            status_can_send;  /* для ST_RESTRICTED */
    UT_hash_handle hh;
} basic_rec;

typedef struct {
    int64_t        group_id;
    char          *username;
    int32_t        member_count;     /* 0, если неизвестно */
    int            is_channel;
    member_status  status;
    int            status_can_send;  /* ST_RESTRICTED */
    int            admin_can_post;   /* ST_ADMIN в канале */
    int            full_tried;       /* getSupergroupFullInfo уже вызывали */
    UT_hash_handle hh;
} super_rec;

typedef struct {
    int64_t        chat_id;
    char          *title;
    int32_t        unread;
    chat_kind      kind;
    int64_t        type_id;      /* user_id / basic_group_id / supergroup_id */
    int            is_channel;   /* chatTypeSupergroup.is_channel */
    int            perm_can_send;/* chat.permissions.can_send_basic_messages */
    int            archived;
    int            has_chat_lists; /* новые TDLib: принадлежность != позиция */
    UT_hash_handle hh;
} chat_rec;

/* ------------------------------------------------------------------ */
/* ожидание ответов                                                    */
/* ------------------------------------------------------------------ */

typedef struct waiter {
    char           extra[24];
    cJSON         *resp;    /* владеет waiter, забирает ожидающий поток */
    int            done;
    struct waiter *next;
} waiter;

typedef struct {
    int64_t   old_id;
    int       ok;      /* 1 -- доставлено, 0 -- ошибка */
    tg_status st;      /* причина при ok == 0 */
} send_result;

/* Состояние авторизации, которое нас интересует. */
typedef enum {
    AUTH_NONE = 0,
    AUTH_WAIT_PARAMS,
    AUTH_WAIT_PHONE,
    AUTH_WAIT_CODE,
    AUTH_WAIT_PASSWORD,
    AUTH_WAIT_EMAIL,
    AUTH_WAIT_EMAIL_CODE,
    AUTH_WAIT_OTHER_DEVICE,
    AUTH_WAIT_REGISTRATION,
    AUTH_READY,
    AUTH_CLOSED
} auth_state;

struct tg_backend {
    int        client_id;
    int        api_id;
    char      *api_hash;
    char      *db_dir;

    pthread_t  recv_thread;
    int        thread_started;
    volatile int stop;          /* 1 -- recv_thread должен выйти */

    pthread_mutex_t mtx;
    pthread_cond_t  cv;         /* ответы, апдейты отправки, авторизация */

    waiter    *waiters;
    uint64_t   next_extra;

    auth_state auth;
    uint64_t   auth_seq;        /* растёт на каждый updateAuthorizationState */
    char      *auth_link;       /* ссылка для входа с другого устройства     */

    send_result send_ring[SEND_RING];
    uint64_t    send_seen;      /* сколько всего результатов положили        */

    int64_t    my_id;           /* наш user_id, 0 -- ещё не узнали           */

    user_rec  *users;
    basic_rec *basics;
    super_rec *supers;
    chat_rec  *chats;
};

/* ------------------------------------------------------------------ */
/* мелкие помощники                                                    */
/* ------------------------------------------------------------------ */

const char *tg_status_str(tg_status st)
{
    switch (st) {
    case TG_OK:        return "TG_OK";
    case TG_ERR_PERM:  return "TG_ERR_PERM";
    case TG_ERR_NET:   return "TG_ERR_NET";
    case TG_ERR_FLOOD: return "TG_ERR_FLOOD";
    case TG_ERR_AUTH:  return "TG_ERR_AUTH";
    default:           return "TG_ERR_OTHER";
    }
}

static char *xstrdup(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* Заменяет *dst копией s (s может быть NULL). */
static void set_str(char **dst, const char *s)
{
    char *p = xstrdup(s);

    if (s && !p)
        return;              /* нет памяти -- оставляем старое значение */
    free(*dst);
    *dst = p;
}

static const char *j_type(const cJSON *o)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "@type");

    return cJSON_IsString(t) ? t->valuestring : "";
}

static const char *j_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/*
 * int53 приходит обычным JSON-числом (double точен до 2^53), а int64 --
 * строкой. Понимаем оба варианта.
 */
static int64_t j_i64(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (cJSON_IsNumber(v))
        return (int64_t)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring)
        return (int64_t)strtoll(v->valuestring, NULL, 10);
    return 0;
}

static int32_t j_i32(const cJSON *o, const char *key)
{
    return (int32_t)j_i64(o, key);
}

static int j_bool(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v) ? 1 : 0;
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0;
    return 0;
}

static cJSON *j_obj(const cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, key);

    return cJSON_IsObject(v) ? v : NULL;
}

static void add_i64(cJSON *o, const char *key, int64_t v)
{
    cJSON_AddNumberToObject(o, key, (double)v);
}

static void abs_deadline(struct timespec *ts, int sec)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += sec;
}

/* mkdir -p для db_dir. Портит path по ходу дела и восстанавливает. */
static int mkdir_p(const char *path)
{
    char *copy = xstrdup(path);
    char *p;
    int rc = 0;

    if (!copy)
        return -1;
    for (p = copy + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(copy, 0700) != 0 && errno != EEXIST)
            rc = -1;
        *p = '/';
    }
    if (rc == 0 && mkdir(copy, 0700) != 0 && errno != EEXIST)
        rc = -1;
    free(copy);
    return rc;
}

/* ------------------------------------------------------------------ */
/* отправка запросов и ожидание ответа                                 */
/* ------------------------------------------------------------------ */

/* Отправляет запрос, не дожидаясь ответа. req освобождается здесь. */
static void td_send_json(tg_backend *b, cJSON *req)
{
    char *s = cJSON_PrintUnformatted(req);

    cJSON_Delete(req);
    if (!s)
        return;
    td_send(b->client_id, s);
    free(s);
}

/*
 * Отправляет запрос и ждёт ответ с тем же "@extra".
 * req освобождается здесь всегда. *resp -- ответ, освобождает вызывающий.
 * Возвращает 0 при получении ответа, -1 при таймауте/закрытии клиента.
 */
static int request(tg_backend *b, cJSON *req, cJSON **resp, int timeout_sec)
{
    waiter w;
    struct timespec ts;
    waiter **pp;
    int rc = 0;

    *resp = NULL;
    memset(&w, 0, sizeof(w));

    pthread_mutex_lock(&b->mtx);
    snprintf(w.extra, sizeof(w.extra), "%" PRIu64, ++b->next_extra);
    w.next = b->waiters;
    b->waiters = &w;
    pthread_mutex_unlock(&b->mtx);

    cJSON_AddStringToObject(req, "@extra", w.extra);
    td_send_json(b, req);

    abs_deadline(&ts, timeout_sec);
    pthread_mutex_lock(&b->mtx);
    while (!w.done && !b->stop) {
        if (pthread_cond_timedwait(&b->cv, &b->mtx, &ts) == ETIMEDOUT)
            break;
    }
    /* снимаем себя со списка в любом случае: w лежит на стеке */
    for (pp = &b->waiters; *pp; pp = &(*pp)->next) {
        if (*pp == &w) {
            *pp = w.next;
            break;
        }
    }
    if (w.done) {
        *resp = w.resp;
        w.resp = NULL;
    } else {
        rc = -1;
    }
    pthread_mutex_unlock(&b->mtx);

    cJSON_Delete(w.resp);   /* на случай гонки done/таймаут -- тут уже NULL */
    return rc;
}

/*
 * Код ошибки TDLib -> tg_status. Часть "нет прав" TDLib отдаёт с кодом 400,
 * поэтому дополнительно смотрим на текст сообщения.
 */
static tg_status map_error_msg(int code, const char *msg)
{
    static const char *const perm_markers[] = {
        "ADMIN_REQUIRED", "PRIVACY", "USER_IS_BLOCKED", "WRITE_FORBIDDEN",
        "CHAT_CHANGE_INFO_FORBIDDEN",
        "rights", "inaccessible", "forbidden",
    };

    switch (code) {
    case 401: return TG_ERR_AUTH;
    case 403: return TG_ERR_PERM;
    case 429: return TG_ERR_FLOOD;
    case 500: return TG_ERR_NET;
    default:  break;
    }
    if (msg) {
        for (size_t i = 0; i < sizeof(perm_markers) / sizeof(perm_markers[0]); i++)
            if (strstr(msg, perm_markers[i]))
                return TG_ERR_PERM;
    }
    return TG_ERR_OTHER;
}

static tg_status map_error(const cJSON *err)
{
    return map_error_msg(j_i32(err, "code"), j_str(err, "message"));
}

/*
 * request() + разбор "error". При ошибке TDLib *resp освобождается и
 * выставляется NULL, код ошибки возвращается. err_code (если не NULL)
 * получает код TDLib -- нужно, чтобы отличить 404 от loadChats.
 */
static tg_status request_checked(tg_backend *b, cJSON *req, cJSON **resp,
                                 int timeout_sec, int *err_code)
{
    tg_status st;

    if (err_code)
        *err_code = 0;
    if (request(b, req, resp, timeout_sec) != 0)
        return TG_ERR_NET;
    if (strcmp(j_type(*resp), "error") == 0) {
        if (err_code)
            *err_code = j_i32(*resp, "code");
        st = map_error(*resp);
        fprintf(stderr, "tgfs: TDLib error %d: %s\n",
                j_i32(*resp, "code"),
                j_str(*resp, "message") ? j_str(*resp, "message") : "?");
        cJSON_Delete(*resp);
        *resp = NULL;
        return st;
    }
    return TG_OK;
}

static cJSON *new_req(const char *type)
{
    cJSON *o = cJSON_CreateObject();

    if (o)
        cJSON_AddStringToObject(o, "@type", type);
    return o;
}

/* ------------------------------------------------------------------ */
/* разбор апдейтов                                                     */
/* ------------------------------------------------------------------ */

static member_status parse_status(const cJSON *status, int *can_send, int *admin_can_post)
{
    const char *t;

    *can_send = 0;
    *admin_can_post = 0;
    if (!status)
        return ST_UNKNOWN;
    t = j_type(status);

    if (strcmp(t, "chatMemberStatusCreator") == 0)
        return ST_CREATOR;
    if (strcmp(t, "chatMemberStatusAdministrator") == 0) {
        const cJSON *r = j_obj(status, "rights");

        *admin_can_post = r ? j_bool(r, "can_post_messages") : 0;
        return ST_ADMIN;
    }
    if (strcmp(t, "chatMemberStatusMember") == 0)
        return ST_MEMBER;
    if (strcmp(t, "chatMemberStatusRestricted") == 0) {
        const cJSON *p = j_obj(status, "permissions");

        *can_send = p ? j_bool(p, "can_send_basic_messages") : 0;
        /* ограниченный, но уже не состоящий в группе -- фактически ушёл */
        if (!j_bool(status, "is_member"))
            return ST_LEFT;
        return ST_RESTRICTED;
    }
    if (strcmp(t, "chatMemberStatusLeft") == 0)
        return ST_LEFT;
    if (strcmp(t, "chatMemberStatusBanned") == 0)
        return ST_BANNED;
    return ST_UNKNOWN;
}

/* Первое активное @username из объекта usernames. */
static const char *first_username(const cJSON *owner)
{
    const cJSON *un = j_obj(owner, "usernames");
    const cJSON *arr, *it;

    if (!un)
        return NULL;
    arr = cJSON_GetObjectItemCaseSensitive((cJSON *)un, "active_usernames");
    if (!cJSON_IsArray(arr))
        return NULL;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it) && it->valuestring[0])
            return it->valuestring;
    }
    return NULL;
}

/* Вызывается с захваченным b->mtx. */
static void upsert_user(tg_backend *b, const cJSON *user)
{
    int64_t id = j_i64(user, "id");
    user_rec *u;
    const char *first = j_str(user, "first_name");
    const char *last  = j_str(user, "last_name");
    const char *utype = j_type(j_obj(user, "type"));
    char name[128];

    if (id == 0)
        return;
    HASH_FIND(hh, b->users, &id, sizeof(id), u);
    if (!u) {
        u = calloc(1, sizeof(*u));
        if (!u)
            return;
        u->user_id = id;
        HASH_ADD(hh, b->users, user_id, sizeof(u->user_id), u);
    }
    snprintf(name, sizeof(name), "%s%s%s",
             first ? first : "",
             (first && *first && last && *last) ? " " : "",
             last ? last : "");
    if (!name[0])
        snprintf(name, sizeof(name), "user_%" PRId64, id);
    set_str(&u->name, name);
    set_str(&u->username, first_username(user));
    u->is_bot     = strcmp(utype, "userTypeBot") == 0;
    u->is_deleted = strcmp(utype, "userTypeDeleted") == 0;
}

static void upsert_basic(tg_backend *b, const cJSON *g)
{
    int64_t id = j_i64(g, "id");
    basic_rec *r;
    int can_send, acp;

    if (id == 0)
        return;
    HASH_FIND(hh, b->basics, &id, sizeof(id), r);
    if (!r) {
        r = calloc(1, sizeof(*r));
        if (!r)
            return;
        r->group_id = id;
        HASH_ADD(hh, b->basics, group_id, sizeof(r->group_id), r);
    }
    r->member_count    = j_i32(g, "member_count");
    r->status          = parse_status(j_obj(g, "status"), &can_send, &acp);
    r->status_can_send = can_send;
}

static void upsert_super(tg_backend *b, const cJSON *g)
{
    int64_t id = j_i64(g, "id");
    super_rec *r;
    int can_send, acp;
    int32_t mc;

    if (id == 0)
        return;
    HASH_FIND(hh, b->supers, &id, sizeof(id), r);
    if (!r) {
        r = calloc(1, sizeof(*r));
        if (!r)
            return;
        r->group_id = id;
        HASH_ADD(hh, b->supers, group_id, sizeof(r->group_id), r);
    }
    r->is_channel      = j_bool(g, "is_channel");
    r->status          = parse_status(j_obj(g, "status"), &can_send, &acp);
    r->status_can_send = can_send;
    r->admin_can_post  = acp;
    set_str(&r->username, first_username(g));
    /* в updateSupergroup member_count обычно 0 -- не затираем уточнённое */
    mc = j_i32(g, "member_count");
    if (mc > 0)
        r->member_count = mc;
}

static void upsert_chat(tg_backend *b, const cJSON *chat)
{
    int64_t id = j_i64(chat, "id");
    chat_rec *c;
    const cJSON *type = j_obj(chat, "type");
    const cJSON *perm = j_obj(chat, "permissions");
    const char *tt;

    if (id == 0 || !type)
        return;
    HASH_FIND(hh, b->chats, &id, sizeof(id), c);
    if (!c) {
        c = calloc(1, sizeof(*c));
        if (!c)
            return;
        c->chat_id = id;
        HASH_ADD(hh, b->chats, chat_id, sizeof(c->chat_id), c);
    }
    set_str(&c->title, j_str(chat, "title"));
    c->unread = j_i32(chat, "unread_count");
    c->perm_can_send = perm ? j_bool(perm, "can_send_basic_messages") : 0;
    c->is_channel = 0;

    /* Современная TDLib хранит принадлежность в chat_lists. В старой -- positions. */
    const cJSON *lists = cJSON_GetObjectItemCaseSensitive(chat, "chat_lists");
    const cJSON *it;
    c->has_chat_lists = cJSON_IsArray(lists);
    c->archived = 0;
    if (c->has_chat_lists) {
        cJSON_ArrayForEach(it, lists) {
            if (strcmp(j_type(it), "chatListArchive") == 0)
                c->archived = 1;
        }
    } else {
        const cJSON *positions = cJSON_GetObjectItemCaseSensitive(chat, "positions");
        cJSON_ArrayForEach(it, positions) {
            if (strcmp(j_type(j_obj(it, "list")), "chatListArchive") == 0 &&
                j_i64(it, "order") != 0)
                c->archived = 1;
        }
    }

    tt = j_type(type);
    if (strcmp(tt, "chatTypePrivate") == 0) {
        c->kind    = CK_PRIVATE;
        c->type_id = j_i64(type, "user_id");
    } else if (strcmp(tt, "chatTypeBasicGroup") == 0) {
        c->kind    = CK_BASIC;
        c->type_id = j_i64(type, "basic_group_id");
    } else if (strcmp(tt, "chatTypeSupergroup") == 0) {
        c->kind       = CK_SUPER;
        c->type_id    = j_i64(type, "supergroup_id");
        c->is_channel = j_bool(type, "is_channel");
    } else {
        /* chatTypeSecret -- по условию пропускаем */
        c->kind    = CK_SECRET;
        c->type_id = 0;
    }
}

static void push_send_result(tg_backend *b, int64_t old_id, int ok, tg_status st)
{
    send_result *slot = &b->send_ring[b->send_seen % SEND_RING];

    slot->old_id = old_id;
    slot->ok     = ok;
    slot->st     = st;
    b->send_seen++;
}

static auth_state parse_auth(const char *t)
{
    if (strcmp(t, "authorizationStateWaitTdlibParameters") == 0) return AUTH_WAIT_PARAMS;
    if (strcmp(t, "authorizationStateWaitPhoneNumber") == 0)     return AUTH_WAIT_PHONE;
    if (strcmp(t, "authorizationStateWaitCode") == 0)            return AUTH_WAIT_CODE;
    if (strcmp(t, "authorizationStateWaitPassword") == 0)        return AUTH_WAIT_PASSWORD;
    if (strcmp(t, "authorizationStateWaitEmailAddress") == 0)    return AUTH_WAIT_EMAIL;
    if (strcmp(t, "authorizationStateWaitEmailCode") == 0)       return AUTH_WAIT_EMAIL_CODE;
    if (strcmp(t, "authorizationStateWaitOtherDeviceConfirmation") == 0)
                                                                 return AUTH_WAIT_OTHER_DEVICE;
    if (strcmp(t, "authorizationStateWaitRegistration") == 0)    return AUTH_WAIT_REGISTRATION;
    if (strcmp(t, "authorizationStateReady") == 0)               return AUTH_READY;
    if (strcmp(t, "authorizationStateClosed") == 0)              return AUTH_CLOSED;
    return AUTH_NONE;
}

/* Разбор одного апдейта. Вызывается с захваченным b->mtx. */
static void handle_update(tg_backend *b, const cJSON *obj)
{
    const char *t = j_type(obj);

    if (strcmp(t, "updateAuthorizationState") == 0) {
        const cJSON *st = j_obj(obj, "authorization_state");
        auth_state a = st ? parse_auth(j_type(st)) : AUTH_NONE;

        if (a == AUTH_WAIT_OTHER_DEVICE)
            set_str(&b->auth_link, j_str(st, "link"));
        if (a != AUTH_NONE) {
            b->auth = a;
            b->auth_seq++;
            pthread_cond_broadcast(&b->cv);
        }
        return;
    }
    if (strcmp(t, "updateNewChat") == 0) {
        upsert_chat(b, j_obj(obj, "chat"));
        return;
    }
    if (strcmp(t, "updateChatAddedToList") == 0 ||
        strcmp(t, "updateChatRemovedFromList") == 0) {
        int64_t id = j_i64(obj, "chat_id");
        chat_rec *c;
        const char *list = j_type(j_obj(obj, "chat_list"));
        int added = strcmp(t, "updateChatAddedToList") == 0;

        HASH_FIND(hh, b->chats, &id, sizeof(id), c);
        if (c) {
            c->has_chat_lists = 1;
            if (strcmp(list, "chatListArchive") == 0)
                c->archived = added;
            else if (added && strcmp(list, "chatListMain") == 0)
                c->archived = 0;
        }
        return;
    }
    if (strcmp(t, "updateChatPosition") == 0) {
        int64_t id = j_i64(obj, "chat_id");
        chat_rec *c;
        const cJSON *position = j_obj(obj, "position");
        const char *list = j_type(j_obj(position, "list"));
        int present = j_i64(position, "order") != 0;

        HASH_FIND(hh, b->chats, &id, sizeof(id), c);
        /* В новых версиях позиция не определяет принадлежность списку. */
        if (c && !c->has_chat_lists) {
            if (strcmp(list, "chatListArchive") == 0)
                c->archived = present;
            else if (present && strcmp(list, "chatListMain") == 0)
                c->archived = 0;
        }
        return;
    }
    if (strcmp(t, "updateChatTitle") == 0) {
        int64_t id = j_i64(obj, "chat_id");
        chat_rec *c;

        HASH_FIND(hh, b->chats, &id, sizeof(id), c);
        if (c)
            set_str(&c->title, j_str(obj, "title"));
        return;
    }
    if (strcmp(t, "updateChatReadInbox") == 0) {
        int64_t id = j_i64(obj, "chat_id");
        chat_rec *c;

        HASH_FIND(hh, b->chats, &id, sizeof(id), c);
        if (c)
            c->unread = j_i32(obj, "unread_count");
        return;
    }
    if (strcmp(t, "updateChatPermissions") == 0) {
        int64_t id = j_i64(obj, "chat_id");
        chat_rec *c;
        const cJSON *p = j_obj(obj, "permissions");

        HASH_FIND(hh, b->chats, &id, sizeof(id), c);
        if (c && p)
            c->perm_can_send = j_bool(p, "can_send_basic_messages");
        return;
    }
    if (strcmp(t, "updateUser") == 0) {
        upsert_user(b, j_obj(obj, "user"));
        return;
    }
    if (strcmp(t, "updateBasicGroup") == 0) {
        upsert_basic(b, j_obj(obj, "basic_group"));
        return;
    }
    if (strcmp(t, "updateSupergroup") == 0) {
        upsert_super(b, j_obj(obj, "supergroup"));
        return;
    }
    if (strcmp(t, "updateMessageSendSucceeded") == 0) {
        push_send_result(b, j_i64(obj, "old_message_id"), 1, TG_OK);
        pthread_cond_broadcast(&b->cv);
        return;
    }
    if (strcmp(t, "updateMessageSendFailed") == 0) {
        const cJSON *e = j_obj(obj, "error");

        push_send_result(b, j_i64(obj, "old_message_id"), 0,
                         e ? map_error(e) : TG_ERR_OTHER);
        pthread_cond_broadcast(&b->cv);
        return;
    }
    /* остальные апдейты нам не нужны */
}

/* ------------------------------------------------------------------ */
/* поток приёма                                                        */
/* ------------------------------------------------------------------ */

static void *recv_thread_fn(void *arg)
{
    tg_backend *b = arg;

    while (!b->stop) {
        const char *raw = td_receive(1.0);
        cJSON *obj;
        const cJSON *extra;

        if (!raw)
            continue;

        obj = cJSON_Parse(raw);
        if (!obj)
            continue;

        extra = cJSON_GetObjectItemCaseSensitive(obj, "@extra");
        if (cJSON_IsString(extra)) {
            waiter *w;
            int delivered = 0;

            pthread_mutex_lock(&b->mtx);
            for (w = b->waiters; w; w = w->next) {
                if (!w->done && strcmp(w->extra, extra->valuestring) == 0) {
                    w->resp = obj;
                    w->done = 1;
                    delivered = 1;
                    break;
                }
            }
            pthread_cond_broadcast(&b->cv);
            pthread_mutex_unlock(&b->mtx);
            if (!delivered)
                cJSON_Delete(obj);  /* ответ на запрос, который уже не ждут */
            continue;
        }

        pthread_mutex_lock(&b->mtx);
        handle_update(b, obj);
        if (b->auth == AUTH_CLOSED)
            b->stop = 1;
        pthread_mutex_unlock(&b->mtx);
        cJSON_Delete(obj);
    }

    /* клиент закрыт -- будим всех, кто ждёт ответа */
    pthread_mutex_lock(&b->mtx);
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mtx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* создание / уничтожение                                              */
/* ------------------------------------------------------------------ */

tg_backend *tg_backend_create(int api_id, const char *api_hash, const char *db_dir)
{
    tg_backend *b;

    if (!api_hash || !db_dir || api_id <= 0)
        return NULL;

    b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;

    b->api_id   = api_id;
    b->api_hash = xstrdup(api_hash);
    b->db_dir   = xstrdup(db_dir);
    if (!b->api_hash || !b->db_dir)
        goto fail;

    if (mkdir_p(b->db_dir) != 0) {
        fprintf(stderr, "tgfs: не удалось создать %s: %s\n",
                b->db_dir, strerror(errno));
        goto fail;
    }

    if (pthread_mutex_init(&b->mtx, NULL) != 0)
        goto fail;
    if (pthread_cond_init(&b->cv, NULL) != 0) {
        pthread_mutex_destroy(&b->mtx);
        goto fail;
    }

    /* шум TDLib в stderr мешает читать вывод ФС */
    {
        cJSON *v = new_req("setLogVerbosityLevel");
        char *s;

        if (v) {
            cJSON_AddNumberToObject(v, "new_verbosity_level", 1);
            s = cJSON_PrintUnformatted(v);
            cJSON_Delete(v);
            if (s) {
                const char *res = td_execute(s);

                (void)res;   /* строка принадлежит TDLib */
                free(s);
            }
        }
    }

    b->client_id = td_create_client_id();

    if (pthread_create(&b->recv_thread, NULL, recv_thread_fn, b) != 0) {
        pthread_cond_destroy(&b->cv);
        pthread_mutex_destroy(&b->mtx);
        goto fail;
    }
    b->thread_started = 1;

    /* первый запрос "будит" клиент и запускает поток апдейтов */
    td_send_json(b, new_req("getOption"));

    return b;

fail:
    free(b->api_hash);
    free(b->db_dir);
    free(b);
    return NULL;
}

static void free_tables(tg_backend *b)
{
    user_rec *u, *ut;
    basic_rec *g, *gt;
    super_rec *s, *st;
    chat_rec *c, *ct;

    HASH_ITER(hh, b->users, u, ut) {
        HASH_DEL(b->users, u);
        free(u->name);
        free(u->username);
        free(u);
    }
    HASH_ITER(hh, b->basics, g, gt) {
        HASH_DEL(b->basics, g);
        free(g);
    }
    HASH_ITER(hh, b->supers, s, st) {
        HASH_DEL(b->supers, s);
        free(s->username);
        free(s);
    }
    HASH_ITER(hh, b->chats, c, ct) {
        HASH_DEL(b->chats, c);
        free(c->title);
        free(c);
    }
}

void tg_backend_destroy(tg_backend *b)
{
    if (!b)
        return;

    if (b->thread_started) {
        td_send_json(b, new_req("close"));
        /* ждём authorizationStateClosed, но не вечно */
        for (int i = 0; i < 50 && !b->stop; i++)
            usleep(100 * 1000);
        b->stop = 1;
        pthread_join(b->recv_thread, NULL);
        pthread_cond_destroy(&b->cv);
        pthread_mutex_destroy(&b->mtx);
    }

    free_tables(b);
    free(b->auth_link);
    free(b->api_hash);
    free(b->db_dir);
    free(b);
}

/* ------------------------------------------------------------------ */
/* авторизация                                                         */
/* ------------------------------------------------------------------ */

static void send_tdlib_parameters(tg_backend *b)
{
    cJSON *r = new_req("setTdlibParameters");
    char files[4096];

    if (!r)
        return;
    snprintf(files, sizeof(files), "%s/files", b->db_dir);

    cJSON_AddBoolToObject(r, "use_test_dc", 0);
    cJSON_AddStringToObject(r, "database_directory", b->db_dir);
    cJSON_AddStringToObject(r, "files_directory", files);
    cJSON_AddStringToObject(r, "database_encryption_key", "");
    cJSON_AddBoolToObject(r, "use_file_database", 0);
    cJSON_AddBoolToObject(r, "use_chat_info_database", 1);
    cJSON_AddBoolToObject(r, "use_message_database", 1);
    cJSON_AddBoolToObject(r, "use_secret_chats", 0);
    cJSON_AddNumberToObject(r, "api_id", b->api_id);
    cJSON_AddStringToObject(r, "api_hash", b->api_hash);
    cJSON_AddStringToObject(r, "system_language_code", "ru");
    cJSON_AddStringToObject(r, "device_model", "tgfs");
    cJSON_AddStringToObject(r, "application_version", "0.1");

    td_send_json(b, r);
}

/* Читает строку со stdin без завершающего '\n'. 0 -- успех. */
static int prompt(const char *question, char *buf, size_t cap)
{
    size_t n;

    fputs(question, stderr);
    fflush(stderr);
    if (!fgets(buf, (int)cap, stdin))
        return -1;
    n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return n ? 0 : -1;
}

tg_status tg_login(tg_backend *b)
{
    uint64_t seen = 0;
    char buf[256];

    if (!b)
        return TG_ERR_OTHER;

    for (;;) {
        auth_state st;
        char *link = NULL;
        struct timespec ts;
        int timed_out = 0;

        abs_deadline(&ts, LOGIN_TIMEOUT_SEC);
        pthread_mutex_lock(&b->mtx);
        while (b->auth_seq == seen && !b->stop) {
            if (pthread_cond_timedwait(&b->cv, &b->mtx, &ts) == ETIMEDOUT) {
                timed_out = 1;
                break;
            }
        }
        seen = b->auth_seq;
        st   = b->auth;
        if (st == AUTH_WAIT_OTHER_DEVICE && b->auth_link)
            link = xstrdup(b->auth_link);
        pthread_mutex_unlock(&b->mtx);

        if (timed_out && st == AUTH_NONE) {
            fprintf(stderr, "tgfs: TDLib не отвечает\n");
            return TG_ERR_NET;
        }
        if (b->stop || st == AUTH_CLOSED)
            return TG_ERR_NET;

        switch (st) {
        case AUTH_WAIT_PARAMS:
            send_tdlib_parameters(b);
            break;

        case AUTH_WAIT_PHONE: {
            cJSON *r;

            if (prompt("Номер телефона (+7...): ", buf, sizeof(buf)) != 0)
                return TG_ERR_AUTH;
            r = new_req("setAuthenticationPhoneNumber");
            if (!r)
                return TG_ERR_OTHER;
            cJSON_AddStringToObject(r, "phone_number", buf);
            td_send_json(b, r);
            break;
        }

        case AUTH_WAIT_CODE: {
            cJSON *r;

            if (prompt("Код из Telegram: ", buf, sizeof(buf)) != 0)
                return TG_ERR_AUTH;
            r = new_req("checkAuthenticationCode");
            if (!r)
                return TG_ERR_OTHER;
            cJSON_AddStringToObject(r, "code", buf);
            td_send_json(b, r);
            break;
        }

        case AUTH_WAIT_PASSWORD: {
            cJSON *r;
            char *pw = getpass("Пароль 2FA: ");

            if (!pw)
                return TG_ERR_AUTH;
            r = new_req("checkAuthenticationPassword");
            if (!r) {
                memset(pw, 0, strlen(pw));
                return TG_ERR_OTHER;
            }
            cJSON_AddStringToObject(r, "password", pw);
            memset(pw, 0, strlen(pw));
            td_send_json(b, r);
            break;
        }

        case AUTH_WAIT_EMAIL: {
            cJSON *r;

            if (prompt("E-mail: ", buf, sizeof(buf)) != 0)
                return TG_ERR_AUTH;
            r = new_req("setAuthenticationEmailAddress");
            if (!r)
                return TG_ERR_OTHER;
            cJSON_AddStringToObject(r, "email_address", buf);
            td_send_json(b, r);
            break;
        }

        case AUTH_WAIT_EMAIL_CODE: {
            cJSON *r, *code;

            if (prompt("Код из письма: ", buf, sizeof(buf)) != 0)
                return TG_ERR_AUTH;
            r = new_req("checkAuthenticationEmailCode");
            code = new_req("emailAddressAuthenticationCode");
            if (!r || !code) {
                cJSON_Delete(r);
                cJSON_Delete(code);
                return TG_ERR_OTHER;
            }
            cJSON_AddStringToObject(code, "code", buf);
            cJSON_AddItemToObject(r, "code", code);
            td_send_json(b, r);
            break;
        }

        case AUTH_WAIT_OTHER_DEVICE:
            fprintf(stderr, "Подтвердите вход по ссылке: %s\n",
                    link ? link : "(ссылка не пришла)");
            free(link);
            break;

        case AUTH_WAIT_REGISTRATION:
            fprintf(stderr, "tgfs: регистрация нового аккаунта не поддерживается\n");
            return TG_ERR_AUTH;

        case AUTH_READY:
            fprintf(stderr, "tgfs: авторизация выполнена\n");
            return TG_OK;

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* пользователи и чаты по требованию                                   */
/* ------------------------------------------------------------------ */

/*
 * Копирует данные пользователя из таблицы; если его там нет -- getUser.
 * Любой из выходных указателей может быть NULL. Строки освобождает
 * вызывающий. Вызывается БЕЗ b->mtx. 0 -- нашли, -1 -- нет.
 */
static int user_info(tg_backend *b, int64_t uid, char **name, char **username,
                     int *is_bot, int *is_deleted)
{
    int found = 0;

    if (name)       *name = NULL;
    if (username)   *username = NULL;
    if (is_bot)     *is_bot = 0;
    if (is_deleted) *is_deleted = 0;

    for (int attempt = 0; attempt < 2 && !found; attempt++) {
        user_rec *u;

        if (attempt == 1) {
            cJSON *r = new_req("getUser");
            cJSON *resp = NULL;

            if (!r)
                break;
            add_i64(r, "user_id", uid);
            if (request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL) != TG_OK || !resp)
                break;
            pthread_mutex_lock(&b->mtx);
            upsert_user(b, resp);
            pthread_mutex_unlock(&b->mtx);
            cJSON_Delete(resp);
        }

        pthread_mutex_lock(&b->mtx);
        HASH_FIND(hh, b->users, &uid, sizeof(uid), u);
        if (u && u->name) {
            if (name)       *name = xstrdup(u->name);
            if (username)   *username = xstrdup(u->username);
            if (is_bot)     *is_bot = u->is_bot;
            if (is_deleted) *is_deleted = u->is_deleted;
            found = 1;
        }
        pthread_mutex_unlock(&b->mtx);
    }
    return found ? 0 : -1;
}

/* Наш user_id; при первом вызове -- getMe. 0, если узнать не удалось. */
static int64_t get_my_id(tg_backend *b)
{
    int64_t id;
    cJSON *r, *resp = NULL;

    pthread_mutex_lock(&b->mtx);
    id = b->my_id;
    pthread_mutex_unlock(&b->mtx);
    if (id)
        return id;

    r = new_req("getMe");
    if (!r)
        return 0;
    if (request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL) == TG_OK && resp) {
        id = j_i64(resp, "id");
        pthread_mutex_lock(&b->mtx);
        upsert_user(b, resp);
        b->my_id = id;
        pthread_mutex_unlock(&b->mtx);
    }
    cJSON_Delete(resp);
    return id;
}

/* Гарантирует, что чат есть в таблице (иначе getChat). Без b->mtx. */
static tg_status ensure_chat(tg_backend *b, int64_t chat_id)
{
    chat_rec *c;
    cJSON *r, *resp = NULL;
    tg_status st;

    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &chat_id, sizeof(chat_id), c);
    pthread_mutex_unlock(&b->mtx);
    if (c)
        return TG_OK;

    r = new_req("getChat");
    if (!r)
        return TG_ERR_OTHER;
    add_i64(r, "chat_id", chat_id);
    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    if (st == TG_OK && resp) {
        pthread_mutex_lock(&b->mtx);
        upsert_chat(b, resp);
        pthread_mutex_unlock(&b->mtx);
    }
    cJSON_Delete(resp);
    return st;
}

/* ------------------------------------------------------------------ */
/* список чатов                                                        */
/* ------------------------------------------------------------------ */

/*
 * Просит TDLib догрузить все чаты из выбранного списка. Чаты приходят не в
 * ответе, а апдейтами updateNewChat; признак "больше нечего грузить" --
 * ошибка 404 на loadChats.
 */
static void load_chat_list(tg_backend *b, const char *list_type)
{
    for (int i = 0; i < 64; i++) {
        cJSON *r = new_req("loadChats");
        cJSON *list = new_req(list_type);
        cJSON *resp = NULL;
        int stop = 0;

        if (!r || !list) {
            cJSON_Delete(r);
            cJSON_Delete(list);
            return;
        }
        cJSON_AddItemToObject(r, "chat_list", list);
        cJSON_AddNumberToObject(r, "limit", 100);

        if (request(b, r, &resp, REQ_TIMEOUT_SEC) != 0)
            return;                    /* таймаут -- довольствуемся тем, что есть */

        if (strcmp(j_type(resp), "error") == 0) {
            int code = j_i32(resp, "code");

            /* 404 "Chats not found" -- всё уже загружено, это штатный ответ */
            if (code != 404)
                fprintf(stderr, "tgfs: loadChats: %d %s\n", code,
                        j_str(resp, "message") ? j_str(resp, "message") : "?");
            stop = 1;
        }
        cJSON_Delete(resp);
        if (stop)
            return;
    }
}

static void load_all_chats(tg_backend *b)
{
    load_chat_list(b, "chatListMain");
    load_chat_list(b, "chatListArchive");
}

/* Уточняет число участников супергруппы/канала. Без b->mtx. */
static int32_t fetch_super_members(tg_backend *b, int64_t supergroup_id)
{
    cJSON *r = new_req("getSupergroupFullInfo");
    cJSON *resp = NULL;
    int32_t mc = -1;

    if (!r)
        return -1;
    add_i64(r, "supergroup_id", supergroup_id);
    if (request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL) == TG_OK && resp) {
        int32_t v = j_i32(resp, "member_count");

        if (v > 0)
            mc = v;
    }
    cJSON_Delete(resp);
    return mc;
}

/* Сводит внутренние таблицы в tg_chat. Вызывается с захваченным b->mtx. */
static int fill_chat(tg_backend *b, const chat_rec *c, tg_chat *out,
                     int64_t *need_super)
{
    *need_super = 0;
    memset(out, 0, sizeof(*out));
    out->id      = c->chat_id;
    out->unread  = c->unread;
    out->members = -1;
    out->archived = c->archived;

    switch (c->kind) {
    case CK_PRIVATE: {
        user_rec *u;

        HASH_FIND(hh, b->users, &c->type_id, sizeof(c->type_id), u);
        out->type      = TG_USER;
        out->user_id   = c->type_id;
        out->can_write = u ? !u->is_deleted : 1;
        out->username  = xstrdup(u ? u->username : NULL);
        out->title     = xstrdup(c->title && *c->title ? c->title
                                                       : (u ? u->name : NULL));
        break;
    }
    case CK_BASIC: {
        basic_rec *g;

        HASH_FIND(hh, b->basics, &c->type_id, sizeof(c->type_id), g);
        /* вышли из группы или нас удалили -- не показываем */
        if (g && (g->status == ST_LEFT || g->status == ST_BANNED))
            return -1;
        out->type  = TG_GROUP;
        out->title = xstrdup(c->title);
        if (g) {
            out->members = g->member_count > 0 ? g->member_count : -1;
            switch (g->status) {
            case ST_CREATOR:
            case ST_ADMIN:      out->can_write = 1; break;
            case ST_MEMBER:     out->can_write = c->perm_can_send; break;
            case ST_RESTRICTED: out->can_write = g->status_can_send; break;
            default:            out->can_write = 0; break;
            }
        } else {
            out->can_write = c->perm_can_send;
        }
        break;
    }
    case CK_SUPER: {
        super_rec *g;

        HASH_FIND(hh, b->supers, &c->type_id, sizeof(c->type_id), g);
        /* вышли из группы/канала или нас удалили -- не показываем */
        if (g && (g->status == ST_LEFT || g->status == ST_BANNED))
            return -1;
        out->title = xstrdup(c->title);
        if (!g) {
            out->type      = c->is_channel ? TG_CHANNEL : TG_GROUP;
            out->can_write = c->is_channel ? 0 : c->perm_can_send;
            break;
        }
        out->type     = g->is_channel ? TG_CHANNEL : TG_GROUP;
        out->username = xstrdup(g->username);
        if (g->member_count > 0)
            out->members = g->member_count;
        else if (!g->full_tried)
            *need_super = c->type_id;

        if (g->is_channel) {
            out->can_write = (g->status == ST_CREATOR) ||
                             (g->status == ST_ADMIN && g->admin_can_post);
        } else {
            switch (g->status) {
            case ST_CREATOR:
            case ST_ADMIN:      out->can_write = 1; break;
            case ST_MEMBER:     out->can_write = c->perm_can_send; break;
            case ST_RESTRICTED: out->can_write = g->status_can_send; break;
            default:            out->can_write = 0; break;
            }
        }
        break;
    }
    default:
        return -1;   /* секретный чат -- пропускаем */
    }

    if (!out->title)
        out->title = xstrdup("");
    return out->title ? 0 : -1;
}

tg_status tg_list_chats(tg_backend *b, tg_chat **out, size_t *n)
{
    chat_rec *c, *tmp;
    tg_chat *arr = NULL;
    size_t cap = 0, cnt = 0;
    int64_t *pending = NULL;    /* супергруппы, для которых нужен member_count */
    size_t npending = 0;

    if (!b || !out || !n)
        return TG_ERR_OTHER;
    *out = NULL;
    *n   = 0;

    load_all_chats(b);

    pthread_mutex_lock(&b->mtx);
    cap = (size_t)HASH_COUNT(b->chats);
    if (cap == 0) {
        pthread_mutex_unlock(&b->mtx);
        return TG_OK;
    }
    arr = calloc(cap, sizeof(*arr));
    pending = calloc(cap, sizeof(*pending));
    if (!arr || !pending) {
        pthread_mutex_unlock(&b->mtx);
        free(arr);
        free(pending);
        return TG_ERR_OTHER;
    }

    HASH_ITER(hh, b->chats, c, tmp) {
        int64_t need = 0;

        if (cnt == cap)
            break;
        if (fill_chat(b, c, &arr[cnt], &need) != 0) {
            free(arr[cnt].title);
            free(arr[cnt].username);
            memset(&arr[cnt], 0, sizeof(arr[cnt]));
            continue;
        }
        if (need)
            pending[npending++] = need;
        cnt++;
    }
    pthread_mutex_unlock(&b->mtx);

    /* Запросы делаем уже без мьютекса, иначе recv_thread не сможет ответить. */
    for (size_t i = 0; i < npending; i++) {
        int32_t mc = fetch_super_members(b, pending[i]);
        super_rec *g;

        pthread_mutex_lock(&b->mtx);
        HASH_FIND(hh, b->supers, &pending[i], sizeof(pending[i]), g);
        if (g) {
            g->full_tried = 1;
            if (mc > 0)
                g->member_count = mc;
        }
        pthread_mutex_unlock(&b->mtx);

        if (mc <= 0)
            continue;
        /* дописываем в уже собранный массив */
        for (size_t k = 0; k < cnt; k++) {
            int64_t tid;

            pthread_mutex_lock(&b->mtx);
            HASH_FIND(hh, b->chats, &arr[k].id, sizeof(arr[k].id), c);
            tid = c ? c->type_id : 0;
            pthread_mutex_unlock(&b->mtx);
            if (tid == pending[i])
                arr[k].members = mc;
        }
    }
    free(pending);

    /* Без b->mtx: recv_thread нужен для получения ответов TDLib.
     * Не выдаём нулевое muted как «включено», если настройки не прочитаны. */
    for (size_t i = 0; i < cnt; i++) {
        tg_status st = tg_get_chat_muted(b, arr[i].id, &arr[i].muted);
        if (st != TG_OK) {
            tg_free_chats(arr, cnt);
            return st;
        }
    }

    *out = arr;
    *n   = cnt;
    return TG_OK;
}

/* ------------------------------------------------------------------ */
/* история                                                             */
/* ------------------------------------------------------------------ */

/* Имя автора сообщения. Может сходить в сеть за пользователем. */
static char *sender_name(tg_backend *b, const cJSON *sender)
{
    const char *t;

    if (!sender)
        return xstrdup("?");
    t = j_type(sender);

    if (strcmp(t, "messageSenderUser") == 0) {
        int64_t uid = j_i64(sender, "user_id");
        char *name = NULL;
        char buf[64];

        if (user_info(b, uid, &name, NULL, NULL, NULL) == 0 && name)
            return name;
        free(name);
        snprintf(buf, sizeof(buf), "user_%" PRId64, uid);
        return xstrdup(buf);
    }

    if (strcmp(t, "messageSenderChat") == 0) {
        int64_t cid = j_i64(sender, "chat_id");
        chat_rec *c;
        char *name = NULL;

        pthread_mutex_lock(&b->mtx);
        HASH_FIND(hh, b->chats, &cid, sizeof(cid), c);
        if (c && c->title)
            name = xstrdup(c->title);
        pthread_mutex_unlock(&b->mtx);
        return name ? name : xstrdup("канал");
    }

    return xstrdup("?");
}

/* Заглушка для нетекстового контента по его @type. */
static const char *content_stub(const char *t)
{
    static const struct { const char *type, *stub; } map[] = {
        { "messagePhoto",            "[фото]"            },
        { "messageVideo",            "[видео]"           },
        { "messageAnimation",        "[гиф]"             },
        { "messageAudio",            "[аудио]"           },
        { "messageVoiceNote",        "[голосовое]"       },
        { "messageVideoNote",        "[видеосообщение]"  },
        { "messageSticker",          "[стикер]"          },
        { "messageDocument",         "[файл]"            },
        { "messageLocation",         "[локация]"         },
        { "messageVenue",            "[место]"           },
        { "messageContact",          "[контакт]"         },
        { "messagePoll",             "[опрос]"           },
        { "messageDice",             "[кубик]"           },
        { "messageGame",             "[игра]"            },
        { "messageInvoice",          "[счёт]"            },
        { "messageCall",             "[звонок]"          },
        { "messagePaidMedia",        "[платное медиа]"   },
        { "messageStory",            "[история]"         },
        { "messageUnsupported",      "[не поддерживается]" },
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(t, map[i].type) == 0)
            return map[i].stub;
    return NULL;
}

/* Текст сообщения: либо сам текст, либо заглушка + подпись. */
static char *content_text(const cJSON *content)
{
    const char *t;
    const cJSON *ft;
    const char *stub, *caption = NULL;

    if (!content)
        return xstrdup("[пусто]");
    t = j_type(content);

    if (strcmp(t, "messageText") == 0) {
        ft = j_obj(content, "text");
        return xstrdup(ft && j_str(ft, "text") ? j_str(ft, "text") : "");
    }
    if (strcmp(t, "messageAnimatedEmoji") == 0) {
        const char *e = j_str(content, "emoji");

        return xstrdup(e ? e : "[эмодзи]");
    }

    stub = content_stub(t);
    ft = j_obj(content, "caption");
    if (ft)
        caption = j_str(ft, "text");

    if (!stub) {
        /* незнакомый тип -- показываем как есть, без префикса "message" */
        char buf[128];

        snprintf(buf, sizeof(buf), "[%s]",
                 strncmp(t, "message", 7) == 0 ? t + 7 : t);
        return xstrdup(buf);
    }
    if (caption && *caption) {
        size_t n = strlen(stub) + strlen(caption) + 2;
        char *p = malloc(n);

        if (p)
            snprintf(p, n, "%s %s", stub, caption);
        return p;
    }
    return xstrdup(stub);
}

/* Выбираем размер по длинной стороне, не полагаясь на порядок sizes. */
static const cJSON *photo_file(const cJSON *content)
{
    const cJSON *sizes, *it, *best = NULL;
    int64_t best_distance = INT64_MAX;

    if (strcmp(j_type(content), "messagePhoto") != 0)
        return NULL;
    sizes = cJSON_GetObjectItemCaseSensitive(j_obj(content, "photo"), "sizes");
    cJSON_ArrayForEach(it, sizes) {
        const cJSON *file = j_obj(it, "photo");
        int32_t w = j_i32(it, "width"), h = j_i32(it, "height");
        int64_t side = w > h ? w : h;
        int64_t distance = side >= 640 ? side - 640 : 640 - side;

        if (j_i32(file, "id") <= 0 || w <= 0 || h <= 0)
            continue;
        if (distance < best_distance) {
            best = file;
            best_distance = distance;
        }
    }
    return best;
}

/* Один алгоритм пагинации для текста и списка фотографий. */
static tg_status get_history(tg_backend *b, int64_t chat_id, int limit,
                             tg_message **out, tg_photo **photos, size_t *n)
{
    tg_message *arr = NULL;
    tg_photo *pics = NULL;
    size_t cnt = 0, npics = 0;
    int64_t from = 0;
    int empty_rounds = 0;
    tg_status st = TG_OK;

    if (out) *out = NULL;
    if (photos) *photos = NULL;
    if (n) *n = 0;
    if (!b || (!out && !photos) || !n || limit <= 0)
        return TG_ERR_OTHER;

    if (out) arr = calloc((size_t)limit, sizeof(*arr));
    else pics = calloc((size_t)limit, sizeof(*pics));
    if (!arr && !pics)
        return TG_ERR_OTHER;

    while (cnt < (size_t)limit) {
        cJSON *r = new_req("getChatHistory");
        cJSON *resp = NULL;
        const cJSON *msgs, *it;
        int want = (int)((size_t)limit - cnt);
        int got = 0;

        if (!r) {
            st = TG_ERR_OTHER;
            break;
        }
        /* Оставляем место для возможного повтора from_message_id. */
        if (from && want < HISTORY_BATCH)
            want++;
        if (want > HISTORY_BATCH)
            want = HISTORY_BATCH;

        add_i64(r, "chat_id", chat_id);
        add_i64(r, "from_message_id", from);
        cJSON_AddNumberToObject(r, "offset", 0);
        cJSON_AddNumberToObject(r, "limit", want);
        cJSON_AddBoolToObject(r, "only_local", 0);

        st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
        if (st != TG_OK)
            break;

        msgs = cJSON_GetObjectItemCaseSensitive(resp, "messages");
        if (cJSON_IsArray(msgs)) {
            cJSON_ArrayForEach(it, msgs) {
                if (cnt == (size_t)limit)
                    break;
                if (!cJSON_IsObject(it))
                    continue;
                int64_t message_id = j_i64(it, "id");
                /* getChatHistory может повторить граничное сообщение. */
                if (message_id <= 0 || (from && message_id >= from))
                    continue;
                if (arr) {
                    arr[cnt].id     = message_id;
                    arr[cnt].date   = j_i64(it, "date");
                    arr[cnt].sender = sender_name(b, j_obj(it, "sender_id"));
                    arr[cnt].text   = content_text(j_obj(it, "content"));
                    if (!arr[cnt].sender || !arr[cnt].text) {
                        free(arr[cnt].sender);
                        free(arr[cnt].text);
                        st = TG_ERR_OTHER;
                        break;
                    }
                } else {
                    const cJSON *file = photo_file(j_obj(it, "content"));
                    if (file) {
                        pics[npics].message_id = message_id;
                        pics[npics].date = j_i64(it, "date");
                        pics[npics].file_id = j_i32(file, "id");
                        pics[npics].size = j_i64(file, "size");
                        npics++;
                    }
                }
                from = message_id;
                cnt++;
                got++;
            }
        }
        cJSON_Delete(resp);
        if (st != TG_OK)
            break;

        if (got == 0) {
            /*
             * Первый вызов нередко приходит пустым: TDLib ещё не подтянул
             * историю с сервера. Даём ему один повторный шанс и выходим.
             */
            if (++empty_rounds >= 2)
                break;
            continue;
        }
        empty_rounds = 0;
    }

    if (st != TG_OK && cnt == 0) {
        tg_free_messages(arr, 0);
        free(pics);
        return st;
    }

    if (out) {
        if (!cnt) { free(arr); arr = NULL; }
        *out = arr;
        *n = cnt;
    } else {
        if (!npics) { free(pics); pics = NULL; }
        *photos = pics;
        *n = npics;
    }
    return TG_OK;
}

tg_status tg_get_messages(tg_backend *b, int64_t chat_id, int limit,
                          tg_message **out, size_t *n)
{
    if (!out) { if (n) *n = 0; return TG_ERR_OTHER; }
    return get_history(b, chat_id, limit, out, NULL, n);
}

tg_status tg_list_photos(tg_backend *b, int64_t chat_id, int limit,
                         tg_photo **out, size_t *n)
{
    if (!out) { if (n) *n = 0; return TG_ERR_OTHER; }
    return get_history(b, chat_id, limit, NULL, out, n);
}

tg_status tg_download_photo(tg_backend *b, int32_t file_id, char **out_path)
{
    cJSON *r, *resp = NULL;
    const cJSON *local;
    const char *path;
    tg_status st;

    if (!out_path) return TG_ERR_OTHER;
    *out_path = NULL;
    if (!b || file_id <= 0) return TG_ERR_OTHER;
    r = new_req("downloadFile");
    if (!r) return TG_ERR_OTHER;
    cJSON_AddNumberToObject(r, "file_id", file_id);
    cJSON_AddNumberToObject(r, "priority", 16);
    cJSON_AddNumberToObject(r, "offset", 0);
    cJSON_AddNumberToObject(r, "limit", 0);
    cJSON_AddBoolToObject(r, "synchronous", 1);
    st = request_checked(b, r, &resp, 60, NULL);
    if (st != TG_OK) return st;
    local = j_obj(resp, "local");
    path = j_str(local, "path");
    if (strcmp(j_type(resp), "file") != 0 ||
        !j_bool(local, "is_downloading_completed") || !path || !*path) {
        cJSON_Delete(resp);
        return TG_ERR_NET;
    }
    *out_path = xstrdup(path);
    cJSON_Delete(resp);
    return *out_path ? TG_OK : TG_ERR_OTHER;
}

void tg_free_photos(tg_photo *photos, size_t n)
{
    (void)n;
    free(photos);
}

/* ------------------------------------------------------------------ */
/* отправка                                                           */
/* ------------------------------------------------------------------ */

tg_status tg_send_message(tg_backend *b, int64_t chat_id, const char *text)
{
    cJSON *r, *content, *ft, *resp = NULL;
    tg_status st, fail_st = TG_ERR_OTHER;
    int64_t old_id;
    uint64_t seen;
    struct timespec ts;
    int result = -1;

    if (!b || !text || !*text)
        return TG_ERR_OTHER;

    r       = new_req("sendMessage");
    content = new_req("inputMessageText");
    ft      = new_req("formattedText");
    if (!r || !content || !ft) {
        cJSON_Delete(r);
        cJSON_Delete(content);
        cJSON_Delete(ft);
        return TG_ERR_OTHER;
    }
    cJSON_AddStringToObject(ft, "text", text);
    cJSON_AddItemToObject(content, "text", ft);
    add_i64(r, "chat_id", chat_id);
    cJSON_AddItemToObject(r, "input_message_content", content);

    /*
     * Засечку по кольцу берём ДО отправки: апдейт о доставке может прийти
     * раньше, чем мы разберём ответ на sendMessage.
     */
    pthread_mutex_lock(&b->mtx);
    seen = b->send_seen;
    pthread_mutex_unlock(&b->mtx);

    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    if (st != TG_OK)
        return st;

    /* ответ -- сообщение с временным id в состоянии pending */
    old_id = j_i64(resp, "id");
    cJSON_Delete(resp);
    if (old_id == 0)
        return TG_ERR_OTHER;

    abs_deadline(&ts, SEND_TIMEOUT_SEC);
    pthread_mutex_lock(&b->mtx);
    for (;;) {
        uint64_t first = b->send_seen > SEND_RING ? b->send_seen - SEND_RING : 0;
        uint64_t i = seen > first ? seen : first;

        for (; i < b->send_seen; i++) {
            const send_result *sr = &b->send_ring[i % SEND_RING];

            if (sr->old_id == old_id) {
                result  = sr->ok;
                fail_st = sr->st;
                break;
            }
        }
        if (result >= 0 || b->stop)
            break;
        if (pthread_cond_timedwait(&b->cv, &b->mtx, &ts) == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&b->mtx);

    if (result == 1)
        return TG_OK;
    if (result == 0)
        return fail_st;
    return TG_ERR_NET;   /* подтверждения не дождались */
}

tg_status tg_create_chat(tg_backend *b, tg_chat_type type,
                         const char *title, tg_chat **out)
{
    cJSON *r, *resp = NULL;
    const cJSON *chat_type;
    const char *created_title;
    tg_chat *arr;
    chat_rec *reserved_chat, *c;
    super_rec *reserved_super, *g;
    int64_t cid, sid;
    tg_status st;

    if (!out)
        return TG_ERR_OTHER;
    *out = NULL;
    if (!b || !title || (type != TG_GROUP && type != TG_CHANNEL))
        return TG_ERR_OTHER;
    pthread_mutex_lock(&b->mtx);
    int ready = b->auth == AUTH_READY && !b->stop;
    pthread_mutex_unlock(&b->mtx);
    if (!ready)
        return TG_ERR_AUTH;

    /* Резервируем структуры результата и кеша до запроса. */
    arr = calloc(1, sizeof(*arr));
    reserved_chat = calloc(1, sizeof(*reserved_chat));
    reserved_super = calloc(1, sizeof(*reserved_super));
    r = new_req("createNewSupergroupChat");
    if (!arr || !reserved_chat || !reserved_super || !r) {
        st = TG_ERR_OTHER;
        goto fail;
    }
    if (!cJSON_AddStringToObject(r, "title", title) ||
        !cJSON_AddBoolToObject(r, "is_channel", type == TG_CHANNEL) ||
        !cJSON_AddBoolToObject(r, "is_forum", 0) ||
        !cJSON_AddStringToObject(r, "description", "") ||
        !cJSON_AddNullToObject(r, "location") ||
        !cJSON_AddNumberToObject(r, "message_auto_delete_time", 0) ||
        !cJSON_AddBoolToObject(r, "for_import", 0)) {
        st = TG_ERR_OTHER;
        goto fail;
    }
    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    r = NULL;  /* request_checked забирает запрос, в том числе при ошибке */
    if (st != TG_OK)
        goto fail;

    cid = j_i64(resp, "id");
    chat_type = j_obj(resp, "type");
    sid = j_i64(chat_type, "supergroup_id");
    created_title = j_str(resp, "title");
    if (strcmp(j_type(resp), "chat") != 0 || cid == 0 || sid <= 0 ||
        strcmp(j_type(chat_type), "chatTypeSupergroup") != 0 ||
        j_bool(chat_type, "is_channel") != (type == TG_CHANNEL) ||
        !created_title) {
        /* Запрос уже ушёл: некорректный ответ не означает, что чат не создан. */
        st = TG_ERR_NET;
        goto fail;
    }
    arr->title = xstrdup(created_title);
    reserved_chat->title = xstrdup(created_title);
    if (!arr->title || !reserved_chat->title) {
        /* Сервер уже создал чат; повторять создание автоматически нельзя. */
        st = TG_ERR_NET;
        goto fail;
    }

    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &cid, sizeof(cid), c);
    if (!c) {
        c = reserved_chat;
        reserved_chat = NULL;
        c->chat_id = cid;
        HASH_ADD(hh, b->chats, chat_id, sizeof(c->chat_id), c);
    } else {
        free(c->title);
        c->title = reserved_chat->title;
        reserved_chat->title = NULL;
    }
    upsert_chat(b, resp);
    /* updateSupergroup обычно приходит до ответа. Если его ещё нет,
     * созданный нами чат всё равно сразу должен быть доступен для записи. */
    HASH_FIND(hh, b->supers, &sid, sizeof(sid), g);
    if (!g) {
        g = reserved_super;
        reserved_super = NULL;
        g->group_id = sid;
        g->is_channel = type == TG_CHANNEL;
        g->status = ST_CREATOR;
        g->member_count = 1;
        HASH_ADD(hh, b->supers, group_id, sizeof(g->group_id), g);
    }
    arr->id = cid;
    arr->type = type;
    arr->can_write = 1;
    arr->members = g->member_count > 0 ? g->member_count : 1;
    arr->unread = c->unread;
    arr->archived = c->archived;
    pthread_mutex_unlock(&b->mtx);
    st = tg_get_chat_muted(b, cid, &arr->muted);
    if (st != TG_OK) {
        /* Чат уже создан: ошибка чтения настроек не отменяет создание. */
        st = TG_ERR_NET;
        goto fail;
    }
    *out = arr;
    arr = NULL;
    st = TG_OK;

fail:
    cJSON_Delete(r);
    cJSON_Delete(resp);
    tg_free_chats(arr, 1);
    if (reserved_chat) {
        free(reserved_chat->title);
        free(reserved_chat);
    }
    free(reserved_super);
    return st;
}

/* Не используем снимок tg_chat: TDLib сама учитывает входящие обновления
 * настроек и истечение mute_for. Возвращаемый JSON принадлежит вызывающему. */
static tg_status get_notification_chat(tg_backend *b, int64_t chat_id,
                                       cJSON **out)
{
    cJSON *r;
    tg_status st;
    int ready;
    const char *type;

    *out = NULL;
    pthread_mutex_lock(&b->mtx);
    ready = b->auth == AUTH_READY;
    pthread_mutex_unlock(&b->mtx);
    if (!ready)
        return TG_ERR_AUTH;

    r = new_req("getChat");
    if (!r || !cJSON_AddNumberToObject(r, "chat_id", (double)chat_id)) {
        cJSON_Delete(r);
        return TG_ERR_OTHER;
    }
    st = request_checked(b, r, out, REQ_TIMEOUT_SEC, NULL);
    if (st != TG_OK)
        return st;
    type = j_type(j_obj(*out, "type"));
    if (strcmp(j_type(*out), "chat") != 0 ||
        (strcmp(type, "chatTypePrivate") != 0 &&
         strcmp(type, "chatTypeBasicGroup") != 0 &&
         strcmp(type, "chatTypeSupergroup") != 0) ||
        strcmp(j_type(j_obj(*out, "notification_settings")),
               "chatNotificationSettings") != 0) {
        cJSON_Delete(*out);
        *out = NULL;
        return TG_ERR_OTHER;
    }
    return TG_OK;
}

tg_status tg_set_chat_muted(tg_backend *b, int64_t chat_id, int muted)
{
    cJSON *chat = NULL, *settings, *r, *resp = NULL;
    tg_status st;
    int64_t self_id;

    if (!b || chat_id == 0 || (muted != 0 && muted != 1))
        return TG_ERR_OTHER;
    st = get_notification_chat(b, chat_id, &chat);
    if (st != TG_OK)
        return st;

    /* getMe при необходимости; не принимаем неизвестный id за чужой чат. */
    if (strcmp(j_type(j_obj(chat, "type")), "chatTypePrivate") == 0) {
        self_id = get_my_id(b);
        if (self_id == 0 || j_i64(j_obj(chat, "type"), "user_id") == self_id) {
            cJSON_Delete(chat);
            return self_id == 0 ? TG_ERR_NET : TG_ERR_PERM;
        }
    }

    settings = cJSON_Duplicate(j_obj(chat, "notification_settings"), 1);
    cJSON_Delete(chat);
    if (!settings)
        return TG_ERR_OTHER;
    /* Удаление + добавление также работает, если поле отсутствовало. */
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "use_default_mute_for");
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "mute_for");
    if (!cJSON_AddBoolToObject(settings, "use_default_mute_for", 0) ||
        !cJSON_AddNumberToObject(settings, "mute_for", muted ? INT32_MAX : 0)) {
        cJSON_Delete(settings);
        return TG_ERR_OTHER;
    }
    r = new_req("setChatNotificationSettings");
    if (!r || !cJSON_AddNumberToObject(r, "chat_id", (double)chat_id) ||
        !cJSON_AddItemToObject(r, "notification_settings", settings)) {
        cJSON_Delete(settings);
        cJSON_Delete(r);
        return TG_ERR_OTHER;
    }
    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    if (st == TG_OK && strcmp(j_type(resp), "ok") != 0)
        st = TG_ERR_OTHER;
    cJSON_Delete(resp);
    return st;
}

tg_status tg_get_chat_muted(tg_backend *b, int64_t chat_id, int *out_muted)
{
    cJSON *chat = NULL, *r, *scope, *resp = NULL;
    const cJSON *settings, *type;
    const char *scope_type;
    tg_status st;

    if (out_muted)
        *out_muted = 0;
    if (!b || chat_id == 0 || !out_muted)
        return TG_ERR_OTHER;
    st = get_notification_chat(b, chat_id, &chat);
    if (st != TG_OK)
        return st;
    settings = j_obj(chat, "notification_settings");
    if (!j_bool(settings, "use_default_mute_for")) {
        *out_muted = j_i32(settings, "mute_for") > 0;
        cJSON_Delete(chat);
        return TG_OK;
    }
    type = j_obj(chat, "type");
    if (strcmp(j_type(type), "chatTypePrivate") == 0)
        scope_type = "notificationSettingsScopePrivateChats";
    else if (strcmp(j_type(type), "chatTypeSupergroup") == 0 &&
             j_bool(type, "is_channel"))
        scope_type = "notificationSettingsScopeChannelChats";
    else
        scope_type = "notificationSettingsScopeGroupChats";
    cJSON_Delete(chat);

    r = new_req("getScopeNotificationSettings");
    scope = new_req(scope_type);
    if (!r || !scope || !cJSON_AddItemToObject(r, "scope", scope)) {
        cJSON_Delete(scope);
        cJSON_Delete(r);
        return TG_ERR_OTHER;
    }
    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    if (st == TG_OK) {
        if (strcmp(j_type(resp), "scopeNotificationSettings") != 0)
            st = TG_ERR_OTHER;
        else
            *out_muted = j_i32(resp, "mute_for") > 0;
    }
    cJSON_Delete(resp);
    return st;
}

tg_status tg_archive_chat(tg_backend *b, int64_t chat_id, int archived)
{
    cJSON *r, *list, *resp = NULL;
    chat_rec *c;
    int supported;
    tg_status st;

    if (!b || chat_id == 0 || (archived != 0 && archived != 1))
        return TG_ERR_OTHER;

    st = ensure_chat(b, chat_id);
    if (st != TG_OK)
        return st;

    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &chat_id, sizeof(chat_id), c);
    supported = c && c->kind != CK_SECRET;
    pthread_mutex_unlock(&b->mtx);
    if (!supported)
        return TG_ERR_OTHER;

    r = new_req("addChatToList");
    list = new_req(archived ? "chatListArchive" : "chatListMain");
    if (!r || !list) {
        cJSON_Delete(r);
        cJSON_Delete(list);
        return TG_ERR_OTHER;
    }
    if (!cJSON_AddNumberToObject(r, "chat_id", (double)chat_id) ||
        !cJSON_AddItemToObject(r, "chat_list", list)) {
        cJSON_Delete(list);
        cJSON_Delete(r);
        return TG_ERR_OTHER;
    }

    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    if (st == TG_OK && strcmp(j_type(resp), "ok") != 0)
        st = TG_ERR_OTHER;
    cJSON_Delete(resp);
    if (st != TG_OK)
        return st;

    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &chat_id, sizeof(chat_id), c);
    if (c)
        c->archived = archived;
    pthread_mutex_unlock(&b->mtx);
    return TG_OK;
}

tg_status tg_rename_chat(tg_backend *b, int64_t chat_id, const char *new_title)
{
    cJSON *r, *resp = NULL;
    chat_rec *c;
    char *title;
    int supported;
    tg_status st;

    if (!b || !new_title || !*new_title || chat_id == 0)
        return TG_ERR_OTHER;

    st = ensure_chat(b, chat_id);
    if (st != TG_OK)
        return st;

    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &chat_id, sizeof(chat_id), c);
    supported = c && (c->kind == CK_BASIC || c->kind == CK_SUPER);
    pthread_mutex_unlock(&b->mtx);
    if (!supported)
        return TG_ERR_OTHER;

    /* Резервируем память до запроса: после успеха сервера кеш обновим без malloc. */
    title = xstrdup(new_title);
    if (!title)
        return TG_ERR_OTHER;
    r = new_req("setChatTitle");
    if (!r) {
        free(title);
        return TG_ERR_OTHER;
    }
    if (!cJSON_AddNumberToObject(r, "chat_id", (double)chat_id) ||
        !cJSON_AddStringToObject(r, "title", new_title)) {
        cJSON_Delete(r);
        free(title);
        return TG_ERR_OTHER;
    }

    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    if (st == TG_OK && strcmp(j_type(resp), "ok") != 0)
        st = TG_ERR_OTHER;
    cJSON_Delete(resp);
    if (st != TG_OK) {
        free(title);
        return st;
    }

    /* updateChatTitle тоже обновляет кеш; здесь не зависим от времени апдейта. */
    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &chat_id, sizeof(chat_id), c);
    if (c) {
        free(c->title);
        c->title = title;
        title = NULL;
    }
    pthread_mutex_unlock(&b->mtx);
    free(title);
    return TG_OK;
}

/* ------------------------------------------------------------------ */
/* участники группы                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t        user_id;
    tg_member_role role;
} raw_member;

static tg_member_role role_of(member_status s)
{
    switch (s) {
    case ST_CREATOR:    return TG_ROLE_CREATOR;
    case ST_ADMIN:      return TG_ROLE_ADMIN;
    case ST_RESTRICTED: return TG_ROLE_RESTRICTED;
    default:            return TG_ROLE_MEMBER;
    }
}

/*
 * Разбирает массив chatMember в buf (не больше cap, без дублей).
 * Возвращает число элементов в JSON-массиве (для пагинации).
 */
static int collect_members(const cJSON *arr, raw_member *buf, size_t cap, size_t *cnt)
{
    const cJSON *it;

    if (!cJSON_IsArray(arr))
        return 0;

    cJSON_ArrayForEach(it, arr) {
        const cJSON *mid = j_obj(it, "member_id");
        member_status s;
        int can_send, acp, dup = 0;
        int64_t uid;

        if (*cnt == cap)
            break;
        if (!mid || strcmp(j_type(mid), "messageSenderUser") != 0)
            continue;                         /* анонимный админ / канал */
        s = parse_status(j_obj(it, "status"), &can_send, &acp);
        if (s == ST_LEFT || s == ST_BANNED)
            continue;
        uid = j_i64(mid, "user_id");
        if (uid == 0)
            continue;
        for (size_t k = 0; k < *cnt; k++) {
            if (buf[k].user_id == uid) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        buf[*cnt].user_id = uid;
        buf[*cnt].role    = role_of(s);
        (*cnt)++;
    }
    return cJSON_GetArraySize(arr);
}

/* Старые TDLib возвращают ok, новые -- failedToAddMembers. */
static tg_status add_member_result(const cJSON *resp)
{
    const cJSON *failed;

    if (strcmp(j_type(resp), "ok") == 0)
        return TG_OK;
    if (strcmp(j_type(resp), "failedToAddMembers") != 0)
        return TG_ERR_OTHER;
    failed = cJSON_GetObjectItemCaseSensitive(resp, "failed_to_add_members");
    if (!cJSON_IsArray(failed))
        return TG_ERR_OTHER;
    return cJSON_GetArraySize(failed) == 0 ? TG_OK : TG_ERR_PERM;
}

tg_status tg_add_member(tg_backend *b, int64_t chat_id, int64_t user_id)
{
    /* Идентификаторы TDLib имеют тип int53: JSON double хранит их точно. */
    const int64_t max_id = INT64_C(9007199254740991);
    chat_rec *c;
    cJSON *r = NULL, *resp = NULL;
    int ready, supported;
    tg_status st;

    if (!b || chat_id == 0 || chat_id < -max_id || chat_id > max_id ||
        user_id <= 0 || user_id > max_id)
        return TG_ERR_OTHER;
    pthread_mutex_lock(&b->mtx);
    ready = b->auth == AUTH_READY && !b->stop;
    pthread_mutex_unlock(&b->mtx);
    if (!ready)
        return TG_ERR_AUTH;

    st = ensure_chat(b, chat_id);
    if (st != TG_OK)
        return st;
    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &chat_id, sizeof(chat_id), c);
    supported = c && (c->kind == CK_BASIC ||
                      (c->kind == CK_SUPER && !c->is_channel));
    pthread_mutex_unlock(&b->mtx);
    if (!supported)
        return TG_ERR_OTHER;

    r = new_req("addChatMember");
    if (!r ||
        !cJSON_AddNumberToObject(r, "chat_id", (double)chat_id) ||
        !cJSON_AddNumberToObject(r, "user_id", (double)user_id) ||
        !cJSON_AddNumberToObject(r, "forward_limit", 0)) {
        cJSON_Delete(r);
        return TG_ERR_OTHER;
    }
    /* request_checked принимает владение r и обрабатывает error/таймаут. */
    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    if (st == TG_OK)
        st = add_member_result(resp);
    cJSON_Delete(resp);

    /* Не увеличиваем member_count вручную: обновление могло уже прийти. */
    return st;
}

tg_status tg_remove_member(tg_backend *b, int64_t chat_id, int64_t user_id)
{
    chat_rec *c;
    cJSON *r = NULL, *member = NULL, *status = NULL, *resp = NULL;
    int ready, supported;
    int64_t self_id;
    tg_status st;
    const char *member_status_type;

    if (!b || chat_id == 0 || user_id <= 0)
        return TG_ERR_OTHER;
    pthread_mutex_lock(&b->mtx);
    ready = b->auth == AUTH_READY && !b->stop;
    pthread_mutex_unlock(&b->mtx);
    if (!ready)
        return TG_ERR_AUTH;
    st = ensure_chat(b, chat_id);
    if (st != TG_OK)
        return st;
    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &chat_id, sizeof(chat_id), c);
    supported = c && (c->kind == CK_BASIC ||
                      (c->kind == CK_SUPER && !c->is_channel));
    pthread_mutex_unlock(&b->mtx);
    if (!supported)
        return TG_ERR_OTHER;

    self_id = get_my_id(b);
    if (!self_id)
        return TG_ERR_NET;
    if (user_id == self_id)
        return TG_ERR_OTHER;

    r = new_req("getChatMember");
    member = new_req("messageSenderUser");
    st = TG_ERR_OTHER;
    if (!r || !member ||
        !cJSON_AddNumberToObject(r, "chat_id", (double)chat_id) ||
        !cJSON_AddNumberToObject(member, "user_id", (double)user_id) ||
        !cJSON_AddItemToObject(r, "member_id", member))
        goto done;
    member = NULL; /* теперь принадлежит r */
    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    r = NULL;
    if (st != TG_OK)
        goto done;
    st = TG_ERR_OTHER;
    if (strcmp(j_type(resp), "chatMember") != 0 ||
        strcmp(j_type(j_obj(resp, "member_id")), "messageSenderUser") != 0 ||
        j_i64(j_obj(resp, "member_id"), "user_id") != user_id)
        goto done;
    member_status_type = j_type(j_obj(resp, "status"));
    if (strcmp(member_status_type, "chatMemberStatusCreator") == 0 ||
        strcmp(member_status_type, "chatMemberStatusAdministrator") == 0) {
        st = TG_ERR_PERM;
        goto done;
    }
    /* Не превращаем бан в Left: это разблокировало бы пользователя. */
    if (strcmp(member_status_type, "chatMemberStatusMember") != 0 &&
        !(strcmp(member_status_type, "chatMemberStatusRestricted") == 0 &&
          j_bool(j_obj(resp, "status"), "is_member")))
        goto done;
    cJSON_Delete(resp);
    resp = NULL;

    r = new_req("setChatMemberStatus");
    member = new_req("messageSenderUser");
    status = new_req("chatMemberStatusLeft");
    if (!r || !member || !status ||
        !cJSON_AddNumberToObject(r, "chat_id", (double)chat_id) ||
        !cJSON_AddNumberToObject(member, "user_id", (double)user_id) ||
        !cJSON_AddItemToObject(r, "member_id", member))
        goto done;
    member = NULL;
    if (!cJSON_AddItemToObject(r, "status", status))
        goto done;
    status = NULL;
    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    r = NULL;
    if (st == TG_OK && strcmp(j_type(resp), "ok") != 0)
        st = TG_ERR_OTHER;

    /* Счётчики участников обновляет TDLib, не уменьшаем их вручную:
     * updateSupergroup мог прийти ещё до ответа и уже учесть удаление. */
done:
    cJSON_Delete(r);
    cJSON_Delete(member);
    cJSON_Delete(status);
    cJSON_Delete(resp);
    return st;
}

tg_status tg_list_members(tg_backend *b, int64_t chat_id, int limit,
                          tg_member **out, size_t *n)
{
    chat_rec *c;
    chat_kind kind;
    int64_t type_id, me;
    int is_channel;
    raw_member *raw;
    size_t cnt = 0;
    tg_member *arr;
    tg_status st;

    if (!b || !out || !n || limit <= 0)
        return TG_ERR_OTHER;
    *out = NULL;
    *n   = 0;

    st = ensure_chat(b, chat_id);
    if (st != TG_OK)
        return st;

    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &chat_id, sizeof(chat_id), c);
    if (!c) {
        pthread_mutex_unlock(&b->mtx);
        return TG_ERR_OTHER;
    }
    kind       = c->kind;
    type_id    = c->type_id;
    is_channel = c->is_channel;
    pthread_mutex_unlock(&b->mtx);

    if (kind == CK_SUPER && is_channel)
        return TG_ERR_OTHER;                 /* канал -- не группа */
    if (kind != CK_BASIC && kind != CK_SUPER)
        return TG_ERR_OTHER;

    raw = calloc((size_t)limit, sizeof(*raw));
    if (!raw)
        return TG_ERR_OTHER;

    if (kind == CK_BASIC) {
        cJSON *r = new_req("getBasicGroupFullInfo");
        cJSON *resp = NULL;

        if (!r) {
            free(raw);
            return TG_ERR_OTHER;
        }
        add_i64(r, "basic_group_id", type_id);
        st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
        if (st == TG_OK && resp)
            collect_members(cJSON_GetObjectItemCaseSensitive(resp, "members"),
                            raw, (size_t)limit, &cnt);
        cJSON_Delete(resp);
    } else {
        int offset = 0;

        while (cnt < (size_t)limit) {
            cJSON *r = new_req("getSupergroupMembers");
            cJSON *filter = new_req("supergroupMembersFilterRecent");
            cJSON *resp = NULL;
            int want = (int)((size_t)limit - cnt);
            int page, total;

            if (!r || !filter) {
                cJSON_Delete(r);
                cJSON_Delete(filter);
                st = TG_ERR_OTHER;
                break;
            }
            if (want > MEMBERS_BATCH)
                want = MEMBERS_BATCH;

            add_i64(r, "supergroup_id", type_id);
            cJSON_AddItemToObject(r, "filter", filter);
            cJSON_AddNumberToObject(r, "offset", offset);
            cJSON_AddNumberToObject(r, "limit", want);

            st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
            if (st != TG_OK)
                break;

            page  = collect_members(cJSON_GetObjectItemCaseSensitive(resp, "members"),
                                    raw, (size_t)limit, &cnt);
            total = j_i32(resp, "total_count");
            cJSON_Delete(resp);

            if (page == 0)
                break;
            offset += page;
            if (total > 0 && offset >= total)
                break;
        }
    }

    /* частичный результат лучше, чем никакого -- как в tg_get_messages */
    if (st != TG_OK && cnt == 0) {
        free(raw);
        return st;
    }
    if (cnt == 0) {
        free(raw);
        return TG_OK;
    }

    arr = calloc(cnt, sizeof(*arr));
    if (!arr) {
        free(raw);
        return TG_ERR_OTHER;
    }

    me = get_my_id(b);
    for (size_t i = 0; i < cnt; i++) {
        tg_member *m = &arr[i];

        m->user_id = raw[i].user_id;
        m->role    = raw[i].role;
        m->is_self = (me != 0 && m->user_id == me);

        (void)user_info(b, m->user_id, &m->name, &m->username, &m->is_bot, NULL);
        if (!m->name) {
            char buf[64];

            snprintf(buf, sizeof(buf), "user_%" PRId64, m->user_id);
            m->name = xstrdup(buf);
        }
        if (!m->name) {
            free(raw);
            tg_free_members(arr, i + 1);
            return TG_ERR_OTHER;
        }
    }
    free(raw);

    *out = arr;
    *n   = cnt;
    return TG_OK;
}

tg_status tg_open_username_chat(tg_backend *b, const char *username,
                                tg_chat **out)
{
    cJSON *r, *resp = NULL;
    const cJSON *type;
    tg_chat *chat = NULL;
    int64_t user_id;
    int ready;
    tg_status st;

    if (!out)
        return TG_ERR_OTHER;
    *out = NULL;
    if (!b || !username)
        return TG_ERR_OTHER;
    if (*username == '@')
        username++;
    if (!*username)
        return TG_ERR_OTHER;
    for (const char *p = username; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
            return TG_ERR_OTHER;
    }
    pthread_mutex_lock(&b->mtx);
    ready = b->auth == AUTH_READY && !b->stop;
    pthread_mutex_unlock(&b->mtx);
    if (!ready)
        return TG_ERR_AUTH;

    r = new_req("searchPublicChat");
    if (!r || !cJSON_AddStringToObject(r, "username", username)) {
        cJSON_Delete(r);
        return TG_ERR_OTHER;
    }
    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    if (st != TG_OK)
        return st;
    type = j_obj(resp, "type");
    user_id = j_i64(type, "user_id");
    if (strcmp(j_type(resp), "chat") != 0 ||
        strcmp(j_type(type), "chatTypePrivate") != 0 || user_id <= 0 ||
        j_i64(resp, "id") == 0) {
        cJSON_Delete(resp);
        return TG_ERR_OTHER;
    }
    cJSON_Delete(resp);

    st = tg_open_user_chat(b, user_id, &chat);
    if (st != TG_OK)
        return st;
    /* Сохраняем диалог в основном списке, в том числе после перезапуска. */
    st = tg_archive_chat(b, chat->id, 0);
    if (st != TG_OK) {
        tg_free_chats(chat, 1);
        return st;
    }
    chat->archived = 0;
    *out = chat;
    return TG_OK;
}

tg_status tg_open_user_chat(tg_backend *b, int64_t user_id, tg_chat **out)
{
    cJSON *r, *resp = NULL;
    chat_rec *c;
    tg_chat *arr;
    int64_t cid, dummy;
    tg_status st;
    int rc;

    if (!b || !out || user_id <= 0)
        return TG_ERR_OTHER;
    *out = NULL;

    /* fill_chat берёт имя и признак "удалён" из таблицы -- подгружаем */
    (void)user_info(b, user_id, NULL, NULL, NULL, NULL);

    r = new_req("createPrivateChat");
    if (!r)
        return TG_ERR_OTHER;
    add_i64(r, "user_id", user_id);
    cJSON_AddBoolToObject(r, "force", 0);

    st = request_checked(b, r, &resp, REQ_TIMEOUT_SEC, NULL);
    if (st != TG_OK)
        return st;

    /* ответ -- объект chat, тот же, что в updateNewChat */
    cid = j_i64(resp, "id");
    pthread_mutex_lock(&b->mtx);
    upsert_chat(b, resp);
    pthread_mutex_unlock(&b->mtx);
    cJSON_Delete(resp);
    if (cid == 0)
        return TG_ERR_OTHER;

    arr = calloc(1, sizeof(*arr));
    if (!arr)
        return TG_ERR_OTHER;

    pthread_mutex_lock(&b->mtx);
    HASH_FIND(hh, b->chats, &cid, sizeof(cid), c);
    rc = c ? fill_chat(b, c, arr, &dummy) : -1;
    pthread_mutex_unlock(&b->mtx);

    if (rc != 0) {
        tg_free_chats(arr, 1);
        return TG_ERR_OTHER;
    }

    st = tg_get_chat_muted(b, cid, &arr->muted);
    if (st != TG_OK) {
        tg_free_chats(arr, 1);
        return st;
    }

    *out = arr;
    return TG_OK;
}

/* ------------------------------------------------------------------ */
/* освобождение                                                        */
/* ------------------------------------------------------------------ */

void tg_free_chats(tg_chat *c, size_t n)
{
    if (!c)
        return;
    for (size_t i = 0; i < n; i++) {
        free(c[i].title);
        free(c[i].username);
    }
    free(c);
}

void tg_free_messages(tg_message *m, size_t n)
{
    if (!m)
        return;
    for (size_t i = 0; i < n; i++) {
        free(m[i].sender);
        free(m[i].text);
    }
    free(m);
}

void tg_free_members(tg_member *m, size_t n)
{
    if (!m)
        return;
    for (size_t i = 0; i < n; i++) {
        free(m[i].name);
        free(m[i].username);
    }
    free(m);
}
