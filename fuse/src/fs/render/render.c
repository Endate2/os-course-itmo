#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *type_name(tg_chat_type type) {
    switch (type) {
        case TG_GROUP: return "group";
        case TG_USER: return "user";
        case TG_CHANNEL: return "channel";
        default: return "unknown";
    }
}

int render_messages(const tg_message *messages, size_t n, char **out, size_t *len) {
    char *buf = NULL;
    size_t buf_len = 0;
    FILE *f = open_memstream(&buf, &buf_len);

    if (!f) {
        return -1;
    }

    if (n == 0) {
        fprintf(f, "нет сообщений\n");
    }

    for (size_t i = n; i > 0 ; i--) {
        const tg_message *m = &messages[i-1];

        time_t t = (time_t)m->date;
        struct tm tm;
        char date[32] = "????-??-?? ??:??";
        if (localtime_r(&t, &tm)) {
            strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm);
        }

        fprintf(f, "[%s] %s: %s \n", date, m->sender, m->text);
    }

    if (fclose(f) != 0) {
        free(buf);
        return -1;
    }

    *out = buf;
    *len = buf_len;
    return 0;
}

int render_info(const tg_chat * chat, char **out, size_t *len) {
    char *buf = NULL;
    size_t buf_len = 0;
    FILE *f = open_memstream(&buf, &buf_len);

    if (!f) {
        return -1;
    }

    fprintf(f, "title: %s\n", chat->title);
    fprintf(f, "type: %s\n", type_name(chat->type));

    if (chat->username) {
        fprintf(f, "username: %s\n", chat->username);
    }else {
        fprintf(f, "username: none\n");
    }

    if (chat->members < 0) {
        fprintf(f, "members: 0\n");
    }else {
        fprintf(f, "members: %d\n", chat->members);
    }

    if (chat->type == TG_USER) {
        fprintf(f, "user_id: %d\n", chat->user_id);
    }

    fprintf(f, "unread: %d\n", chat->unread);
    fprintf(f, "writable: %s\n", chat->can_write ? "yes" : "no");

    if (fclose(f) != 0) {
        free(buf);
        return -1;
    }

    *out = buf;
    *len = buf_len;
    return 0;
}