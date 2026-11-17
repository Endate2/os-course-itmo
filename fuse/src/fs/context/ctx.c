#include "ctx.h"
#include <time.h>

tg_status ctx_refresh_chats(context * ctx) {
    tg_chat *chat;
    size_t n_chats;
    tg_status status = tg_list_chats(ctx->backend, &chat, &n_chats);

    if (status != TG_OK) return status;

    tg_free_chats(ctx->chat, ctx->n_chats);
    ctx->n_chats = n_chats;
    ctx->chat = chat;
    ctx->chats_updated = time(NULL);

    return TG_OK;
}

void ctx_free(context * ctx) {
    tg_free_chats(ctx->chat, ctx->n_chats);
    ctx->chat = NULL;
    ctx->n_chats = 0;
    ctx->chats_updated = 0;
}

tg_chat *ctx_get_chat(context * ctx, int64_t chat_id) {
    for (size_t i = 0; i < ctx->n_chats; i++) {
        if (ctx->chat[i].id == chat_id) {
            return &ctx->chat[i];
        }
    }
    return NULL;
}