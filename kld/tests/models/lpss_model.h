// lpss_model: the Intel LPSS private register block the mfd driver touches.
// end (satoru)
#ifndef KLD_LPSS_MODEL_H
#define KLD_LPSS_MODEL_H
void lpss_model_init(void *bar, unsigned long len);
unsigned long lpss_model_reads(void);
unsigned long lpss_model_writes(void);
#endif
// end (satoru)
