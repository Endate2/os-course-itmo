#define _GNU_SOURCE

#include "ops.h"
#include "context/ctx.h"
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include "inode/inode.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "render/render.h"

static const char *xattr_names[] = { "user.tg.unread", "user.tg.writable", "user.chat.notifications", "user.id" };


struct buf_file {
    char *p;
    int descriptor;
    size_t size;
    type_of_work type;
    size_t cap;
    int64_t chat_id;
    node_kind kind;
};


static int status_to_errno(tg_status st) {
    switch (st) {
        case TG_OK: return 0;
        case TG_ERR_PERM: return EACCES;
        case TG_ERR_AUTH: return EACCES;
        case TG_ERR_NET: return EIO;
        case TG_ERR_FLOOD: return EAGAIN;
        case TG_ERR_OTHER:
        default:
            return EIO;
    }
}


static int generate_messages(context *ctx, node *n_ode, char **out, size_t *len, time_t *mtime) {
    tg_chat *chat = NULL;
    if (n_ode -> key.user_id != 0) {
        chat = ctx_get_chat(ctx, n_ode->key.user_id);
    }else {
        chat = ctx_get_chat(ctx, n_ode->key.chat_id);
    }
    if (!chat) {
        return ENOENT;
    }

    if (n_ode->kind == N_MESSAGES) {
        tg_message *messages = NULL;
        size_t n_messages = 0;
        tg_status status =  tg_get_messages(ctx->backend, chat->id, ctx->limit, &messages, &n_messages);

        if (status != TG_OK) {
            return status_to_errno(status);
        }

        if (mtime && n_messages > 0) {
            *mtime = (time_t)messages[0].date ;
        }
        int res = render_messages(messages, n_messages, out, len);
        tg_free_messages(messages, n_messages);

        if (res == -1) {
            return ENOMEM;
        }
    }else {
        if (mtime) {
            *mtime = ctx->chats_updated;
        }
        if (render_info(chat, out, len) != 0) {
            return ENOMEM;
        }
    }
    return 0;
}


static int generate_message(context *ctx, node *n_ode, char **out, size_t *len, time_t *mtime) {
    tg_chat *chat = NULL;
    if (n_ode -> key.user_id != 0) {
        chat = ctx_get_chat(ctx, n_ode->key.user_id);
    }else {
        chat = ctx_get_chat(ctx, n_ode->key.chat_id);
    }
    if (!chat) {
        return ENOENT;
    }

    tg_message *msgs = NULL;
    size_t n = 0;
    tg_status st = tg_get_messages(ctx->backend, chat->id, ctx->limit, &msgs, &n);
    if (st != TG_OK) {
        return status_to_errno(st);
    }

    int res = ENOENT;
    for (size_t i = 0; i < n; i++) {
        if (msgs[i].id != n_ode->key.message_id) {
            continue;
        }
        if (mtime) {
            *mtime = (time_t)msgs[i].date ;
        }
        res = render_messages(&msgs[i], 1, out, len);
        break;
    }

    tg_free_messages(msgs, n);
    return res;
}


static int dirbuf_add(fuse_req_t req, struct buf_file *buf, const char *name,
                      fuse_ino_t ino, mode_t mode) {
    size_t old = buf->size;
    size_t entry = fuse_add_direntry(req, NULL, 0, name, NULL, 0);
    size_t new_size = old + entry;

    char *newp = realloc(buf->p, new_size);
    if (!newp)
        return -1;

    buf->p = newp;
    buf->size = new_size;

    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = ino;
    st.st_mode = mode;

    fuse_add_direntry(req, buf->p + old, buf->size - old, name, &st, buf->size);

    return 0;
}


static void reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize, off_t off, size_t maxsize) {
    if ((size_t)off < bufsize) {
        size_t len = bufsize - off;
        if (len > maxsize) {
            len = maxsize;
        }
        fuse_reply_buf(req, buf + off, len);
    }else {
        fuse_reply_buf(req, NULL, 0);
    }
}


