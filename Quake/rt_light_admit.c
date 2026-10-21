#include "rt_light_admit.h"

void RT_LightBudgetInit (rt_light_budget_t *budget)
{
	budget->used = 0;
}

int RT_LightBudgetRemaining (const rt_light_budget_t *budget)
{
	return RT_LIGHT_REGISTRY_CAPACITY - budget->used;
}

int RT_LightBudgetClaim (rt_light_budget_t *budget, int count)
{
	if (count <= 0 || count > RT_LightBudgetRemaining (budget))
		return 0;

	budget->used += count;
	return 1;
}

void RT_KeepEmissiveLightColor (float color[3])
{
	float sum = 0.0f;

	for (int i = 0; i < 3; i++)
		if (color[i] > 0.0f)
			sum += color[i];

	if (sum < RT_LIGHT_KEEP_SUM)
	{
		const float c = RT_LIGHT_KEEP_SUM * (1.0f / 3.0f);
		color[0] = color[1] = color[2] = c;
	}
}
