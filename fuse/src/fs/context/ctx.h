#include "../../backend/tg_backend.h"
#include <fuse_lowlevel.h>


typedef struct ctx {
    tg_backend *backend;
    tg_chat *chat;
    size_t n_chats;
    int limit;
    time_t chats_updated;
} context;

tg_status ctx_refresh_chats(context * ctx);
void ctx_free(context * ctx);
tg_chat *ctx_get_chat(context * ctx, int64_t chat_id);