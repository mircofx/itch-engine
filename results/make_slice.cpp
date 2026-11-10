// Walks ITCH 5.0 framing (2-byte BE length prefix) and prints the byte offset after N messages.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
int main(int c, char** v){ int fd=open(v[1],O_RDONLY); struct stat st; fstat(fd,&st);
 auto* p=(const uint8_t*)mmap(nullptr,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); uint64_t n=strtoull(v[2],0,10),i=0,off=0;
 while(i<n && off+2<=(uint64_t)st.st_size){ uint16_t len=(p[off]<<8)|p[off+1]; off+=2+len; ++i; }
 printf("%lu %lu\n",i,off); }
