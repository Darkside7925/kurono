// i915 register model seam (d4.1): see i915_model.c. (satoru)
#ifndef KLD_I915_MODEL_H
#define KLD_I915_MODEL_H

// argv "model:<variant>" turns the model on before the driver loads. (satoru)
void i915_model_enable(const char *variant);
int i915_model_on(void);
// one summary line (write counts per block) to the log. (satoru)
void i915_model_summary(const char *when);

#endif
// end (satoru)
