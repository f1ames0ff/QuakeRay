#define QR_SPATIAL_ADAPTER_IMPLEMENTATION
#include "qr_spatial_adapter.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{

constexpr float kQuakeUnitsPerMeter = 1.0f / QR_QUAKE_UNIT_IN_METERS;

template <typename T, int Tag = 0>
std::vector<T> &ThreadScratch()
{
    static thread_local std::vector<T> scratch;
    return scratch;
}

void QuakePointToMeters(float position[3])
{
    for (int axis = 0; axis < 3; axis++)
    {
        position[axis] *= QR_QUAKE_UNIT_IN_METERS;
    }
}

void QuakePointToMeters(QrFloat3D &position)
{
    QuakePointToMeters(position.data);
}

void QuakeTransformTranslationToMeters(QrTransform &transform)
{
    for (int row = 0; row < 3; row++)
    {
        transform.matrix[row][3] *= QR_QUAKE_UNIT_IN_METERS;
    }
}

void QuakeVerticesToMeters(std::vector<QrVertex> &destination, const QrVertex *source, uint32_t count)
{
    destination.assign(source, source + count);

    for (QrVertex &vertex : destination)
    {
        QuakePointToMeters(vertex.position);
    }
}

void QuakeRelativePowerToMeters(QrFloat3D &color)
{
    const float meterArea = QR_QUAKE_UNIT_IN_METERS * QR_QUAKE_UNIT_IN_METERS;
    for (float &channel : color.data)
    {
        channel *= meterArea;
    }
}

void QuakeViewProjectionToMeters(const float source[16], float destination[16])
{
    for (int column = 0; column < 4; column++)
    {
        for (int row = 0; row < 4; row++)
        {
            destination[column * 4 + row] = source[column * 4 + row];

            if (column < 3)
            {
                destination[column * 4 + row] *= kQuakeUnitsPerMeter;
            }
        }
    }
}

void QuakeTransmittancePerUnitToMeters(QrFloat3D &transmittance)
{
    for (float &channel : transmittance.data)
    {
        channel = std::pow(channel, kQuakeUnitsPerMeter);
    }
}

QrResult UploadClusterLightSources(QrInstance instance,
                                   const QrClusterLightSourcesUploadInfo *uploadInfo,
                                   uint32_t sliceCount, bool begin)
{
    if (uploadInfo == nullptr)
    {
        return begin ? qrBeginClusterLightSources(instance, uploadInfo, sliceCount)
                     : qrUploadClusterLightSources(instance, uploadInfo);
    }

    QrClusterLightSourcesUploadInfo converted = *uploadInfo;
    std::vector<QrClusterLightSource> &sources = ThreadScratch<QrClusterLightSource>();

    converted.topUpReach *= QR_QUAKE_UNIT_IN_METERS;

    if (uploadInfo->pLights != nullptr && uploadInfo->numLights > 0)
    {
        sources.assign(uploadInfo->pLights, uploadInfo->pLights + uploadInfo->numLights);

        for (QrClusterLightSource &source : sources)
        {
            QuakePointToMeters(source.origin);
            source.reach *= QR_QUAKE_UNIT_IN_METERS;
            source.radius *= QR_QUAKE_UNIT_IN_METERS;
        }

        converted.pLights = sources.data();
    }

    return begin ? qrBeginClusterLightSources(instance, &converted, sliceCount)
                 : qrUploadClusterLightSources(instance, &converted);
}

}

