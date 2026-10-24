#pragma once
#include <cstdint>
#include <cstddef>
struct flash_area { const uint8_t *data; size_t fa_size; };
int flash_area_read(const flash_area *,uint32_t,void *,size_t);
int flash_area_open(int,const flash_area **);
void flash_area_close(const flash_area *);
int flash_area_write(const flash_area *,uint32_t,const void *,size_t);
struct flash_area_check {const uint8_t *match;size_t clen;uint8_t *rbuf;size_t rblen;};
int flash_area_check_int_sha256(const flash_area *,const flash_area_check *);
