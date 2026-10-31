#ifndef ED_INFERENCE_MODELS_H
#define ED_INFERENCE_MODELS_H
extern const unsigned char ed_model_cpu[];
extern const unsigned char ed_model_u55[];
extern const unsigned int ed_model_cpu_size;
extern const unsigned int ed_model_u55_size;
extern const float ed_model_input_scale;
extern const int ed_model_input_zero;
extern const float ed_model_output_scale;
extern const signed char ed_model_golden_inputs[][64];
extern const signed char ed_model_golden_outputs[][5];
extern const unsigned int ed_model_golden_count;
#endif
