/* NIST SP800-38D all-zero AES128-GCM vector, authentication and decryption. */
#include <stdio.h>
#include <string.h>
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct evp_cipher_st EVP_CIPHER;
extern EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);extern void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *);
extern const EVP_CIPHER *EVP_aes_128_gcm(void);
extern int EVP_EncryptInit_ex(EVP_CIPHER_CTX*,const EVP_CIPHER*,void*,const unsigned char*,const unsigned char*);
extern int EVP_EncryptUpdate(EVP_CIPHER_CTX*,unsigned char*,int*,const unsigned char*,int);
extern int EVP_EncryptFinal_ex(EVP_CIPHER_CTX*,unsigned char*,int*);
extern int EVP_DecryptInit_ex(EVP_CIPHER_CTX*,const EVP_CIPHER*,void*,const unsigned char*,const unsigned char*);
extern int EVP_DecryptUpdate(EVP_CIPHER_CTX*,unsigned char*,int*,const unsigned char*,int);
extern int EVP_DecryptFinal_ex(EVP_CIPHER_CTX*,unsigned char*,int*);
extern int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX*,int,int,void*);
int main(void){unsigned char key[16]={0},iv[12]={0},plain[16]={0},out[32],tag[16],decoded[32];int n,last;const unsigned char expected[16]={0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78},expected_tag[16]={0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};EVP_CIPHER_CTX *c=EVP_CIPHER_CTX_new();if(!c)return 1;if(EVP_EncryptInit_ex(c,EVP_aes_128_gcm(),0,key,iv)!=1||EVP_EncryptUpdate(c,out,&n,plain,16)!=1||n!=16||EVP_EncryptFinal_ex(c,out+n,&last)!=1||last||EVP_CIPHER_CTX_ctrl(c,0x10,16,tag)!=1)return 2;if(memcmp(expected,out,16)||memcmp(expected_tag,tag,16))return 3;EVP_CIPHER_CTX_free(c);for(int tamper=0;tamper<2;tamper++){c=EVP_CIPHER_CTX_new();if(!c)return 4;if(tamper)tag[0]^=1;if(EVP_DecryptInit_ex(c,EVP_aes_128_gcm(),0,key,iv)!=1||EVP_DecryptUpdate(c,decoded,&n,out,16)!=1||EVP_CIPHER_CTX_ctrl(c,0x11,16,tag)!=1)return 5;int ok=EVP_DecryptFinal_ex(c,decoded+n,&last);EVP_CIPHER_CTX_free(c);if(ok!=(tamper?0:1)||(!tamper&&memcmp(decoded,plain,16)))return 6;}puts("AES_GCM_KAT: PASS ciphertext/tag/decrypt/tampered-tag rejection");return 0;}