static void tg_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
    (void) fi;
    context *ctx = fuse_req_userdata(req);
    node* n_ode= inode_get(ino);
    if (!n_ode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (n_ode->kind != N_ROOT && n_ode->kind != N_CATEGORY_OF_CHATS &&
    n_ode->kind != N_NAME_OF_CHAT && n_ode->kind != N_MEMBERS && n_ode->kind != N_MEMBER &&
    n_ode->kind != N_MESSAGES_FILES && n_ode->kind != N_MEDIA) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    struct buf_file buf = {0};
    switch (n_ode->kind) {
        case N_ROOT:
            for (size_t i = 0; i < TG_CHAT_TYPE_COUNT; i++) {
                if (dirbuf_add(req, &buf, inode_category_name(i), i+2, S_IFDIR) != 0) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                };
            }
            break;
        case N_CATEGORY_OF_CHATS: {
            ctx_refresh_chats(ctx);
            for (size_t i = 0; i < ctx->n_chats; i++) {
                if (n_ode->category == TG_ARCHIVE) {
                    if (!ctx->chat[i].archived) {
                        continue;
                    }
                } else {
                    if (ctx->chat[i].archived ||
                        n_ode->category != ctx->chat[i].type) {
                        continue;
                        }
                }
                char* name = inode_chat_name(ctx->chat, i);
                if (!name) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    break;
                }
                node* child_node = inode_get_or_create(n_ode, N_NAME_OF_CHAT, ctx -> chat[i].id);
                child_node->category = ctx->chat[i].type;
                if (dirbuf_add(req, &buf, name, child_node->ino, S_IFDIR) != 0) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                };

                free(name);
            }
            if (n_ode -> category == TG_USER) {
                if (dirbuf_add(req, &buf, "add", 6, S_IFREG) != 0) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                }
            }
            break;
        }
        case N_NAME_OF_CHAT: {
            static const struct { const char *name; node_kind kind; } files[] = {
                {"messages", N_MESSAGES},{"send",N_SEND},{"info", N_INFO},{"messages_files", N_MESSAGES_FILES}, {"media", N_MEDIA},{"members", N_MEMBERS},
            };
            for (size_t i = 0; i < 5; i++) {
                node *child = inode_get_or_create(n_ode, files[i].kind, n_ode->key.chat_id);
                if (files[i].kind == N_MESSAGES_FILES || files[i].kind == N_MEDIA) {
                    if (dirbuf_add(req, &buf, files[i].name, child->ino, S_IFDIR) != 0) {
                        free(buf.p);
                        fuse_reply_err(req, ENOMEM);
                        return;
                    };
                    continue;
                }
                if (dirbuf_add(req, &buf, files[i].name, child->ino, S_IFREG) != 0) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                };
            }
            if (n_ode -> category == TG_GROUP) {
                node *child = inode_get_or_create(n_ode, N_MEMBERS, n_ode->key.chat_id);
                if (dirbuf_add(req, &buf, "members", child->ino, S_IFDIR) != 0) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                };
            }
            break;
        }
        case N_MEMBERS: {
            tg_member *members = NULL;
            size_t n_members = 0;

            tg_status status = tg_list_members(ctx->backend, n_ode->key.chat_id, ctx->limit, &members, &n_members);
            if (status != TG_OK) {
                free(members);
                fuse_reply_err(req, status_to_errno(status));
                return;
            }
            for (size_t i = 0; i < n_members; i++) {
                char *name = inode_member_name(members, i);
                if (!name) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    break;
                }
                node *child_node = inode_get_or_create(n_ode, N_MEMBER, members[i].user_id);
                if (dirbuf_add(req, &buf, name, child_node->ino, S_IFDIR) != 0) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                };

                free(name);
            }
            tg_free_members(members, n_members);
            break;
        }
        case N_MEMBER: {
            static const struct { const char *name; node_kind kind; } mfiles[] = {
                {"messages", N_MESSAGES},{"send",N_SEND},{"info", N_INFO} ,{"messages_files", N_MESSAGES_FILES}, {"media", N_MEDIA}
            };
            for (size_t i = 0; i < 5; i++) {
                node *child = inode_get_or_create(n_ode, mfiles[i].kind, 0);
                if (mfiles[i].kind == N_MESSAGES_FILES || mfiles[i].kind == N_MEDIA) {
                    if (dirbuf_add(req, &buf, mfiles[i].name, child->ino, S_IFDIR) != 0) {
                        free(buf.p);
                        fuse_reply_err(req, ENOMEM);
                        return;
                    };
                    continue;
                }
                if (dirbuf_add(req, &buf, mfiles[i].name, child->ino, S_IFREG) != 0) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                };
            }
            break;
        }
        case N_MESSAGES_FILES: {
            tg_message *messages = NULL;
            size_t n_messages = 0;
            int64_t target_chat_id = n_ode->key.chat_id;
            if (n_ode -> key.user_id != 0) {
                tg_chat *chat = NULL;
                tg_status status =  tg_open_user_chat(ctx->backend, n_ode->key.user_id, &chat);

                if (status != TG_OK) {
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                }

                target_chat_id = chat->id;
                tg_free_chats(chat, 1);
            }
            tg_status status = tg_get_messages(ctx->backend, target_chat_id, ctx->limit, &messages, &n_messages);
            if (status != TG_OK) {
                tg_free_messages(messages, n_messages);
                free(buf.p);
                fuse_reply_err(req, ENOMEM);
                return;
            }

            for (size_t i = 0; i < n_messages; i++) {
                char *name = inode_message_name(&messages[i]);
                if (!name) {
                    tg_free_messages(messages, n_messages);
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                }

                node *child = inode_get_or_create(n_ode, N_MESSAGE, messages[i].id);
                if (dirbuf_add(req, &buf, name, child->ino, S_IFREG) != 0) {
                    tg_free_messages(messages, n_messages);
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                };
                free(name);
            }
            tg_free_messages(messages, n_messages);
            break;
        }
        case N_MEDIA: {
            tg_photo *photos = NULL;
            size_t n_photos = 0;
            int64_t target_chat_id = n_ode->key.chat_id;
            if (n_ode -> key.user_id != 0) {
                tg_chat *chat = NULL;
                tg_status status =  tg_open_user_chat(ctx->backend, n_ode->key.user_id, &chat);

                if (status != TG_OK) {
                    free(buf.p);
                    fuse_reply_err(req, status_to_errno(status));
                    return;
                }

                target_chat_id = chat->id;
                tg_free_chats(chat, 1);
            }
            tg_status status = tg_list_photos(ctx->backend, target_chat_id, ctx->limit, &photos, &n_photos);
            if (status != TG_OK) {
                tg_free_photos(photos, n_photos);
                free(buf.p);
                fuse_reply_err(req, status_to_errno(status));
                return;
            }

            for (size_t i = 0; i < n_photos; i++) {
                char *name = inode_photo_name(&photos[i]);
                if (!name) {
                    tg_free_photos(photos, n_photos);
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                }

                node *child = inode_get_or_create(n_ode, N_MEDIA_FILE, photos[i].message_id);
                if (dirbuf_add(req, &buf, name, child->ino, S_IFREG) != 0) {
                    tg_free_photos(photos, n_photos);
                    free(buf.p);
                    fuse_reply_err(req, ENOMEM);
                    return;
                };
                free(name);
            }
            tg_free_photos(photos, n_photos);
            break;
        }
        default:
            break;
    }

    reply_buf_limited(req, buf.p, buf.size, off, size);
    free(buf.p);
}


