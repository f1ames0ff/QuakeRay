#ifndef QR_SPATIAL_ADAPTER_H
#define QR_SPATIAL_ADAPTER_H

#include <qray/qray.h>

// The Quake host keeps its native units; this factor is the single source of
// truth for the boundary conversion performed below.
#define QR_QUAKE_UNIT_IN_METERS 0.025f

#ifdef __cplusplus
extern "C" {
#endif

QrResult QRCONV QR_QuakeUploadGeometry(QrInstance instance, const QrGeometryUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeUpdateGeometryTransform(QrInstance instance, const QrUpdateTransformInfo *updateInfo);
QrResult QRCONV QR_QuakeUploadRasterizedGeometry(QrInstance instance,
                                                 const QrRasterizedGeometryUploadInfo *uploadInfo,
                                                 const float *viewProjection, const QrViewport *viewport);
QrResult QRCONV QR_QuakeUploadParticles(QrInstance instance, const QrParticleUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeUploadDecal(QrInstance instance, const QrDecalUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeUploadPortal(QrInstance instance, const QrPortalUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeUploadSphericalLight(QrInstance instance,
                                             const QrSphericalLightUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeUploadSpotLight(QrInstance instance, const QrSpotLightUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeUploadPolygonalLight(QrInstance instance,
                                             const QrPolygonalLightUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeUploadTexturedAreaLight(QrInstance instance,
                                                const QrTexturedAreaLightUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeUploadTexturedAreaLights(QrInstance instance,
                                                 const QrTexturedAreaLightUploadInfo *uploadInfos,
                                                 uint32_t count);
QrResult QRCONV QR_QuakeUploadDtalGroups(QrInstance instance, const QrDtalGroupUploadBatch *uploadInfo);
QrResult QRCONV QR_QuakeUploadClusterLightSources(QrInstance instance,
                                                  const QrClusterLightSourcesUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeBeginClusterLightSources(QrInstance instance,
                                                 const QrClusterLightSourcesUploadInfo *uploadInfo,
                                                 uint32_t sliceCount);
QrResult QRCONV QR_QuakeUploadWorldLights(QrInstance instance, const QrWorldLightsUploadInfo *uploadInfo);
QrResult QRCONV QR_QuakeSetFogVolumes(QrInstance instance, uint32_t count, const QrFogVolume *volumes);
QrResult QRCONV QR_QuakeDrawFrame(QrInstance instance, const QrDrawFrameInfo *drawInfo);

#ifdef __cplusplus
}
#endif

#if !defined(QR_SPATIAL_ADAPTER_IMPLEMENTATION)
#define qrUploadGeometry QR_QuakeUploadGeometry
#define qrUpdateGeometryTransform QR_QuakeUpdateGeometryTransform
#define qrUploadRasterizedGeometry QR_QuakeUploadRasterizedGeometry
#define qrUploadParticles QR_QuakeUploadParticles
#define qrUploadDecal QR_QuakeUploadDecal
#define qrUploadPortal QR_QuakeUploadPortal
#define qrUploadSphericalLight QR_QuakeUploadSphericalLight
#define qrUploadSpotLight QR_QuakeUploadSpotLight
#define qrUploadPolygonalLight QR_QuakeUploadPolygonalLight
#define qrUploadTexturedAreaLight QR_QuakeUploadTexturedAreaLight
#define qrUploadTexturedAreaLights QR_QuakeUploadTexturedAreaLights
#define qrUploadDtalGroups QR_QuakeUploadDtalGroups
#define qrUploadClusterLightSources QR_QuakeUploadClusterLightSources
#define qrBeginClusterLightSources QR_QuakeBeginClusterLightSources
#define qrUploadWorldLights QR_QuakeUploadWorldLights
#define qrSetFogVolumes QR_QuakeSetFogVolumes
#define qrDrawFrame QR_QuakeDrawFrame
#endif

#endif
