#ifndef ED_EVIDENCE_FORMAT_H
#define ED_EVIDENCE_FORMAT_H
#include <stdint.h>
#include <stddef.h>
#define ED_EDV2_HEADER_BYTES 24u
int ed_edv2_pack(const uint8_t *metadata, uint32_t metadata_len, const int16_t *xyz, uint32_t samples, uint8_t *out, size_t cap, size_t *written);
int ed_edv2_validate(const uint8_t *blob, size_t length);
#endif
