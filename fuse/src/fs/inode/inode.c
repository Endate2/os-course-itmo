#include "inode.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static node *head_ino = NULL;
static fuse_ino_t next_ino = 7;
static node *head_key = NULL;
static time_t mount_time = 0;

static const char *category_names[] = { "users", "groups", "channels", "archive" };

static void base_name(const char *title, char *buf, size_t sz) {
    size_t len = 0;
    int words = 1;

    while (*title == ' ') title++;
    for (; *title && len + 1 < sz; title++) {
        if (*title == ' ') {
            while (title[1] == ' ') title++;
            if (title[1] == '\0' || ++words > 2)
                break;
            buf[len++] = '_';
        } else {
            buf[len++] = *title;
        }
    }
    buf[len] = '\0';
}

char *inode_message_name(const tg_message *m) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld.txt", m->id);
    return strdup(buf);
}

char *inode_photo_name(const tg_photo *p) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld.jpg", p->message_id);
    return strdup(buf);
}

char *inode_chat_name(const tg_chat *chats, size_t i) {
    char mine[128];
    base_name(chats[i].title, mine, sizeof mine);
    return strdup(mine);
}

char *inode_member_name(const tg_member *members, size_t i) {
    char mine[128];
    base_name(members[i].name, mine, sizeof mine);
    return strdup(mine);
}

void inode_fill_stat(const node *n, int can_write, struct stat *st, size_t* len, time_t mtime) {
    if (mtime == 0) {
        mtime = mount_time;
    }
    memset(st, 0, sizeof *st);
    st->st_ino = n->ino;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_size = (off_t)*len;
    st->st_atime = mtime;
    st->st_mtime = mtime;
    st->st_ctime = mtime;

    switch (n->kind) {
        case N_ROOT:
        case N_MESSAGES_FILES:
        case N_MEMBER:
        case N_MEDIA:
            st->st_mode = S_IFDIR | 0555;
            st->st_nlink = 2;
            break;
        case N_MESSAGES:
        case N_MESSAGE:
        case N_MEDIA_FILE:
        case N_INFO:
            st->st_mode  = S_IFREG | 0444;
            st->st_nlink = 1;
            break;
        case N_SEND:
            st->st_mode  = S_IFREG | (can_write ? 0200 : 0444);
            st->st_nlink = 1;
            break;
        case N_CATEGORY_OF_CHATS:
        case N_NAME_OF_CHAT:
            st->st_mode = S_IFDIR | 0755;
            st->st_nlink = 2;
            break;
        case N_ADD:
            st->st_mode = S_IFREG | 0200;
            st->st_nlink = 1;
            break;
        case N_MEMBERS:
            st->st_mode = S_IFDIR | 0755;
            st->st_nlink = 2;
            break;
        default:
            break;
    }
}

static node_key make_key(int64_t chat_id, node_kind kind, int64_t user_id, int64_t message_id) {
    node_key k;
    memset(&k, 0, sizeof k);
    k.chat_id = chat_id;
    k.kind = kind;
    k.user_id = user_id;
    k.message_id = message_id;
    return k;
}

const char *inode_category_name(tg_chat_type t) {
    return category_names[t];
}

static node* add_node(fuse_ino_t ino, node_kind kind) {
    node *n = calloc(1, sizeof(node));
    if (!n) {
        abort();
    }
    n->ino = ino;
    n->kind = kind;
    HASH_ADD(hh_ino, head_ino, ino, sizeof(fuse_ino_t), n);
    return n;
}

void inode_init(void) {
    add_node(FUSE_ROOT_ID, N_ROOT);

    mount_time = time(NULL);
    for (int t = 0; t < TG_CHAT_TYPE_COUNT; t++) {
        node *n = add_node(t + 2, N_CATEGORY_OF_CHATS);
        n->category = (tg_chat_type)t;
    }
    node *n = add_node(6, N_ADD);
    n->category = TG_USER;
}

node *inode_get(fuse_ino_t ino) {
    node *n = NULL;
    HASH_FIND(hh_ino, head_ino, &ino, sizeof(fuse_ino_t), n);
    return n;
}

