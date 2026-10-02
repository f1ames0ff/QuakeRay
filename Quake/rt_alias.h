#ifndef RT_ALIAS_H
#define RT_ALIAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_ALIAS_UNIFORM_PRIOR 0.05

int RT_Alias_Build(const double *weights, int count, float *outPrimary, float *outSecondary, uint32_t *outAlias);

#ifdef __cplusplus
}
#endif

#endif
