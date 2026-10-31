#ifndef ED_EVIDENCE_CAPTURE_H
#define ED_EVIDENCE_CAPTURE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void ed_evidence_capture_push(int16_t x, int16_t y, int16_t z);
int ed_evidence_capture_request(const char *event_id, const char *challenge);
int ed_evidence_capture_poll(void);
uint8_t ed_evidence_capture_pending(void);
int ed_evidence_capture_get_chunk(uint16_t index, uint16_t *total, const char **event_id, const uint8_t **data, size_t *length);
void ed_evidence_capture_ack_upload(void);
#ifdef __cplusplus
}
#endif
#endif