node* inode_lookup(fuse_ino_t parent, const char *name, const tg_chat *chats, const tg_member *members, size_t size) {
    node *parent_node = inode_get(parent);

    if (!parent_node) {
        return NULL;
    }
    switch (parent_node->kind) {
        case N_ROOT:
            for (size_t i = 0; i < TG_CHAT_TYPE_COUNT; i++) {
                if (strcmp(category_names[i] , name) == 0) {
                    return inode_get(i+2);
                }
            }
            return NULL;

        case N_CATEGORY_OF_CHATS:
            if (parent_node->category == TG_USER && strcmp(name, "add") == 0) {
                return inode_get(6);
            }
            for (size_t i = 0; i < size; i++) {
                if (parent_node->category == TG_ARCHIVE) {
                    if (!chats[i].archived)
                        continue;
                } else {
                    if (chats[i].type != parent_node->category || chats[i].archived)
                        continue;
                }

                char *nm = inode_chat_name(chats, i);
                if (!nm) {
                    return NULL;
                }
                int eq = strcmp(name, nm) == 0;
                free(nm);
                if (eq) {
                    node *child =  inode_get_or_create(parent_node, N_NAME_OF_CHAT, chats[i].id);

                    if (child != NULL) {
                        child->category = chats[i].type;
                    }

                    return child;
                }
            }
            return NULL;
        case N_MEMBERS:
            for (size_t i = 0; i < size; i++) {
                char *nm = inode_member_name(members, i);
                if (!nm) {
                    return NULL;
                }
                int eq = strcmp(name, nm) == 0;
                free(nm);
                if (eq) {
                    return inode_get_or_create(parent_node, N_MEMBER, members[i].user_id);
                }
            }
            return NULL;
        case N_MEMBER:
        case N_NAME_OF_CHAT: {
            node_kind kind;
            if (strcmp(name, "messages") == 0) {
                kind = N_MESSAGES;
            }else if (strcmp(name, "send") == 0) {
                kind = N_SEND;
            }else if (strcmp(name, "info") == 0) {
                kind = N_INFO;
            }else if (strcmp(name, "messages_files") == 0 ) {
                kind = N_MESSAGES_FILES;
            }else if (strcmp(name, "media") == 0) {
                kind = N_MEDIA;
            }else if (strcmp(name, "members") == 0 && parent_node -> category == TG_GROUP) {
                kind = N_MEMBERS;
                return inode_get_or_create(parent_node, N_MEMBERS, 0);
            }
            else {
                return NULL;
            }

            return inode_get_or_create(parent_node, kind, 0);
        }
        case N_MESSAGES_FILES: {
            long id;
            sscanf(name, "%ld.txt", &id);
            if (id <= 0) {
                return NULL;
            }

            return inode_get_or_create(parent_node, N_MESSAGE, id);
        }
        case N_MEDIA: {
            long id;
            sscanf(name, "%ld.jpg", &id);
            if (id <= 0) {
                return NULL;
            }
            return inode_get_or_create(parent_node, N_MEDIA_FILE, id);
        }
        default:
            return NULL;
    }
}

void inode_destroy(void) {
    node *nd, *tmp;
    HASH_CLEAR(hh_key, head_key);
    HASH_ITER(hh_ino, head_ino, nd, tmp) {
        HASH_DELETE(hh_ino, head_ino, nd);
        free(nd);
    }
    next_ino = 7;
}

node *inode_get_or_create(const node *parent, node_kind kind, int64_t id) {
    if (!parent) {
        return NULL;
    }

    int64_t chat_id = 0;
    int64_t user_id = 0;
    int64_t message_id = 0;

    switch (kind) {
        case N_NAME_OF_CHAT:
            chat_id = id;
            break;
        case N_MEMBER:
            chat_id = parent->key.chat_id;
            user_id = id;
            break;
        case N_MESSAGE:
        case N_MEDIA_FILE:
            chat_id = parent->key.chat_id;
            user_id = parent->key.user_id;
            message_id = id;
            break;
        case N_MEMBERS:
        case N_MESSAGES:
        case N_SEND:
        case N_INFO:
        case N_MEDIA:
        case N_MESSAGES_FILES:
            chat_id = parent->key.chat_id;
            user_id = parent->key.user_id;
            break;
        default:
            return NULL;
    }

    node_key k = make_key(chat_id, kind, user_id, message_id);
    node *n = NULL;

    HASH_FIND(hh_key, head_key, &k, sizeof(node_key), n);
    if (n) {
        return n;
    }

    n = add_node(next_ino++, kind);

    n->key = k;
    n->category = parent->category;
    HASH_ADD(hh_key, head_key, key, sizeof(node_key), n);

    return n;
}