static void tg_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void)fi;
    context *ctx = fuse_req_userdata(req);
    node* n_ode= inode_get(ino);
    char *text = NULL;
    time_t mtime = 0;
    size_t text_len = 0;
    int can_write = 0;

    if (!n_ode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (n_ode->kind == N_SEND) {
       tg_chat *chat = ctx_get_chat(ctx, n_ode->key.chat_id);
        if (chat) {
            can_write = chat->can_write;
        }
    }

    if (n_ode->kind == N_MESSAGES || n_ode->kind == N_INFO) {
        int res = generate_messages(ctx, n_ode, &text, &text_len, &mtime);
        if (res != 0) {
            free(text);
            fuse_reply_err(req, res);
            return;
        }
    }else if (n_ode -> kind == N_MEDIA_FILE) {
        tg_photo *photos = NULL;
        size_t n_photos = 0;
        tg_status status = tg_list_photos(ctx->backend, n_ode->key.chat_id, ctx->limit, &photos, &n_photos);
        if (status != TG_OK) {
            tg_free_photos(photos, n_photos);
            fuse_reply_err(req, status_to_errno(status));
            return;
        }
        tg_photo *photo = NULL;
        for (size_t i = 0; i < n_photos; i++) {
           if (photos[i].message_id == n_ode->key.message_id) {
               photo = &photos[i];
               break;
           }
        }
        if (!photo) {
            free(photos);
            fuse_reply_err(req, ENOENT);
            return;
        }
        mtime = photo->date;
        text_len = photo->size;
        tg_free_photos(photos, n_photos);
    }
    else if (n_ode -> kind == N_CATEGORY_OF_CHATS || n_ode->kind == N_MEMBER || n_ode->kind == N_MEMBERS
        || n_ode->kind == N_MESSAGES_FILES || n_ode ->kind == N_MESSAGE || n_ode->kind == N_MEDIA) {
        mtime = ctx -> chats_updated;
    }

    struct stat st;
    inode_fill_stat(n_ode, can_write, &st, &text_len, mtime);
    free(text);
    fuse_reply_attr(req, &st, 1.0);
}

