// i915 register model data tables (d4.1): see i915_model_tables.c. (satoru)
#ifndef KLD_I915_MODEL_TABLES_H
#define KLD_I915_MODEL_TABLES_H

struct i915m_init { unsigned int off; unsigned int val; };
extern const struct i915m_init i915m_init_table[];
extern const unsigned int i915m_init_count;

#endif
// end (satoru)
