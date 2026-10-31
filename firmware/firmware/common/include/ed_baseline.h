#ifndef ED_BASELINE_H
#define ED_BASELINE_H
#include "ed_monitor_types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { ed_baseline_t value; float samples[60][ED_FEATURE_COUNT]; uint32_t count; uint64_t started_mono_us; uint8_t active; uint8_t timed_out; } ed_baseline_builder_t;
void ed_baseline_begin(ed_baseline_builder_t *b, const ed_asset_key_t *key);
void ed_baseline_begin_at(ed_baseline_builder_t *b, const ed_asset_key_t *key, uint64_t now_us);
int ed_baseline_feed(ed_baseline_builder_t *b, const ed_features_t *f);
int ed_baseline_feed_ex(ed_baseline_builder_t *b, const ed_features_t *f, ed_quality_t quality, uint8_t idle, uint8_t saturated, uint64_t now_us);
int ed_baseline_preview(const ed_baseline_builder_t *b, ed_baseline_t *out);
int ed_baseline_commit(ed_baseline_builder_t *b, ed_baseline_t *out);
float ed_baseline_deviation(const ed_baseline_t *b, const ed_features_t *f);
int ed_baseline_key_matches(const ed_baseline_t *b, const ed_asset_key_t *key);
#ifdef __cplusplus
}
#endif
#endif