static void tg_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    context *ctx = fuse_req_userdata(req);
    node*  paren_node= inode_get(parent);

    char *text = NULL;
    size_t text_len = 0;
    time_t mtime = 0;
    int can_write = 0;
    node *child_node = NULL;
    tg_member *members = NULL;

    if (!paren_node) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (paren_node -> kind == N_CATEGORY_OF_CHATS) {
        ctx_refresh_chats(ctx);
    }


    if (paren_node->kind == N_MEMBERS) {
        size_t n_members = 0;

        tg_status status = tg_list_members(ctx->backend, paren_node->key.chat_id, ctx->limit, &members, &n_members);
        if (status != TG_OK) {
            fuse_reply_err(req, status_to_errno(status));
            return;
        }

        child_node = inode_lookup(parent, name, ctx->chat, members, n_members);
        tg_free_members(members, n_members);
    }else {
        child_node = inode_lookup(parent, name, ctx->chat, NULL, ctx->n_chats);
    }
    if (!child_node) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (child_node->kind == N_SEND) {
        tg_chat *chat = ctx_get_chat(ctx, child_node->key.chat_id);
        if (chat) {
            can_write = chat->can_write;
        }
    }

    if (child_node->kind == N_MESSAGES || child_node->kind == N_INFO) {
        int res = generate_messages(ctx, child_node, &text, &text_len, &mtime);
        if (res != 0) {
            free(text);
            fuse_reply_err(req, res);
            return;
        }
    }else if (child_node -> kind == N_MEDIA_FILE) {
        tg_photo *photos = NULL;
        size_t n_photos = 0;
        tg_status status = tg_list_photos(ctx->backend, child_node->key.chat_id, ctx->limit, &photos, &n_photos);
        if (status != TG_OK) {
            tg_free_photos(photos, n_photos);
            fuse_reply_err(req, status_to_errno(status));
            return;
        }
        tg_photo *photo = NULL;
        for (size_t i = 0; i < n_photos; i++) {
            if (photos[i].message_id == child_node->key.message_id) {
                photo = &photos[i];
                break;
            }
        }
        if (!photo) {
            free(photos);
            fuse_reply_err(req, ENOENT);
            return;
        }
        mtime = photo->date;
        text_len = photo->size;
        tg_free_photos(photos, n_photos);
    }else if (paren_node -> kind == N_CATEGORY_OF_CHATS || paren_node->kind == N_MEMBER ||
        paren_node->kind == N_MEMBERS  || paren_node->kind == N_MESSAGES_FILES || paren_node ->kind == N_MESSAGE) {
        mtime = ctx -> chats_updated;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));

    struct stat st;
    inode_fill_stat(child_node, can_write, &st, &text_len, mtime);
    free(text);

    e.attr = st;
    e.ino = child_node->ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    fuse_reply_entry(req, &e);
}


