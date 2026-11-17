#ifndef INODE_H
#define INODE_H

#include <fuse_lowlevel.h>
#include <sys/stat.h>
#include "../../../third_party/uthash.h"
#include "../../backend/tg_backend.h"

typedef enum { N_ROOT, N_CATEGORY_OF_CHATS, N_NAME_OF_CHAT, N_ADD, N_MESSAGES, N_SEND, N_INFO,
    N_MEDIA, N_MEDIA_FILE, N_MESSAGES_FILES, N_MESSAGE, N_MEMBERS, N_MEMBER} node_kind;

typedef struct {
    int64_t chat_id;
    node_kind kind;
    int64_t user_id;
    int64_t message_id;
} node_key;

typedef struct inode {
    fuse_ino_t ino;
    node_key key;
    node_kind kind;
    tg_chat_type category;
    UT_hash_handle hh_ino;
    UT_hash_handle hh_key;
} node;

void inode_init(void);
void inode_destroy(void);
node* inode_get(fuse_ino_t ino);
node* inode_lookup(fuse_ino_t parent, const char *name, const tg_chat *chats, const tg_member *members, size_t n);
const char *inode_category_name(tg_chat_type t);
node *inode_get_or_create(const node *parent, node_kind kind, int64_t id);
void inode_fill_stat(const node *n, int can_write, struct stat *st, size_t* len, time_t mtime);
char *inode_chat_name(const tg_chat *chats, size_t i);
char *inode_member_name(const tg_member *members, size_t i);
char *inode_message_name(const tg_message *m);
char *inode_photo_name(const tg_photo *p);

#endif