extern "C" QrResult QRCONV QR_QuakeUploadGeometry(QrInstance instance,
                                                  const QrGeometryUploadInfo *uploadInfo)
{
    if (uploadInfo == nullptr)
    {
        return qrUploadGeometry(instance, uploadInfo);
    }

    QrGeometryUploadInfo converted = *uploadInfo;
    QuakeTransformTranslationToMeters(converted.transform);

    if (converted.passThroughType == QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT)
    {
        converted.layerColors[2].data[1] *= QR_QUAKE_UNIT_IN_METERS;
    }

    if (uploadInfo->pVertices != nullptr && uploadInfo->vertexCount > 0)
    {
        std::vector<QrVertex> &vertices = ThreadScratch<QrVertex>();
        QuakeVerticesToMeters(vertices, uploadInfo->pVertices, uploadInfo->vertexCount);
        converted.pVertices = vertices.data();
    }

    return qrUploadGeometry(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUpdateGeometryTransform(QrInstance instance,
                                                           const QrUpdateTransformInfo *updateInfo)
{
    if (updateInfo == nullptr)
    {
        return qrUpdateGeometryTransform(instance, updateInfo);
    }

    QrUpdateTransformInfo converted = *updateInfo;
    QuakeTransformTranslationToMeters(converted.transform);
    return qrUpdateGeometryTransform(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUploadRasterizedGeometry(
    QrInstance instance, const QrRasterizedGeometryUploadInfo *uploadInfo,
    const float *viewProjection, const QrViewport *viewport)
{
    if (uploadInfo == nullptr ||
        uploadInfo->renderType == QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN)
    {
        return qrUploadRasterizedGeometry(instance, uploadInfo, viewProjection, viewport);
    }

    QrRasterizedGeometryUploadInfo converted = *uploadInfo;
    QuakeTransformTranslationToMeters(converted.transform);
    std::vector<QrVertex> &vertices = ThreadScratch<QrVertex>();

    if (uploadInfo->pVertices != nullptr && uploadInfo->vertexCount > 0)
    {
        QuakeVerticesToMeters(vertices, uploadInfo->pVertices, uploadInfo->vertexCount);
        converted.pVertices = vertices.data();
    }

    if ((uploadInfo->pipelineState & QR_RASTERIZED_GEOMETRY_STATE_SMOKE) != 0)
    {
        if (uploadInfo->pVertices != nullptr && uploadInfo->vertexCount > 0)
        {
            for (QrVertex &vertex : vertices)
            {
                vertex.normal[1] *= QR_QUAKE_UNIT_IN_METERS;
            }
        }

        for (int channel = 0; channel < 3; channel++)
        {
            converted.smokeNoise.data[channel] *= kQuakeUnitsPerMeter;
        }
        converted.smokeNoise.data[3] *= QR_QUAKE_UNIT_IN_METERS;
    }

    float convertedViewProjection[16] = {};
    if (viewProjection != nullptr)
    {
        QuakeViewProjectionToMeters(viewProjection, convertedViewProjection);
        viewProjection = convertedViewProjection;
    }

    return qrUploadRasterizedGeometry(instance, &converted, viewProjection, viewport);
}

extern "C" QrResult QRCONV QR_QuakeUploadParticles(QrInstance instance,
                                                   const QrParticleUploadInfo *uploadInfo)
{
    if (uploadInfo == nullptr || uploadInfo->pPoints == nullptr || uploadInfo->count == 0)
    {
        return qrUploadParticles(instance, uploadInfo);
    }

    QrParticleUploadInfo converted = *uploadInfo;
    std::vector<QrParticlePoint> &points = ThreadScratch<QrParticlePoint>();
    points.assign(uploadInfo->pPoints, uploadInfo->pPoints + uploadInfo->count);

    for (QrParticlePoint &point : points)
    {
        QuakePointToMeters(point.position);
        point.size *= QR_QUAKE_UNIT_IN_METERS;
    }

    converted.pPoints = points.data();
    return qrUploadParticles(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUploadDecal(QrInstance instance, const QrDecalUploadInfo *uploadInfo)
{
    if (uploadInfo == nullptr)
    {
        return qrUploadDecal(instance, uploadInfo);
    }

    QrDecalUploadInfo converted = *uploadInfo;
    QuakeTransformTranslationToMeters(converted.transform);
    return qrUploadDecal(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUploadPortal(QrInstance instance, const QrPortalUploadInfo *uploadInfo)
{
    if (uploadInfo == nullptr)
    {
        return qrUploadPortal(instance, uploadInfo);
    }

    QrPortalUploadInfo converted = *uploadInfo;
    QuakePointToMeters(converted.inPosition);
    QuakePointToMeters(converted.outPosition);
    return qrUploadPortal(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUploadSphericalLight(
    QrInstance instance, const QrSphericalLightUploadInfo *uploadInfo)
{
    if (uploadInfo == nullptr)
    {
        return qrUploadSphericalLight(instance, uploadInfo);
    }

    QrSphericalLightUploadInfo converted = *uploadInfo;
    QuakePointToMeters(converted.position);
    converted.radius *= QR_QUAKE_UNIT_IN_METERS;
    QuakeRelativePowerToMeters(converted.color);
    return qrUploadSphericalLight(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUploadSpotLight(QrInstance instance,
                                                   const QrSpotLightUploadInfo *uploadInfo)
{
    if (uploadInfo == nullptr)
    {
        return qrUploadSpotLight(instance, uploadInfo);
    }

    QrSpotLightUploadInfo converted = *uploadInfo;
    QuakePointToMeters(converted.position);
    converted.radius *= QR_QUAKE_UNIT_IN_METERS;
    QuakeRelativePowerToMeters(converted.color);
    return qrUploadSpotLight(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUploadPolygonalLight(
    QrInstance instance, const QrPolygonalLightUploadInfo *uploadInfo)
{
    if (uploadInfo == nullptr)
    {
        return qrUploadPolygonalLight(instance, uploadInfo);
    }

    QrPolygonalLightUploadInfo converted = *uploadInfo;
    for (QrFloat3D &position : converted.positions)
    {
        QuakePointToMeters(position);
    }
    QuakeRelativePowerToMeters(converted.color);
    return qrUploadPolygonalLight(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUploadTexturedAreaLight(
    QrInstance instance, const QrTexturedAreaLightUploadInfo *uploadInfo)
{
    if (uploadInfo == nullptr)
    {
        return qrUploadTexturedAreaLight(instance, uploadInfo);
    }

    QrTexturedAreaLightUploadInfo converted = *uploadInfo;
    QuakePointToMeters(converted.A);
    QuakePointToMeters(converted.B);
    QuakePointToMeters(converted.C);
    converted.area *= QR_QUAKE_UNIT_IN_METERS * QR_QUAKE_UNIT_IN_METERS;
    return qrUploadTexturedAreaLight(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUploadTexturedAreaLights(
    QrInstance instance, const QrTexturedAreaLightUploadInfo *uploadInfos, uint32_t count)
{
    if (uploadInfos == nullptr || count == 0)
    {
        return qrUploadTexturedAreaLights(instance, uploadInfos, count);
    }

    std::vector<QrTexturedAreaLightUploadInfo> &converted = ThreadScratch<QrTexturedAreaLightUploadInfo>();
    converted.assign(uploadInfos, uploadInfos + count);

    for (QrTexturedAreaLightUploadInfo &light : converted)
    {
        QuakePointToMeters(light.A);
        QuakePointToMeters(light.B);
        QuakePointToMeters(light.C);
        light.area *= QR_QUAKE_UNIT_IN_METERS * QR_QUAKE_UNIT_IN_METERS;
    }

    return qrUploadTexturedAreaLights(instance, converted.data(), count);
}

extern "C" QrResult QRCONV QR_QuakeUploadDtalGroups(QrInstance instance,
                                                    const QrDtalGroupUploadBatch *uploadInfo)
{
    if (uploadInfo == nullptr || uploadInfo->groupCount == 0 ||
        uploadInfo->pGroups == nullptr || uploadInfo->pMembers == nullptr)
    {
        return qrUploadDtalGroups(instance, uploadInfo);
    }

    std::vector<QrDtalGroupUploadInfo> &groups = ThreadScratch<QrDtalGroupUploadInfo>();
    std::vector<QrDtalMemberUpload> &members = ThreadScratch<QrDtalMemberUpload>();
    groups.assign(uploadInfo->pGroups, uploadInfo->pGroups + uploadInfo->groupCount);
    members.assign(uploadInfo->pMembers, uploadInfo->pMembers + uploadInfo->memberCount);

    for (QrDtalGroupUploadInfo &group : groups)
    {
        QuakePointToMeters(group.center);
        QuakePointToMeters(group.boundsMin);
        QuakePointToMeters(group.boundsMax);
        group.area *= QR_QUAKE_UNIT_IN_METERS * QR_QUAKE_UNIT_IN_METERS;
        group.reach *= QR_QUAKE_UNIT_IN_METERS;
        group.boundsRadius *= QR_QUAKE_UNIT_IN_METERS;
    }

    for (QrDtalMemberUpload &member : members)
    {
        QuakePointToMeters(member.A);
        QuakePointToMeters(member.B);
        QuakePointToMeters(member.C);
        member.area *= QR_QUAKE_UNIT_IN_METERS * QR_QUAKE_UNIT_IN_METERS;
    }

    QrDtalGroupUploadBatch converted = *uploadInfo;
    converted.pGroups = groups.data();
    converted.pMembers = members.data();
    return qrUploadDtalGroups(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeUploadClusterLightSources(
    QrInstance instance, const QrClusterLightSourcesUploadInfo *uploadInfo)
{
    return UploadClusterLightSources(instance, uploadInfo, 1, false);
}

extern "C" QrResult QRCONV QR_QuakeBeginClusterLightSources(
    QrInstance instance, const QrClusterLightSourcesUploadInfo *uploadInfo, uint32_t sliceCount)
{
    return UploadClusterLightSources(instance, uploadInfo, sliceCount, true);
}

extern "C" QrResult QRCONV QR_QuakeUploadWorldLights(
    QrInstance instance, const QrWorldLightsUploadInfo *uploadInfo)
{
    if (uploadInfo == nullptr)
    {
        return qrUploadWorldLights(instance, uploadInfo);
    }

    QrWorldLightsUploadInfo converted = *uploadInfo;
    std::vector<QrFloat3D> &clusterMins = ThreadScratch<QrFloat3D, 0>();
    std::vector<QrFloat3D> &clusterMaxs = ThreadScratch<QrFloat3D, 1>();
    std::vector<QrVertex> &faceVertices = ThreadScratch<QrVertex>();

    if (uploadInfo->pClusterMins != nullptr && uploadInfo->pClusterMaxs != nullptr &&
        uploadInfo->numClusters > 0)
    {
        clusterMins.assign(uploadInfo->pClusterMins, uploadInfo->pClusterMins + uploadInfo->numClusters);
        clusterMaxs.assign(uploadInfo->pClusterMaxs, uploadInfo->pClusterMaxs + uploadInfo->numClusters);

        for (QrFloat3D &point : clusterMins)
        {
            QuakePointToMeters(point);
        }
        for (QrFloat3D &point : clusterMaxs)
        {
            QuakePointToMeters(point);
        }

        converted.pClusterMins = clusterMins.data();
        converted.pClusterMaxs = clusterMaxs.data();
    }

    if (uploadInfo->pFaceVertices != nullptr && uploadInfo->numFaceVertices > 0)
    {
        QuakeVerticesToMeters(faceVertices, uploadInfo->pFaceVertices, uploadInfo->numFaceVertices);
        converted.pFaceVertices = faceVertices.data();
    }

    return qrUploadWorldLights(instance, &converted);
}

extern "C" QrResult QRCONV QR_QuakeSetFogVolumes(QrInstance instance, uint32_t count,
                                                 const QrFogVolume *volumes)
{
    if (volumes == nullptr || count == 0)
    {
        return qrSetFogVolumes(instance, count, volumes);
    }

    const uint32_t maxVolumes = static_cast<uint32_t>(QR_MAX_FOG_VOLUMES);
    const uint32_t convertedCount = (count < maxVolumes) ? count : maxVolumes;
    std::vector<QrFogVolume> &converted = ThreadScratch<QrFogVolume>();
    converted.assign(volumes, volumes + convertedCount);

    for (QrFogVolume &volume : converted)
    {
        QuakePointToMeters(volume.pointA);
        QuakePointToMeters(volume.pointB);
        volume.halfExtinctionDistance *= QR_QUAKE_UNIT_IN_METERS;
    }

    return qrSetFogVolumes(instance, convertedCount, converted.data());
}

extern "C" QrResult QRCONV QR_QuakeDrawFrame(QrInstance instance, const QrDrawFrameInfo *drawInfo)
{
    if (drawInfo == nullptr)
    {
        return qrDrawFrame(instance, drawInfo);
    }

    QrDrawFrameInfo converted = *drawInfo;
    converted.view[12] *= QR_QUAKE_UNIT_IN_METERS;
    converted.view[13] *= QR_QUAKE_UNIT_IN_METERS;
    converted.view[14] *= QR_QUAKE_UNIT_IN_METERS;
    converted.cameraNear *= QR_QUAKE_UNIT_IN_METERS;
    converted.cameraFar *= QR_QUAKE_UNIT_IN_METERS;
    converted.rayLength *= QR_QUAKE_UNIT_IN_METERS;

    QrDrawFrameIlluminationParams illumination = {};
    if (drawInfo->pIlluminationParams != nullptr)
    {
        illumination = *drawInfo->pIlluminationParams;
        illumination.cellWorldSize *= QR_QUAKE_UNIT_IN_METERS;
        illumination.sunBounceRange *= QR_QUAKE_UNIT_IN_METERS;
        converted.pIlluminationParams = &illumination;
    }

    QrDrawFrameVolumetricParams volumetric = {};
    if (drawInfo->pVolumetricParams != nullptr)
    {
        volumetric = *drawInfo->pVolumetricParams;
        volumetric.volumetricFar *= QR_QUAKE_UNIT_IN_METERS;
        volumetric.scaterring *= kQuakeUnitsPerMeter;
        converted.pVolumetricParams = &volumetric;
    }

    QrDrawFrameLevelFogParams levelFog = {};
    if (drawInfo->pLevelFogParams != nullptr)
    {
        levelFog = *drawInfo->pLevelFogParams;
        levelFog.density *= kQuakeUnitsPerMeter;
        converted.pLevelFogParams = &levelFog;
    }

    QrDrawFrameReflectRefractParams reflectRefract = {};
    if (drawInfo->pReflectRefractParams != nullptr)
    {
        reflectRefract = *drawInfo->pReflectRefractParams;
        reflectRefract.waterWaveSpeed *= QR_QUAKE_UNIT_IN_METERS;
        reflectRefract.waterTextureAreaScale *= QR_QUAKE_UNIT_IN_METERS;
        QuakeTransmittancePerUnitToMeters(reflectRefract.waterColor);
        QuakeTransmittancePerUnitToMeters(reflectRefract.acidColor);
        converted.pReflectRefractParams = &reflectRefract;
    }

    QrDrawFrameSkyParams sky = {};
    if (drawInfo->pSkyParams != nullptr)
    {
        sky = *drawInfo->pSkyParams;
        QuakePointToMeters(sky.skyViewerPosition);

        if (sky.skyType == QR_SKY_TYPE_PROCEDURAL)
        {
            float *cloudSettings = &sky.skyCubemapRotationTransform.matrix[0][0];
            cloudSettings[7] *= QR_QUAKE_UNIT_IN_METERS;
            cloudSettings[8] *= QR_QUAKE_UNIT_IN_METERS;
        }

        converted.pSkyParams = &sky;
    }

    QrPostEffectsNearDofParams nearDof = {};
    if (drawInfo->postEffectParams.pNearDof != nullptr)
    {
        nearDof = *drawInfo->postEffectParams.pNearDof;
        nearDof.focusDistance *= QR_QUAKE_UNIT_IN_METERS;
        converted.postEffectParams.pNearDof = &nearDof;
    }

    return qrDrawFrame(instance, &converted);
}