static void tg_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    context *ctx = fuse_req_userdata(req);
    node* n_ode= inode_get(ino);

    if (!n_ode) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    int mode = fi->flags & O_ACCMODE;

    char *text = NULL;
    size_t len = 0;
    type_of_work type;
    int64_t target_chat = 0;
    int fd = -1;
    switch (n_ode->kind) {
        case N_ROOT:
        case N_CATEGORY_OF_CHATS:
        case N_NAME_OF_CHAT:
        case N_MESSAGES_FILES:
        case N_MEDIA:
            fuse_reply_err(req, EISDIR);
            return;
        case N_SEND:
            if (mode != O_WRONLY) {
                fuse_reply_err(req, EACCES);
                return;
            }
            if (n_ode->key.user_id != 0) {
                tg_chat *chat = NULL;
                tg_status status = tg_open_user_chat(ctx->backend, n_ode->key.user_id, &chat);
                if (status != TG_OK) {
                    fuse_reply_err(req, status_to_errno(status));
                    return;
                }
                if (!chat->can_write) {
                    tg_free_chats(chat, 1);
                    fuse_reply_err(req, EACCES);
                    return;
                }
                target_chat = chat->id;
                tg_free_chats(chat, 1);
            }else {
                tg_status status = ctx_refresh_chats(ctx);
                if (status != TG_OK) {
                    fuse_reply_err(req, status_to_errno(status));
                    return;
                }

                tg_chat* chat = ctx_get_chat(ctx, n_ode->key.chat_id);
                if (!chat) {
                    fuse_reply_err(req, ENOENT);
                    return;
                }

                if (!chat->can_write) {
                    fuse_reply_err(req, EACCES);
                    return;
                }
                target_chat = chat->id;
            }

            type = WRITE;
            break;
        case N_ADD:
            if (mode != O_WRONLY) {
                fuse_reply_err(req, EACCES);
                return;
            }

            tg_status status = ctx_refresh_chats(ctx);
            if (status != TG_OK) {
                fuse_reply_err(req, status_to_errno(status));
                return;
            }

            type = WRITE;
            break;
        case N_MESSAGES:
        case N_INFO: {
            if (mode != O_RDONLY) {
                fuse_reply_err(req, EACCES); return;
            }
            time_t mtime;
            int res = generate_messages(ctx, n_ode, &text, &len, &mtime);
            if (res != 0) {
                free(text); fuse_reply_err(req, res); return;
            }
            type = READ;
            break;
        }
        case N_MESSAGE: {
            if (mode != O_RDONLY) {
                fuse_reply_err(req, EACCES); return;
            }
            time_t mtime;
            int res = generate_message(ctx, n_ode, &text, &len, &mtime);
            if (res != 0) {
                free(text); fuse_reply_err(req, res); return;
            }
            type = READ;
            break;
        }
        case N_MEDIA_FILE: {
            if (mode != O_RDONLY) {
                fuse_reply_err(req, EACCES); return;
            }

            tg_photo *photos = NULL;
            size_t n_photos = 0;
            tg_status status = tg_list_photos(ctx->backend, n_ode->key.chat_id, ctx->limit, &photos, &n_photos);
            if (status != TG_OK) {
                tg_free_photos(photos, n_photos);
                fuse_reply_err(req, status_to_errno(status));
                return;
            }
            tg_photo *photo = NULL;
            for (size_t i = 0; i < n_photos; i++) {
                if (photos[i].message_id == n_ode->key.message_id) {
                    photo = &photos[i];
                    break;
                }
            }
            if (!photo) {
                free(photos);
                fuse_reply_err(req, ENOENT);
                return;
            }

            char *path = NULL;
            status = tg_download_photo(ctx->backend, photo->file_id, &path);
            tg_free_photos(photos, n_photos);

            if (status != TG_OK) {
                free(path);
                fuse_reply_err(req, status_to_errno(status));
                return;
            }

            fd = open(path, O_RDONLY);
            int open_error = errno;
            free(path);

            if (fd == -1) {
                fuse_reply_err(req, open_error);
                return;
            }

            type = READ;
            break;
        }
        default:
            fuse_reply_err(req, EIO);
            return;
    }
    struct buf_file *file = calloc(1, sizeof *file);
    if (!file) {
        free(text);
        if (fd >= 0) {
            close(fd);
        }
        fuse_reply_err(req, ENOMEM);
        return;
    }
    file->descriptor = fd;
    file->p = text;
    file->size = len;
    file->cap = len;
    file->type = type;
    file->chat_id = target_chat;
    file->kind = n_ode -> kind;

    fi->fh = (uint64_t)file;
    fi->direct_io = 1;

    if (type == WRITE) {
        fi->nonseekable = 1;
    }
    fuse_reply_open(req, fi);
}


