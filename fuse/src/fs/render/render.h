#ifndef RENDER_H
#define RENDER_H

#include "tg_backend.h"

int render_messages(const tg_message *message, size_t n, char **out, size_t *len);
int render_info(const tg_chat * chat, char **out, size_t *len);

#endif
