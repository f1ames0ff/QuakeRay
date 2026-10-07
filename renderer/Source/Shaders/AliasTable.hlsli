#ifndef ALIAS_TABLE_HLSLI_
#define ALIAS_TABLE_HLSLI_

int aliasColumn(const float u, const int count)
{
    return min((int)(u * (float)count), count - 1);
}

uint aliasChoice(const float u, const int count, const int column, const float primary, const uint aliasIndex)
{
    const float fraction = u * (float)count - (float)column;

    return (fraction < primary) ? (uint)column : aliasIndex;
}

#endif