static void tg_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                    off_t off, struct fuse_file_info *fi) {
    (void) ino;
    struct buf_file *file = (struct buf_file *)fi->fh;
    if (file -> descriptor == -1) {
        reply_buf_limited(req, file->p, file->size, off, size);
    }else {
        char *buf = malloc(size);
        if (!buf) {
            fuse_reply_err(req, ENOMEM);
            return;
        }
        ssize_t res = pread(file->descriptor, buf, size, off);
        if (res == -1) {
            fuse_reply_err(req, errno);
            free(buf);
            return;
        }
        fuse_reply_buf(req, buf, res);
        free(buf);
    }
}


static void tg_release(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info *fi) {
    (void) ino;
    struct buf_file *file = (struct buf_file *)fi->fh;
    if (file->descriptor != -1) {
        close(file->descriptor);
    }
    free(file->p);
    free(file);

    fuse_reply_err(req, 0);
}


static void tg_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off, struct fuse_file_info *fi){
    (void) off; (void) ino;
    struct buf_file *file = (struct buf_file *)fi->fh;

    size_t new_size = file->size + size + 1;
    if (new_size > file->cap) {
        size_t new_cap = file->cap;
        while (new_size > new_cap) {
            if (new_cap == 0) {
                new_cap = 64;
                continue;
            }
            new_cap = new_cap * 2;
        }
        char* pointer = realloc(file->p, new_cap);
        if (pointer == NULL) {
            fuse_reply_err(req, ENOMEM);
            return;
        }
        file->cap = new_cap;
        file->p = pointer;
    }

    memcpy(file->p + file->size, buf, size);
    file->size = new_size - 1;
    fuse_reply_write(req, size);
}


static void tg_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void) ino;
    context *ctx = fuse_req_userdata(req);
    struct buf_file *file = (struct buf_file *)fi->fh;
    if (file->type != WRITE || file->size == 0) {
        fuse_reply_err(req, 0);
        return;
    }

    size_t len = file->size;
    file->p[len] = '\0';

    file->size = 0;
    tg_status status;
    if (file->kind == N_ADD) {
        tg_chat *chat = NULL;
        status = tg_open_username_chat(ctx->backend, file->p, &chat);

        if (status == TG_OK) {
            tg_free_chats(chat, 1);
            status = ctx_refresh_chats(ctx);
        }
    }else if (file -> kind == N_SEND) {
        status = tg_send_message(ctx->backend, file->chat_id, file->p);
    }else {
        fuse_reply_err(req, EINVAL);
        return;
    }

    fuse_reply_err(req, status_to_errno(status));
}


