#pragma once
#include <stddef.h>
// Caller authenticates all .deb files before this parser is entered. No archive
// entry is extracted onto the host: guest metadata is retained in an output tar.
struct lr_debian_bootstrap_options {
    int (*progress)(void *, size_t, size_t);
    int (*cancel)(void *);
    void (*log)(void *, int severity, const char *message);
    void *context;
    int trace_entries;
};
int lr_debian_bootstrap_diagnosed(const char *const *,size_t,const char *,const struct lr_debian_bootstrap_options *,char *,size_t);
int lr_debian_bootstrap(const char *const *,size_t,const char *,int (*)(void *,size_t,size_t),void *,char *,size_t);
// Arch Linux ARM. The caller authenticates every package (SHA-256 from the
// repository database and the package's OpenPGP signature) before this runs.
// local_names[i] is "NAME-VERSION"; local_descs[i] is the pacman local
// database desc text. Writes package files, var/lib/pacman/local/* (desc,
// files, install scripts) and LinuxRunner's first-boot initialization.
int lr_arch_bootstrap_diagnosed(const char *const *packages, const char *const *local_names, const char *const *local_descs,
                                size_t count, const char *output, const struct lr_debian_bootstrap_options *options,
                                char *error, size_t error_size);
// Reads a pacman repository database (gzip tar). *out receives, for each
// entry, "\x1e" NAME-VERSION "\n" followed by its desc and depends files.
int lr_arch_read_database(const char *path, char **out, size_t *out_size, char *error, size_t error_size);
