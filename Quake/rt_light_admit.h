#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_LIGHT_REGISTRY_CAPACITY 4095

#define RT_LIGHT_RENDERER_KEEP_SUM 0.0001f
#define RT_LIGHT_KEEP_SUM          0.0002f

typedef struct rt_light_budget_s
{
	int used;
} rt_light_budget_t;

void RT_LightBudgetInit (rt_light_budget_t *budget);
int  RT_LightBudgetRemaining (const rt_light_budget_t *budget);
int  RT_LightBudgetClaim (rt_light_budget_t *budget, int count);

void RT_KeepEmissiveLightColor (float color[3]);

#ifdef __cplusplus
}
#endif
