// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

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
