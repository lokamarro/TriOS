#include <stddef.h>
// SHA-512 crypt ("$6$<salt>$<hash>", 5000 rounds) for /etc/shadow via chpasswd -e.
int lr_sha512_crypt(const char *password, const char *salt, char *out, size_t out_size);
// 16 random characters from the crypt alphabet.
int lr_random_salt(char out[17]);
