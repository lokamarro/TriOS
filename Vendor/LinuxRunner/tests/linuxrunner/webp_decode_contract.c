// Calls the original distribution libwebp through its public decoding API.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint8_t *WebPDecodeRGBA(const uint8_t *,size_t,int *,int *);
extern size_t WebPEncodeRGB(const uint8_t *,int,int,int,float,uint8_t **);
extern void WebPFree(void *);
static uint8_t *file(const char *name,size_t *size) {
    FILE *f=fopen(name,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)||(*size=ftell(f))>1024*1024){fclose(f);return NULL;}
    rewind(f);uint8_t *p=malloc(*size);if(!p){fclose(f);return NULL;}
    if(fread(p,1,*size,f)!=*size){free(p);p=NULL;}fclose(f);return p;
}
int main(int argc,char **argv) {
    if(argc==6&&!strcmp(argv[1],"--encode")) {
        int w=atoi(argv[3]),h=atoi(argv[4]);size_t n;
        uint8_t *input=file(argv[2],&n),*output=NULL;
        if(w<1||h<1||w>512||h>512||!input||n!=(size_t)w*h*3)return 2;
        size_t encoded=WebPEncodeRGB(input,w,h,w*3,75.0f,&output);
        if(!encoded||!output)return 1;
        FILE *f=fopen(argv[5],"wb");if(!f)return 1;
        int ok=fwrite(output,1,encoded,f)==encoded;ok&=fclose(f)==0;
        WebPFree(output);free(input);if(!ok)return 1;
        printf("WEBP_ENCODE_PASS width=%d height=%d bytes=%zu original_library=PASS\n",w,h,encoded);return 0;
    }
    if(argc!=3)return 2;
    size_t n,m;uint8_t *data=file(argv[1],&n),*expected=file(argv[2],&m);if(!data||!expected)return 2;
    int width=0,height=0;uint8_t *pixels=WebPDecodeRGBA(data,n,&width,&height);
    if(!pixels||width<=0||height<=0||(size_t)width*height*4!=m)return 1;
    if(memcmp(pixels,expected,m)){fprintf(stderr,"FAIL original libwebp pixels differ\n");return 1;}
    printf("WEBP_DECODE_PASS width=%d height=%d pixels=%zu original_library=PASS\n",width,height,m);
    WebPFree(pixels);free(data);free(expected);return 0;
}
