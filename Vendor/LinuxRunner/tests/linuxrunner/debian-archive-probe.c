#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
int main(int argc,char **argv){
 if(argc!=2)return 2;struct archive *a=archive_read_new();archive_read_support_format_ar(a);archive_read_support_filter_all(a);
 if(archive_read_open_filename(a,argv[1],65536)!=ARCHIVE_OK){fprintf(stderr,"outer: %s\n",archive_error_string(a));return 1;}
 struct archive_entry *e;while(archive_read_next_header(a,&e)==ARCHIVE_OK){
  if(!strcmp(archive_entry_pathname(e),"control.tar.xz")){
   size_t size=(size_t)archive_entry_size(e);void *bytes=malloc(size);if(!bytes)return 1;
   if(archive_read_data(a,bytes,size)!=(la_ssize_t)size)return 1;
   struct archive *inner=archive_read_new();int xz=archive_read_support_filter_xz(inner);archive_read_support_format_tar(inner);errno=0;
   int r=archive_read_open_memory(inner,bytes,size);fprintf(stderr,"package=apt member=control.tar.xz operation=archive_read_open_memory registration=%d result=%d archive_errno=%d errno=%d message=%s\n",xz,r,archive_errno(inner),errno,archive_error_string(inner)?archive_error_string(inner):"none");
   archive_read_free(inner);free(bytes);archive_read_free(a);return r==ARCHIVE_OK?0:1;
  }archive_read_data_skip(a);
 }return 1;
}