void tg_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size){
    context *ctx = fuse_req_userdata(req);
    node *n_ode = inode_get(ino);
    if (n_ode == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (n_ode->kind != N_NAME_OF_CHAT) {
        fuse_reply_err(req, ENODATA);
        return;
    }

    tg_chat *chat = ctx_get_chat(ctx, n_ode->key.chat_id);
    if (chat == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    char buf[64];
    int len = 0;
    if (strcmp(name, "user.tg.id") == 0) {
        len = snprintf(buf, sizeof(buf), "%ld", chat->id);
    }
    if (strcmp(name, "user.tg.unread") == 0) {
        len = snprintf(buf, sizeof(buf), "%d", chat->unread);
    }
    if (strcmp(name, "user.tg.writable") == 0) {
        len = snprintf(buf, sizeof(buf), "%d", chat->can_write);
    }
    if (strcmp(name, "user.chat.notifications") == 0) {
        len = snprintf(buf, sizeof(buf), "%d", chat->muted);
    }
    if (strcmp(name, "user.id") == 0) {
        len = snprintf(buf, sizeof(buf), "%d", chat->user_id);
    }


    if (size == 0) {
        fuse_reply_xattr(req, (size_t)len);
    }
    else if (size < (size_t)len) {
        fuse_reply_err(req, ERANGE);
    }
    else {
        fuse_reply_buf(req, buf, (size_t)len);
    }
}



void tg_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
    node *n_ode = inode_get(ino);
    if (n_ode == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (n_ode->kind != N_NAME_OF_CHAT) {
        fuse_reply_err(req, ENODATA);
        return;
    }

    char buf[256];
    int len = 0;
    for (int i = 0; i < sizeof(xattr_names) / sizeof(*xattr_names); i++) {
        size_t l = strlen(xattr_names[i]) + 1;
        memcpy(buf + len, xattr_names[i], l);
        len += l;
    }

    if (size == 0) {
        fuse_reply_xattr(req, (size_t)len);
    }
    else if (size < (size_t)len) {
        fuse_reply_err(req, ERANGE);
    }
    else {
        fuse_reply_buf(req, buf, (size_t)len);
    }
}


static int move_chat_to_archive(context *ctx, tg_chat *chat, const node *src, const node *dst, const char *name, const char *newname) {
    int dst_archive = dst->category == TG_ARCHIVE;
    int src_archive = src->category == TG_ARCHIVE;
    if (src_archive && dst_archive) {
        return EINVAL;
    }

    if (strcmp(name, newname) != 0) {
        return EINVAL;
    }

    if (dst->category != TG_ARCHIVE && chat->type != dst->category) {
        return EINVAL;
    }

    tg_status status = tg_archive_chat(ctx->backend, chat->id, dst_archive);

    if (status != TG_OK) {
        return status_to_errno(status);
    }

    chat->archived = dst_archive;
    ctx->chats_updated = time(NULL);

    return 0;
}


static void tg_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent, const char *newname, unsigned int flags) {
    context *ctx = fuse_req_userdata(req);
    node *src = inode_get(parent);
    node *dst = inode_get(newparent);

    if (src == NULL || dst == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (flags & ~RENAME_NOREPLACE) {
        fuse_reply_err(req, EINVAL);
        return;
    }

    if (src->kind != N_CATEGORY_OF_CHATS || dst->kind != N_CATEGORY_OF_CHATS) {
        fuse_reply_err(req, EACCES);
        return;
    }

    tg_status status = ctx_refresh_chats(ctx);
    if (status != TG_OK) {
        fuse_reply_err(req, status_to_errno(status));
        return;
    }

    node *child_node = inode_lookup(parent, name, ctx->chat, NULL, ctx->n_chats);
    if (!child_node) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    tg_chat *chat = ctx_get_chat(ctx, child_node->key.chat_id);
    if (chat == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (parent != newparent) {
        int error = move_chat_to_archive(ctx, chat, src, dst, name, newname);
        fuse_reply_err(req, error);
        return;
    }

    if (strcmp(name, newname) == 0) {
        fuse_reply_err(req, 0);
        return;
    }

    status = tg_rename_chat(ctx->backend, chat->id, newname);
    if (status != TG_OK) {
        fuse_reply_err(req, status_to_errno(status));
        return;
    }
    char *title = strdup(newname);
    free(chat->title);
    chat->title = title;
    ctx->chats_updated = time(NULL);

    fuse_reply_err(req, 0);
}


static void tg_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
    (void) mode;

    context *ctx = fuse_req_userdata(req);
    node *parent_node = inode_get(parent);

    if (parent_node == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (parent_node->kind == N_MEMBERS) {
        char *end;
        int64_t user_id = strtoll(name, &end, 10);
        if (user_id <= 0) {
            fuse_reply_err(req, EINVAL);
            return;
        }

        tg_status status = tg_add_member(ctx->backend, parent_node->key.chat_id, user_id);
        if (status != TG_OK) {
            fuse_reply_err(req, status_to_errno(status));
            return;
        }

        node *child = inode_get_or_create(parent_node, N_MEMBER, user_id);
        if (child == NULL) {
            fuse_reply_err(req, ENOENT);
            return;
        }

        struct fuse_entry_param e;
        memset(&e, 0, sizeof(e));

        struct stat st;
        size_t len = 0;
        inode_fill_stat(child, 0, &st, &len, ctx->chats_updated);

        e.attr = st;
        e.ino = child->ino;
        e.attr_timeout = 1.0;
        e.entry_timeout = 1.0;

        fuse_reply_entry(req, &e);
        return;
    }

    if (parent_node->category != TG_GROUP && parent_node->category != TG_CHANNEL) {
        fuse_reply_err(req, EACCES);
        return;
    }

    if (parent_node->kind != N_CATEGORY_OF_CHATS) {
        fuse_reply_err(req, EACCES);
        return;
    }

    tg_status status = ctx_refresh_chats(ctx);
    if (status != TG_OK) {
        fuse_reply_err(req, status_to_errno(status));
        return;
    }

    node *existing = inode_lookup(parent, name, ctx->chat, NULL, ctx->n_chats);
    if (existing) {
        fuse_reply_err(req, EEXIST);
        return;
    }

    tg_chat *new = realloc(ctx -> chat, (ctx -> n_chats + 1) * sizeof(tg_chat));
    if (new == NULL) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    ctx->chat = new;

    tg_chat *created = NULL;

    status = tg_create_chat(ctx->backend, parent_node -> category, name, &created);
    if (status != TG_OK) {
        fuse_reply_err(req, status_to_errno(status));
        return;
    }

    ctx->chat[ctx->n_chats] = *created;
    ctx->n_chats++;
    ctx->chats_updated = time(NULL);

    int64_t chat_id = created->id;
    free(created);

    node *new_node = inode_get_or_create(parent_node, N_NAME_OF_CHAT, chat_id);
    if (new_node == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));

    struct stat st;
    size_t len = 0;
    inode_fill_stat(new_node, 0, &st, &len, ctx->chats_updated);

    e.attr = st;
    e.ino = new_node->ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    fuse_reply_entry(req, &e);
}


static void tg_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
    context *ctx = fuse_req_userdata(req);
    node *parent_node = inode_get(parent);

    if (parent_node == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (parent_node->kind != N_MEMBERS) {
        fuse_reply_err(req, EACCES);
        return;
    }

    tg_member *members = {0};
    size_t n_members = 0;
    tg_status status = tg_list_members(ctx->backend, parent_node -> key.chat_id, ctx->limit, &members, &n_members);

    if (status != TG_OK) {
        fuse_reply_err(req, status_to_errno(status));
        return;
    }

    node *child_node = inode_lookup(parent_node->ino, name, ctx->chat, members, n_members);
    if (!child_node) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    int64_t user_id = child_node->key.user_id;
    tg_free_members(members, n_members);

    status = tg_remove_member(ctx->backend, parent_node->key.chat_id, user_id);
    if (status != TG_OK) {
        fuse_reply_err(req, status_to_errno(status));
        return;
    }

    fuse_reply_err(req, 0);
}


static void tg_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name, const char *value, size_t size, int flags) {
    (void) flags;

    context *ctx = fuse_req_userdata(req);

    node *n_ode = inode_get(ino);

    if (n_ode == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (n_ode -> kind != N_NAME_OF_CHAT) {
        fuse_reply_err(req, EACCES);
        return;
    }

    if (strcmp(name, "user.chat.notifications") != 0) {
        fuse_reply_err(req, EACCES);
        return;
    }

    if (size != 1 || (value[0] != '0' && value[0] != '1')) {
        fuse_reply_err(req, EINVAL);
        return;
    }

    int muted = value[0] - '0';

    tg_status status = ctx_refresh_chats(ctx);
    if (status != TG_OK) {
        fuse_reply_err(req, status_to_errno(status));
        return;
    }

    status = tg_set_chat_muted(ctx->backend, n_ode->key.chat_id, muted);

    if (status != TG_OK) {
        fuse_reply_err(req, status_to_errno(status));
        return;
    }

    tg_chat *chat = ctx_get_chat(ctx, n_ode->key.chat_id);
    if (chat) {
        chat->muted = muted;
    }

    fuse_reply_err(req, 0);
}

const struct fuse_lowlevel_ops tgfs_ops = {
    .rename = tg_rename,
    .getattr = tg_getattr,
    .lookup = tg_lookup,
    .readdir = tg_readdir,
    .open = tg_open,
    .read = tg_read,
    .release = tg_release,
    .write = tg_write,
    .flush = tg_flush,
    .getxattr = tg_getxattr,
    .listxattr = tg_listxattr,
    .mkdir = tg_mkdir,
    .setxattr = tg_setxattr,
    .rmdir = tg_rmdir,
};

