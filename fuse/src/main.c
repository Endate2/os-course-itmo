#include <stdio.h>
#include <stdlib.h>
#include <fuse_lowlevel.h>

#include "tg_backend.h"
#include "fs/context/ctx.h"
#include "fs/inode/inode.h"
#include "ops.h"

struct options{
    int limit;
};

static struct fuse_opt tg_opts[] = {
    { "limit=%d", offsetof(struct options, limit), 0 },
    FUSE_OPT_END
};


int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_cmdline_opts opts;
    struct options opt = {0};
    struct fuse_session *se;
    tg_backend *backend;
    context ctx = {0};
    tg_status st;

    const char *id = getenv("TG_API_ID");
    const char *hash = getenv("TG_API_HASH");
    if (fuse_opt_parse(&args, &opt, tg_opts, NULL) == -1) {
        return 1;
    }

    if (fuse_parse_cmdline(&args, &opts) != 0) {
        return 1;
    }


    if (fuse_opt_add_arg(&args, "-odefault_permissions") != 0) {
        free(opts.mountpoint);
        fuse_opt_free_args(&args);
        return 1;
    }

    backend = tg_backend_create(atoi(id), hash, "tdlib-db");
    if (!backend) {
        free(opts.mountpoint);
        fuse_opt_free_args(&args);
        return 1;
    }

    st = tg_login(backend);
    if (st != TG_OK) {
        tg_backend_destroy(backend);
        free(opts.mountpoint);
        fuse_opt_free_args(&args);
        return 1;
    }
    ctx.backend = backend;
    ctx.limit = opt.limit;

    st = ctx_refresh_chats(&ctx);

    inode_init();

    se = fuse_session_new(&args, &tgfs_ops, sizeof(tgfs_ops), &ctx);
    if (!se) {
        inode_destroy();
        ctx_free(&ctx);
        tg_backend_destroy(backend);
        free(opts.mountpoint);
        fuse_opt_free_args(&args);
        return 1;
    }

    if (fuse_set_signal_handlers(se) != 0) {
        fuse_session_destroy(se);
        inode_destroy();
        ctx_free(&ctx);
        tg_backend_destroy(backend);
        free(opts.mountpoint);
        fuse_opt_free_args(&args);
        return 1;
    }

    if (fuse_session_mount(se, opts.mountpoint) != 0) {
        fuse_remove_signal_handlers(se);
        fuse_session_destroy(se);
        inode_destroy();
        ctx_free(&ctx);
        tg_backend_destroy(backend);
        free(opts.mountpoint);
        fuse_opt_free_args(&args);
        return 1;
    }

    fuse_session_loop(se);

    fuse_session_unmount(se);
    fuse_remove_signal_handlers(se);
    fuse_session_destroy(se);
    inode_destroy();
    ctx_free(&ctx);
    tg_backend_destroy(backend);
    free(opts.mountpoint);
    fuse_opt_free_args(&args);

    return 0;
}