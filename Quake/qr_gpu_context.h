#pragma once

#include "qray/qray.h"

#ifdef __cplusplus
extern "C"
{
#endif

QrResult QR_GpuCreateInstance(const QrInstanceCreateInfo *pInfo, QrInstance *pResult);
QrResult QR_GpuValidateSharedContext(QrInstance instance);

#ifdef __cplusplus
}
#endif
