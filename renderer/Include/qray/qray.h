// Copyright (c) 2020-2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef qray_H_
#define qray_H_

#include <stdint.h>

#if defined(_WIN32) && !defined(QR_STATIC)
    #ifdef QR_LIBRARY_EXPORTS
        #define QRAPI __declspec(dllexport)
    #else
        #define QRAPI __declspec(dllimport)
    #endif
    #define QRCONV __cdecl
#else
    // QR_STATIC: the renderer is linked into the host, so the QR_* functions
    // are plain internal calls (no dllimport/dllexport).
    #define QRAPI
    #define QRCONV
#endif // defined(_WIN32) && !defined(QR_STATIC)

#define QR_API_VERSION "1.03.0000"

#ifdef QR_USE_SURFACE_WIN32
    #include <windows.h>
#endif // QR_USE_SURFACE_WIN32
#ifdef QR_USE_SURFACE_METAL
    #ifdef __OBJC__
    @class CAMetalLayer;
    #else
    typedef void CAMetalLayer;
    #endif
#endif // QR_USE_SURFACE_METAL
#ifdef QR_USE_SURFACE_WAYLAND
    #include <wayland-client.h>
#endif // QR_USE_SURFACE_WAYLAND
#ifdef QR_USE_SURFACE_XCB
    #include <xcb/xcb.h>
#endif // QR_USE_SURFACE_XCB
#ifdef QR_USE_SURFACE_XLIB
    #include <X11/Xlib.h>
#endif // QR_USE_SURFACE_XLIB

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(QR_DEFINE_NON_DISPATCHABLE_HANDLE)
    #if defined(__LP64__) || defined(_WIN64) || (defined(__x86_64__) && !defined(__ILP32__) ) || defined(_M_X64) || defined(__ia64) || defined (_M_IA64) || defined(__aarch64__) || defined(__powerpc64__)
        #define QR_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef struct object##_T *object;
    #else
        #define QR_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;
    #endif
#endif

typedef uint32_t QrBool32;
QR_DEFINE_NON_DISPATCHABLE_HANDLE(QrInstance)
typedef uint32_t QrMaterial;
typedef uint32_t QrCubemap;
typedef uint32_t QrFlags;

#define QR_NULL_HANDLE      0
#define QR_NO_MATERIAL      0
#define QR_EMPTY_CUBEMAP    0
#define QR_FALSE            0
#define QR_TRUE             1

typedef enum QrResult
{
    QR_SUCCESS,
    QR_GRAPHICS_API_ERROR,
    QR_CANT_FIND_PHYSICAL_DEVICE,
    QR_WRONG_ARGUMENT,
    QR_TOO_MANY_INSTANCES,
    QR_WRONG_INSTANCE,
    QR_FRAME_WASNT_STARTED,
    QR_FRAME_WASNT_ENDED,
    QR_CANT_UPDATE_TRANSFORM,
    QR_CANT_UPDATE_TEXCOORDS,
    QR_CANT_UPDATE_MATERIAL,
    QR_CANT_UPDATE_ANIMATED_MATERIAL,
    QR_CANT_UPLOAD_RASTERIZED_GEOMETRY,
    QR_WRONG_MATERIAL_PARAMETER,
    QR_WRONG_FUNCTION_CALL,
    QR_ERROR_CANT_FIND_BLUE_NOISE,
    QR_ERROR_CANT_FIND_WATER_TEXTURES,
} QrResult;

typedef void (*PFN_qrPrint)(const char *pMessage, void *pUserData);
typedef void (*PFN_qrOpenFile)(const char *pFilePath, void *pUserData, const void **ppOutData, uint32_t *pOutDataSize, void **ppOutFileUserHandle);
typedef void (*PFN_qrCloseFile)(void *pFileUserHandle, void *pUserData);

typedef struct QrWin32SurfaceCreateInfo QrWin32SurfaceCreateInfo;
typedef struct QrMetalSurfaceCreateInfo QrMetalSurfaceCreateInfo;
typedef struct QrWaylandSurfaceCreateInfo QrWaylandSurfaceCreateInfo;
typedef struct QrXcbSurfaceCreateInfo QrXcbSurfaceCreateInfo;
typedef struct QrXlibSurfaceCreateInfo QrXlibSurfaceCreateInfo;

#ifdef QR_USE_SURFACE_WIN32
typedef struct QrWin32SurfaceCreateInfo
{
    HINSTANCE           hinstance;
    HWND                hwnd;
} QrWin32SurfaceCreateInfo;
#endif // QR_USE_SURFACE_WIN32

#ifdef QR_USE_SURFACE_METAL
typedef struct QrMetalSurfaceCreateInfo
{
    const CAMetalLayer  *pLayer;
} QrMetalSurfaceCreateInfo;
#endif // QR_USE_SURFACE_METAL

#ifdef QR_USE_SURFACE_WAYLAND
typedef struct QrWaylandSurfaceCreateInfo
{
    struct wl_display   *display;
    struct wl_surface   *surface;
} QrWaylandSurfaceCreateInfo;
#endif // QR_USE_SURFACE_WAYLAND

#ifdef QR_USE_SURFACE_XCB
typedef struct QrXcbSurfaceCreateInfo
{
    xcb_connection_t    *connection;
    xcb_window_t        window;
} QrXcbSurfaceCreateInfo;
#endif // QR_USE_SURFACE_XCB

#ifdef QR_USE_SURFACE_XLIB
typedef struct QrXlibSurfaceCreateInfo
{
    Display             *dpy;
    Window              window;
} QrXlibSurfaceCreateInfo;
#endif // QR_USE_SURFACE_XLIB

typedef enum QrTextureSwizzling
{
    QR_TEXTURE_SWIZZLING_ROUGHNESS_METALLIC_EMISSIVE,
    QR_TEXTURE_SWIZZLING_ROUGHNESS_METALLIC,
    QR_TEXTURE_SWIZZLING_METALLIC_ROUGHNESS_EMISSIVE,
    QR_TEXTURE_SWIZZLING_METALLIC_ROUGHNESS,
    QR_TEXTURE_SWIZZLING_NULL_ROUGHNESS_METALLIC,
} QrTextureSwizzling;

typedef struct QrInstanceCreateInfo
{
    // Application name.
    const char                  *pAppName;
    // Application GUID. Generate it for your application and specify it here.
    const char                  *pAppGUID;

    // Exactly one of these surface create infos must be not null.
    QrWin32SurfaceCreateInfo    *pWin32SurfaceInfo;
    QrMetalSurfaceCreateInfo    *pMetalSurfaceCreateInfo;
    QrWaylandSurfaceCreateInfo  *pWaylandSurfaceCreateInfo;
    QrXcbSurfaceCreateInfo      *pXcbSurfaceCreateInfo;
    QrXlibSurfaceCreateInfo     *pXlibSurfaceCreateInfo;

    // Path to the development configuration file. It's read line by line. Case-insensitive.
    // "VulkanValidation"   - validate each Vulkan API call and print using pfnPrint
    // "Developer"          - load PNG texture files instead of KTX2; reload a texture if its PNG file was changed
    // "FPSMonitor"         - show FPS at the window name
    const char                  *pConfigPath;
    
    // Optional function to print messages from the library.
    // Requires "VulkanValidation" in the configuration file.
    PFN_qrPrint                 pfnPrint;
    // Custom user data that is passed to pfnUserPrint.
    void                        *pUserPrintData;

    const char                  *pShaderFolderPath;
    // Path to the file with 128 layers of uncompressed 128x128 blue noise images.
    const char                  *pBlueNoiseFilePath;
    // Optional function to load files: shaders, blue noise and overriden textures.
    // If null, files will be opened with standard methods. pfnLoadFile is very simple,
    // as it requires file data (ppOutData, pOutDataSize) to be fully loaded to the memory.
    // The value, ppOutFileUserHandle point on, will be passed to PFN_qrCloseFile.
    // So for example, it can be a file handle.
    PFN_qrOpenFile              pfnOpenFile;
    PFN_qrCloseFile             pfnCloseFile;
    // Custom user data that is passed to pfnUserLoadFile.
    void                        *pUserLoadFileData;

    // How many texture layers should be used to get albedo color for primary rays / indrect illumination.
    uint32_t                    primaryRaysMaxAlbedoLayers;
    uint32_t                    indirectIlluminationMaxAlbedoLayers;

    QrBool32                    rayCullBackFacingTriangles;
    // Allow QR_GEOMETRY_VISIBILITY_TYPE_SKY.
    // If true, QR_GEOMETRY_VISIBILITY_TYPE_WORLD_2 must not be used.
    QrBool32                    allowGeometryWithSkyFlag;

    // Memory that must be allocated for vertex and index buffers of rasterized geometry.
    // It can't be changed after qrCreateInstance.
    // If buffer is full, rasterized data will be ignored
    uint32_t                    rasterizedMaxVertexCount;
    uint32_t                    rasterizedMaxIndexCount;
    // Apply gamma correction to packed rasterized vertex colors.
    QrBool32                    rasterizedVertexColorGamma;

    // Size of a cubemap side to render rasterized sky in.
    uint32_t                    rasterizedSkyCubemapSize;  

    // Max amount of textures to be used during the execution.
    // The value is clamped to [1024..4096]
    uint32_t                    maxTextureCount;
    // If true, 'filter' in QrMaterialCreateInfo, QrCubemapCreateInfo
    // will set only magnification filter.
    QrBool32                    textureSamplerForceMinificationFilterLinear;
    QrBool32                    textureSamplerForceNormalMapFilterLinear;

    // The folder to find overriding textures in.
    const char                  *pOverridenTexturesFolderPath;
    // If not null and the configuration file contains "Developer",
    // this path is used instead of pOverridenTexturesFolderPath.
    const char                  *pOverridenTexturesFolderPathDeveloper;
    // Postfixes will be used to determine textures that should be 
    // loaded from files if the texture should be overridden
    // i.e. if postfix="_n" then "Floor_01.*" => "Floor_01_n.*", 
    // where "*" is some image extension
    // If null, then empty string will be used.
    const char                  *pOverridenAlbedoAlphaTexturePostfix;
    // If null, then "_rme" will be used.
    const char                  *pOverridenRoughnessMetallicEmissionTexturePostfix;
    // If null, then "_n" will be used.
    const char                  *pOverridenNormalTexturePostfix;

    QrBool32                    originalAlbedoAlphaTextureIsSRGB;
    QrBool32                    originalRoughnessMetallicEmissionTextureIsSRGB;
    QrBool32                    originalNormalTextureIsSRGB;

    QrBool32                    overridenAlbedoAlphaTextureIsSRGB;
    QrBool32                    overridenRoughnessMetallicEmissionTextureIsSRGB;
    QrBool32                    overridenNormalTextureIsSRGB;

    // Path to normal texture path. Ignores pOverridenTexturesFolderPath and pOverridenNormalTexturePostfix
    const char                  *pWaterNormalTexturePath;

    QrBool32                    lensFlareVerticesInScreenSpace;
    // If true, 'pointToCheck' XY are screen space [0..1] coordinates to check NDC depth [0..1] which is specified in Z.
    // Otherwise, XYZ specify a world point to which view-projection will be applied to determine its
    // screen space coords and NDC depth.
    QrBool32                    lensFlarePointToCheckIsInScreenSpace;

    QrTextureSwizzling          pbrTextureSwizzling;

    QrBool32                    effectWipeIsUsed;
} QrInstanceCreateInfo;

QRAPI QrResult QRCONV qrCreateInstance(
    const QrInstanceCreateInfo          *pInfo,
    QrInstance                          *pResult);

QRAPI QrResult QRCONV qrDestroyInstance(
    QrInstance                          qrInstance);



typedef struct QrLayeredMaterial
{
    // Geometry can have up to 3 materials, QR_NO_MATERIAL is no material.
    QrMaterial  layerMaterials[3];
} QrLayeredMaterial;

typedef enum QrGeometryType
{
    QR_GEOMETRY_TYPE_STATIC,
    QR_GEOMETRY_TYPE_STATIC_MOVABLE,
    QR_GEOMETRY_TYPE_DYNAMIC
} QrGeometryType;

typedef enum QrGeometryPassThroughType
{
    QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
    QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED,
    QR_GEOMETRY_PASS_THROUGH_TYPE_MIRROR,
    QR_GEOMETRY_PASS_THROUGH_TYPE_PORTAL,
    QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_ONLY_REFLECT,
    QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT,
    QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT,
    QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT,
} QrGeometryPassThroughType;

typedef enum QrGeometryPrimaryVisibilityType
{
    QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
    QR_GEOMETRY_VISIBILITY_TYPE_WORLD_1,
    QR_GEOMETRY_VISIBILITY_TYPE_WORLD_2,
    QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON,
    QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER,
    // If ray hits this geometry, then pretend like it was a miss (i.e. fetch sky info)
    QR_GEOMETRY_VISIBILITY_TYPE_SKY,
} QrGeometryPrimaryVisibilityType;

typedef enum QrGeometryMaterialBlendType
{
    QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE,
    QR_GEOMETRY_MATERIAL_BLEND_TYPE_ALPHA,
    QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD,
    QR_GEOMETRY_MATERIAL_BLEND_TYPE_SHADE
} QrGeometryMaterialBlendType;

// Row-major transformation matrix.
typedef struct QrTransform
{
    float       matrix[3][4];
} QrTransform;

typedef struct QrMatrix3D
{
    float       matrix[3][3];
} QrMatrix3D;

typedef struct QrFloat2D
{
    float       data[2];
} QrFloat2D;

typedef struct QrFloat3D
{
    float       data[3];
} QrFloat3D;

typedef struct QrFloat4D
{
    float       data[4];
} QrFloat4D;

typedef struct QrVertex
{
    float       position[3];        uint32_t _padding0;
    float       normal[3];          uint32_t _padding1;
    float       texCoord[2];
    float       texCoordLayer1[2];
    float       texCoordLayer2[2];
    // RGBA packed into 32-bit uint. R component is at the little end, i.e. (a<<24 | b<<16 | g<<8 | r)
    uint32_t    packedColor;
    uint32_t    cluster;
    uint32_t    lightStyles;
    uint32_t    _padding2[3];
} QrVertex;

typedef enum QrGeometryUploadFlagBits
{
    QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT = 1,
    QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT = 2,
    QR_GEOMETRY_UPLOAD_GENERATE_INVERTED_NORMALS_BIT = 4,
    // Set this flag if on the both sides of polygons the media is the same.
    // For example, waterfall geometry represented by one flat square,
    // so on both sides is air media.
    QR_GEOMETRY_UPLOAD_NO_MEDIA_CHANGE_ON_REFRACT_BIT = 8,
    // Multiply the thoughput by albedo on reflection / refraction.
    // E.g. mirror has some texture on it. 
    QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT = 16,
    QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_ADD_BIT = 32,
    // If hit the geometry with this flag, ignore refract geometry after.
    QR_GEOMETRY_UPLOAD_IGNORE_REFRACT_AFTER_REFRACT_BIT = 64,
    // Animate the texture coordinates of the turbulent surfaces (lava, teleport)
    // with the "warp" that the classic engine used for them.
    QR_GEOMETRY_UPLOAD_TURB_WARP_BIT = 128,
} QrGeometryUploadFlagBits;
typedef QrFlags QrGeometryUploadFlags;

typedef struct QrGeometryUploadInfo
{
    uint64_t                        uniqueID;
    QrGeometryUploadFlags           flags;

    QrGeometryType                  geomType;
    QrGeometryPassThroughType       passThroughType;
    QrGeometryPrimaryVisibilityType visibilityType;

    uint32_t                        vertexCount;
    const QrVertex                  *pVertices;
    
    // Can be null, if indices are not used.
    // pIndices is an array of uint32_t of size indexCount.
    uint32_t                        indexCount;
    const uint32_t                  *pIndices;

    // Look QrPortalUploadInfo.
    // Must be null if not QR_GEOMETRY_PASS_THROUGH_TYPE_PORTAL.
    // Must be non-null if QR_GEOMETRY_PASS_THROUGH_TYPE_PORTAL.
    uint8_t                         *pPortalIndex;

    // RGBA color for each material layer.
    QrFloat4D                       layerColors[3];
    QrGeometryMaterialBlendType     layerBlendingTypes[3];
    // These default values will be used if no overriding 
    // texture is found. Clamped to [0,1].
    float                           defaultRoughness;
    float                           defaultMetallicity;
    // Emission = defaultEmission * color
    float                           defaultEmission;

    QrLayeredMaterial               geomMaterial;
    QrTransform                     transform;
} QrGeometryUploadInfo;

typedef struct QrUpdateTransformInfo
{
    uint64_t        movableStaticUniqueID;
    QrTransform     transform;
} QrUpdateTransformInfo;

typedef struct QrUpdateTexCoordsInfo
{
    // movable or non-movable static unique geom ID
    uint64_t        staticUniqueID;
    uint32_t        vertexOffset;
    uint32_t        vertexCount;
    // If an array member is null, then texture coordinates
    // won't be updated for that layer.
    const void      *pTexCoordLayerData[3];
} QrUpdateTexCoordsInfo;


// Uploaded dynamic geometry can only be visible in the current frame, i.e.
// dynamic geometry must be uploaded each frame.
// Uploaded static geometriy can only be visible after submitting them using qrSubmitStaticGeometries.
// Dynamic geometry can be uploaded only between qrStartFrame - qrDrawFrame.
// Static geometry can be uploaded only between qrBeginStaticGeometries - qrSubmitStaticGeometries.
// Uploading dynamic geometries and then calling qrBeginStaticGeometries will erase them.
QRAPI QrResult QRCONV qrUploadGeometry(
    QrInstance                              qrInstance,
    const QrGeometryUploadInfo              *pUploadInfo);

// Updating transform is available only for movable static geometry.
// Other geometry types don't need it because they are either fully static
// or uploaded every frame, so transforms are always as they are intended.
QRAPI QrResult QRCONV qrUpdateGeometryTransform(
    QrInstance                              qrInstance,
    const QrUpdateTransformInfo             *pUpdateInfo);

QRAPI QrResult QRCONV qrUpdateGeometryTexCoords(
    QrInstance                              qrInstance,
    const QrUpdateTexCoordsInfo             *pUpdateInfo);



// Clear current scene from all static geometries and make it available for recording new geometries.
// New scene can be visible only after the submission using qrSubmitStaticGeometries.
QRAPI QrResult QRCONV qrBeginStaticGeometries(
    QrInstance                          qrInstance);

// After uploading all static geometry, scene must be submitted before rendering.
// Note that movable static geometry can be still moved using qrUpdateGeometryTransform.
// If the static scene geometry should be changed, it must be cleared using qrBeginStaticGeometries
// and new static geometries must be uploaded.
// To clear static scene, call qrBeginStaticGeometries and then qrSubmitStaticGeometries
// without uploading any geometry.
// qrBeginStaticGeometries and qrSubmitStaticGeometries can be called outside of qrStartFrame-qrDrawFrame.
QRAPI QrResult QRCONV qrSubmitStaticGeometries(
    QrInstance                          qrInstance);



typedef enum QrBlendFactor
{
    QR_BLEND_FACTOR_ONE,
    QR_BLEND_FACTOR_ZERO,
    QR_BLEND_FACTOR_SRC_COLOR,
    QR_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    QR_BLEND_FACTOR_DST_COLOR,
    QR_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    QR_BLEND_FACTOR_SRC_ALPHA,
    QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
} QrBlendFactor;

// DEFAULT:     The rendering will be done with the resolution
//              (renderWidth, renderHeight) that is set in QrDrawFrameInfo.
//              Examples: particles, semitransparent world objects.
// SWAPCHAIN:   Swapchain's resolution will be used.
//              Note: "depthTest" and "depthWrite" must be false.
//              Examples: HUD
// SKY:         Geometry will be drawn to the background of ray-traced image
//              if skyType is QR_SKY_TYPE_RASTERIZED_GEOMETRY.
//              Also, the cubemap for this kind of geometry will be created
//              for specular and indirect bounces.
typedef enum QrRasterizedGeometryRenderType
{
    QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
    QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN,
    QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY
} QrRasterizedGeometryRenderType;

typedef enum QrRasterizedGeometryStateFlagBits
{
    QR_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST         = 1,
    QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE       = 2,
    QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST         = 4,
    QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE        = 8,
    QR_RASTERIZED_GEOMETRY_STATE_FORCE_LINE_LIST    = 16,
    QR_RASTERIZED_GEOMETRY_STATE_SMOKE              = 32,
} QrRasterizedGeometryStateFlagBits;
typedef uint32_t QrRasterizedGeometryStateFlags;

typedef struct QrRasterizedGeometryUploadInfo
{
    QrRasterizedGeometryRenderType          renderType;

    uint32_t                                vertexCount;
    const QrVertex                          *pVertices;
    
    // Can be 0/null.
    // indexData is an array of uint32_t of size indexCount.
    uint32_t                                indexCount;
    const void                              *pIndices;

    QrTransform                             transform;

    QrFloat4D                               color;
    // Only the albedo-alpha texture is used for rasterized geometry.
    QrMaterial                              material;
    QrRasterizedGeometryStateFlags          pipelineState;
    QrBlendFactor                           blendFuncSrc;
    QrBlendFactor                           blendFuncDst;

    QrFloat4D                               smokeNoise;
    QrFloat4D                               smokeLook;
} QrRasterizedGeometryUploadInfo;



typedef struct QrExtent2D
{
    uint32_t    width;
    uint32_t    height;
} QrExtent2D;

typedef struct QrExtent3D
{
    uint32_t    width;
    uint32_t    height;
    uint32_t    depth;
} QrExtent3D;

// Struct is used to transform from NDC to window coordinates.
// x, y, width, height are specified in pixels. (x,y) defines top-left corner.
typedef struct QrViewport
{
    float       x;
    float       y;
    float       width;
    float       height;
    float       minDepth;
    float       maxDepth;
} QrViewport;

// Upload geometry that will be drawn using rasterization.
// Whole buffer for such geometry be discarded after frame finish.
// "viewProjection" -- 4x4 view-projection matrix to apply to the rasterized
//                     geometry. Matrix is column major. If it's null,
//                     then the matrices from QrDrawFrameInfo are used.
// "viewport"       -- pointer to a viewport to draw in. If it's null,
//                     then the fullscreen one is used with minDepth 0.0
//                     and maxDepth 1.0.
QRAPI QrResult QRCONV qrUploadRasterizedGeometry(
    QrInstance                              qrInstance,
    const QrRasterizedGeometryUploadInfo    *pUploadInfo,
    const float                             *pViewProjection,
    const QrViewport                        *pViewport);



typedef struct QrDecalUploadInfo
{
    // Transformation from [-0.5, 0.5] cube to a scaled oriented box.
    // Orientation should transform (0,0,1) to decal's normal.
    QrTransform     transform;
    QrMaterial      material;
} QrDecalUploadInfo;

QRAPI QrResult QRCONV qrUploadDecal(
    QrInstance                              qrInstance,
    const QrDecalUploadInfo                 *pUploadInfo);



typedef struct QrPortalUploadInfo
{
    // Index to specify in QrGeometryUploadInfo.
    // Must be in [0, 62].
    uint8_t         portalIndex;
    QrFloat3D       inPosition;
    QrFloat3D       outPosition;
    QrFloat3D       outDirection;
    QrFloat3D       outUp;
} QrPortalUploadInfo;

QRAPI QrResult QRCONV qrUploadPortal(
    QrInstance                              qrInstance,
    const QrPortalUploadInfo                *pUploadInfo);



typedef struct QrDirectionalLightUploadInfo
{
    // Used to match the same light source from the previous frame.
    uint64_t        uniqueID;
    QrFloat3D       color;
    QrFloat3D       direction;
    float           angularDiameterDegrees;
} QrDirectionalLightUploadInfo;

typedef struct QrSphericalLightUploadInfo
{
    // Used to match the same light source from the previous frame.
    uint64_t        uniqueID;
    QrFloat3D       color;
    QrFloat3D       position;
    // Sphere radius.
    float           radius;
    QrFloat3D       normal;
} QrSphericalLightUploadInfo;

typedef struct QrPolygonalLightUploadInfo
{
    // Used to match the same light source from the previous frame.
    uint64_t        uniqueID;
    QrFloat3D       color;
    QrFloat3D       positions[3];
} QrPolygonalLightUploadInfo;

#define MAX_TEXTURED_AREA_LIGHT_VERTS 8
typedef struct QrTexturedAreaLightUploadInfo
{
    // Used to match the same light source from the previous frame.
    uint64_t        uniqueID;
    QrFloat3D       color;
    QrFloat3D       A;
    QrFloat3D       B;
    QrFloat3D       C;
    QrFloat3D       normal;
    float           area;
    int             numVerts;
    QrFloat2D       uvVerts[MAX_TEXTURED_AREA_LIGHT_VERTS];
    // Emission texture of the light. QR_NO_MATERIAL when the whole polygon glows evenly,
    // and then meanEmiss is the emission. With a material the light samples the mask of
    // that texture at the point it picks, and meanEmiss is the mean of that mask.
    QrMaterial      material;
    float           meanEmiss;
    // Whether A/B/C are a fit of the surface's own uv. Recorded by the host for its own
    // bookkeeping and not sent to the renderer: the shader always places its point as
    // C + A * u + B * v, whatever the fit was.
    int             fit;
    int             isStatic;
} QrTexturedAreaLightUploadInfo;

// Only one spotlight is available in a scene.
typedef struct QrSpotLightUploadInfo
{
    // Used to match the same light source from the previous frame.
    uint64_t        uniqueID;
    QrFloat3D       color;
    QrFloat3D       position;
    QrFloat3D       direction;
    float           radius;
    // Light source disk radius.
    // Inner cone angle. In radians.
    float           angleOuter;
    // Outer cone angle. In radians.
    float           angleInner;
} QrSpotLightUploadInfo;

QRAPI QrResult QRCONV qrUploadDirectionalLight(
    QrInstance                          qrInstance,
    const QrDirectionalLightUploadInfo  *pUploadInfo);

QRAPI QrResult QRCONV qrUploadSphericalLight(
    QrInstance                          qrInstance,
    const QrSphericalLightUploadInfo    *pUploadInfo);

QRAPI QrResult QRCONV qrUploadSpotLight(
    QrInstance                          qrInstance,
    const QrSpotLightUploadInfo         *pUploadInfo);

QRAPI QrResult QRCONV qrUploadPolygonalLight(
    QrInstance                          qrInstance,
    const QrPolygonalLightUploadInfo    *pUploadInfo);

QRAPI QrResult QRCONV qrUploadTexturedAreaLight(
    QrInstance                          qrInstance,
    const QrTexturedAreaLightUploadInfo *pUploadInfo);

// Equivalent to qrUploadTexturedAreaLight() for each of the count lights, in order, with the
// per-instance and per-upload work paid once for the batch.
QRAPI QrResult QRCONV qrUploadTexturedAreaLights(
    QrInstance                          qrInstance,
    const QrTexturedAreaLightUploadInfo *pUploadInfos,
    uint32_t                            count);


// Cluster of a light the host could not place in an open leaf.
#define QR_CLUSTER_LIGHT_NO_CLUSTER    (~0u)

// A light the renderer has to sample. The host registers one per light it uploads and
// supplies the things the renderer cannot derive: which leaf the origin resolved into,
// the origin itself, in the same Quake units as the cluster bounds of
// QrWorldLightsUploadInfo, and how far the light still matters.
typedef struct QrClusterLightSource
{
    uint64_t  uniqueID;
    QrFloat3D origin;
    // QR_CLUSTER_LIGHT_NO_CLUSTER when the origin resolved into no open leaf.
    uint32_t  cluster;
    // Distance from the origin up to which the light still belongs in a cluster list, in
    // Quake units. A light is registered by a moving entity and every cluster within this
    // distance reaches it, whatever its PVS says: a light that reaches across leaf
    // boundaries is what keeps a flame lighting the wall it stands against. Zero or less
    // means the light states no reach of its own and reaches wherever its leaf sees, which
    // is what a light of the map itself wants, its own PVS row being its reach. A positive
    // value is a promise from the host that the light contributes nothing beyond it, and
    // the renderer holds the light to it: a light whose reach covers a few rooms instead
    // of a map is what keeps a single moving entity from reaching every list of the scene.
    float     reach;
} QrClusterLightSource;

typedef struct QrClusterLightSourcesUploadInfo
{
    uint32_t                    numLights;
    const QrClusterLightSource *pLights;
    // Reach of the pass that tops a cluster up with lights its own PVS hides, in Quake
    // units: such a light is taken when it is closer to the cluster bounds than this. It
    // also caps the reach a light may state for itself in QrClusterLightSource.
    float                       topUpReach;
    // Zero keeps the composition all or nothing: a frame whose light set is unchanged takes
    // every light and every PVS row again as soon as one of them moved. Nonzero lets such a
    // frame take back the slots of the lights that moved, hand them out again from where
    // those lights stand now, and look at the top-up set of the clusters that lost or can
    // gain one, leaving every other list of the scene as the composition left it.
    int32_t                     allowIncremental;
} QrClusterLightSourcesUploadInfo;

// Registers the lights of the current frame. The renderer composes the per-cluster lists
// out of them and hands them to the light manager itself, so the host never sees a list.
QRAPI QrResult QRCONV qrUploadClusterLightSources(
    QrInstance                              qrInstance,
    const QrClusterLightSourcesUploadInfo   *pUploadInfo);

// What the last composed frame did, for the host's statistics panel.
typedef struct QrClusterLightStats
{
    // Clusters of the map and sources of the last frame.
    uint32_t clusters;
    uint32_t sources;
    // Sources that resolved into no open leaf, and so hold no slot anywhere.
    uint32_t unresolved;
    // Entries the lists hold, and (light, cluster) pairs the passes granted and rejected:
    // a rejection means that the cluster had filled all of its slots.
    uint32_t listEntries;
    uint32_t grants;
    uint32_t denied;
    // Slots granted by the reach pass of the last frame.
    uint32_t topUpGrants;
    // (Light, cluster) pairs pass one left out because the cluster stands beyond the reach
    // the light states for itself. Zero when no light of the frame states one.
    uint32_t reachGated;
    // Sources whose PVS walk ran on this frame and sources whose walk the cache reused.
    uint32_t walkedSources;
    uint32_t cachedSources;
    // Sources that appeared, disappeared and changed leaf since the previous frame.
    uint32_t addedSources;
    uint32_t removedSources;
    uint32_t movedSources;
    // Frames that composed the lists and frames that found the sources unchanged.
    uint32_t composedFrames;
    uint32_t reusedFrames;
    // Clusters that filled every slot of their list.
    uint32_t fullClusters;
    // Where the time of the last frame went, in milliseconds: the PVS walk of pass one, the
    // reach pass, the compaction of the slots into the lists, the publication to the light
    // manager, and the whole call. A frame that reused the composition only pays the last two.
    float    visMs;
    float    topUpMs;
    float    fillMs;
    float    publishMs;
    float    totalMs;
} QrClusterLightStats;

QRAPI QrResult QRCONV qrGetClusterLightStats(
    QrInstance              qrInstance,
    QrClusterLightStats    *pStats);

// Slot accounting of every source of the last composed frame, in the order the sources
// were uploaded. Either output array may be null when only the count is wanted.
QRAPI QrResult QRCONV qrGetClusterLightGrants(
    QrInstance  qrInstance,
    uint32_t   *pGranted,
    uint32_t   *pDenied,
    uint32_t    maxCount,
    uint32_t   *pCount);

// One cluster list as the last composed frame left it: exactly the lights that frame gave
// the cluster, with no invalid entries, and no more than the renderer's per-list limit.
QRAPI QrResult QRCONV qrGetClusterLightList(
    QrInstance  qrInstance,
    uint32_t    cluster,
    uint64_t   *pLightUniqueIds,
    uint32_t    maxCount,
    uint32_t   *pCount);


// A face of the world (or of an inline brush model, such as a door) that the renderer
// turns into light polygons. Corners of every face live in one shared array.
typedef enum QrWorldLightFaceFlags
{
    // Only a part of the emission texture glows, so the polygon has to be clipped
    // against the bounding box of the bright texels.
    QR_WORLD_LIGHT_FACE_MASKED_BIT       = 1 << 0,
    // The face belongs to an inline brush model, not to the world itself.
    QR_WORLD_LIGHT_FACE_INLINE_MODEL_BIT = 1 << 1,
} QrWorldLightFaceFlags;

typedef struct QrWorldLightFace
{
    // Same id the face geometry is uploaded with, so a light keeps its identity.
    uint64_t  uniqueID;
    uint32_t  firstVertex;
    uint32_t  numVertices;
    // BSP leaf the face stands in, the same number QrVertex::cluster carries.
    uint32_t  cluster;
    // QrWorldLightFaceFlags
    uint32_t  flags;
    // Emission texture of the light. QR_NO_MATERIAL when the whole texture glows.
    QrMaterial material;
    QrFloat3D  color;
    float      meanEmiss;
    // 0 for faces the host still moves and re-uploads every frame.
    uint32_t   isStatic;
} QrWorldLightFace;

typedef enum QrWorldLightsUploadFlags
{
    // Print what was accepted, so a manual test can compare the renderer's tables
    // against the host's own. The host sets it from a cvar of its own.
    QR_WORLD_LIGHTS_UPLOAD_PRINT_STATS_BIT = 1 << 0,
} QrWorldLightsUploadFlags;

typedef enum QrWorldClusterFlags
{
    // Leaf is CONTENTS_SOLID. No light is visible from it, so the light list builder
    // skips it entirely.
    QR_WORLD_CLUSTER_SOLID_BIT = 1 << 0,
} QrWorldClusterFlags;

typedef struct QrWorldLightsUploadInfo
{
    // QrWorldLightsUploadFlags
    uint32_t flags;

    // One entry per BSP leaf of the world.
    uint32_t         numClusters;
    const QrFloat3D *pClusterMins;
    const QrFloat3D *pClusterMaxs;
    // QrWorldClusterFlags, one byte per cluster.
    const uint8_t   *pClusterFlags;

    // Emissive faces with their corners in world space, as raw Quake vertexes.
    uint32_t                numFaces;
    const QrWorldLightFace *pFaces;
    uint32_t                numFaceVertices;
    const QrVertex         *pFaceVertices;

    // Compressed Quake PVS of the world: pVisOffsets[cluster] is a byte offset into
    // pVisData, or -1 when that cluster has no row. Rows are pvsRowBytes long.
    const uint8_t *pVisData;
    uint32_t       visDataSize;
    uint32_t       pvsRowBytes;
    const int32_t *pVisOffsets;

    // Per-cluster sky visibility, ceil(numClusters / 8) bytes: bit c of byte c/8 is 1
    // when a sun ray from cluster c can still reach the sky (the PVS union of every
    // cluster that holds sky). May be NULL, which leaves every cluster tracing.
    const uint8_t *pClusterSkyVisibility;
} QrWorldLightsUploadInfo;

QRAPI QrResult QRCONV qrUploadWorldLights(
    QrInstance                    qrInstance,
    const QrWorldLightsUploadInfo *pUploadInfo);



typedef enum QrSamplerAddressMode
{
    QR_SAMPLER_ADDRESS_MODE_REPEAT,
    QR_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
    QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
    QR_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
} QrSamplerAddressMode;

typedef enum QrSamplerFilter
{
    QR_SAMPLER_FILTER_LINEAR,
    QR_SAMPLER_FILTER_NEAREST,
} QrSamplerFilter;

typedef struct QrTextureSet
{
    const void *pDataAlbedoAlpha;
    const void *pDataRoughnessMetallicEmission;
    const void *pDataNormal;
} QrTextureSet;

typedef enum QrMaterialCreateFlagBits
{
    // If set, mipmaps will be generated by the library.
    QR_MATERIAL_CREATE_DONT_GENERATE_MIPMAPS_BIT = 1,
    // Force to use lowest mip level while rendering
    QR_MATERIAL_CREATE_FORCE_LOWEST_MIP_BIT = 2,
    // If set, sampler will be controlled with QrDrawFrameTexturesParams::dynamicSamplerFilter.
    QR_MATERIAL_CREATE_DYNAMIC_SAMPLER_FILTER_BIT = 4,
    // If set, "qrUpdateMaterialContents" can be used to update textures of the material.
    // Tip: set pRelativePath as null, to avoid access to the filesystem.
    QR_MATERIAL_CREATE_UPDATEABLE_BIT = 8,
} QrMaterialCreateFlagBits;
typedef QrFlags QrMaterialCreateFlags;

typedef struct QrMaterialCreateInfo
{
    QrMaterialCreateFlags   flags;
    // If data is used then size must specify width and height.
    // "data" must be (width * height * 4) bytes.
    // Can be (0,0).
    QrExtent2D              size;
    // Only R8G8B8A8 textures.
    // Firstly, the library will try to find image file using "relativePath",
    // and if nothing is found "data" is used. Additional overriding data
    // such as normal, metallic, roughness, emission maps will be loaded
    // using "relativePath" and overriding postfixes.
    QrTextureSet            textures;
    // "relativePath" must be in the following format:
    //      "<folders>/<name>.<extension>"
    // where "<folders>/" and ".<extension>" can be empty.
    // The library will try to find image files using path:
    //      "<overridenTexturesFolderPath><folders>/<name>.ktx2"
    // Image files must be in KTX2 format.
    // If null, then texture overriding is disabled.
    const char              *pRelativePath;
    QrSamplerFilter         filter;
    QrSamplerAddressMode    addressModeU;
    QrSamplerAddressMode    addressModeV;
} QrMaterialCreateInfo;

typedef struct QrMaterialUpdateInfo
{
    QrMaterial              target;
    QrTextureSet            textures;
} QrMaterialUpdateInfo;

typedef struct QrAnimatedMaterialCreateInfo
{
    uint32_t                            frameCount;
    QrMaterialCreateInfo                *pFrames;
} QrAnimatedMaterialCreateInfo;

QRAPI QrResult QRCONV qrCreateMaterial(
    QrInstance                          qrInstance,
    const QrMaterialCreateInfo          *pCreateInfo,
    QrMaterial                          *pResult);

QRAPI QrResult QRCONV qrCreateAnimatedMaterial(
    QrInstance                          qrInstance,
    const QrAnimatedMaterialCreateInfo  *pCreateInfo,
    QrMaterial                          *pResult);

QRAPI QrResult QRCONV qrChangeAnimatedMaterialFrame(
    QrInstance                          qrInstance,
    QrMaterial                          animatedMaterial,
    uint32_t                            frameIndex);

QRAPI QrResult QRCONV qrUpdateMaterialContents(
    QrInstance                          qrInstance,
    const QrMaterialUpdateInfo          *pUpdateInfo);

// Destroying QR_NO_MATERIAL has no effect.
QRAPI QrResult QRCONV qrDestroyMaterial(
    QrInstance                          qrInstance,
    QrMaterial                          material);



typedef struct QrCubemapFaceData
{
    const void *pPositiveX;
    const void *pNegativeX;
    const void *pPositiveY;
    const void *pNegativeY;
    const void *pPositiveZ;
    const void *pNegativeZ;
} QrCubemapFaceData;

typedef struct QrCubemapFacePaths
{
    const char *pPositiveX;
    const char *pNegativeX;
    const char *pPositiveY;
    const char *pNegativeY;
    const char *pPositiveZ;
    const char *pNegativeZ;
} QrCubemapFacePaths;

typedef struct QrCubemapCreateInfo
{
    union
    {
        const void          *pData[6];
        QrCubemapFaceData   dataFaces;
    };

    // Overriding paths for each cubemap face.
    union
    {
        const char          *pRelativePaths[6];
        QrCubemapFacePaths  relativePathFaces;
    };

    // width = height = sideSize
    uint32_t                sideSize;
    QrBool32                useMipmaps;
    QrSamplerFilter         filter;
} QrCubemapCreateInfo;

QRAPI QrResult QRCONV qrCreateCubemap(
    QrInstance                          qrInstance,
    const QrCubemapCreateInfo           *pCreateInfo,
    QrCubemap                           *pResult);

QRAPI QrResult QRCONV qrDestroyCubemap(
    QrInstance                          qrInstance,
    QrCubemap                           cubemap);



typedef struct QrStartFrameInfo
{
    QrBool32        requestVSync;
    QrBool32        requestShaderReload;
} QrStartFrameInfo;

QRAPI QrResult QRCONV qrStartFrame(
    QrInstance                          qrInstance,
    const QrStartFrameInfo              *pStartInfo);

typedef enum QrSkyType
{
    QR_SKY_TYPE_COLOR,
    QR_SKY_TYPE_CUBEMAP,
    QR_SKY_TYPE_RASTERIZED_GEOMETRY,
    // Procedural atmospheric sky. The sky cubemap is filled by a compute
    // pass using the current directional light (sun) direction/color.
    QR_SKY_TYPE_PROCEDURAL
} QrSkyType;

typedef struct QrDrawFrameTonemappingParams
{
    float       minLogLuminance;
    float       maxLogLuminance;
    float       luminanceWhitePoint;
    // Exposure compensation in EV (log2 stops): the tone mapped result is
    // scaled by 2^exposureBias. Negative values darken the image.
    float       exposureBias;
    // Blends the adaptive tone curve with Reinhard
    // (0 = adaptive curve only, 1 = Reinhard only).
    float       contrast;
} QrDrawFrameTonemappingParams;

typedef struct QrDrawFrameSkyParams
{
    QrSkyType   skyType;
    // The colour of the sky itself, sent by the host as rt_sky_color: for the
    // procedural sky (QR_SKY_TYPE_PROCEDURAL) it is exactly the colour the sky
    // is drawn with, and for QR_SKY_TYPE_COLOR it is the colour of the flat sky.
    // Sky brightness does not belong here; it is skyColorMultiplier below.
    QrFloat3D   skyColorDefault;
    // The colour of the sun's disc in the procedural sky: the sun itself, as
    // opposed to skyColorDefault above, which is the colour of the sky it hangs
    // in, so a dark sky can hold a bright sun. The host sends rt_sun_color here.
    // Default: (1, 1, 1)
    QrFloat3D   sunDiscColor;
    // The result sky color is multiplied by this value.
    float       skyColorMultiplier;
    // Opacity the procedural clouds are composited over the sky with
    // (rt_sky_cloud_alpha, 0 = no clouds, 1 = opaque). The name is kept for
    // compatibility with the tint strength the procedural sky used to have.
    float       skyColorSaturation;    float       skyAmbientLod;
    QrBool32    skyNee;
    // A point from which rays are traced while using QR_SKY_TYPE_RASTERIZED_GEOMETRY.
    QrFloat3D   skyViewerPosition;
    // If sky type is QR_SKY_TYPE_CUBEMAP, this cubemap is used.
    QrCubemap   skyCubemap;
    // Apply this transform to the direction when sampling a sky cubemap (QR_SKY_TYPE_CUBEMAP).
    // If equals to zero, then default value is used.
    // Default: identity matrix.
    QrMatrix3D  skyCubemapRotationTransform;
    // If false, the volumetric sun shafts (god rays) are disabled: the shadow map
    // is not rendered at all and the god rays buffers are cleared to zero.
    // Default: true
    QrBool32    godRaysEnabled;
    // Strength of the volumetric sun shafts: 1 is the look they were calibrated
    // with, 2 is twice as bright, 0 disables them (and then the shadow map that
    // feeds them is not rendered either). The host cvar is rt_godrays_intensity,
    // this engine's naming of Q2RTX's own gr_intensity knob, but the two values
    // cannot be compared: upstream multiplies the accumulated sun disc radiance
    // by it, this one multiplies the directional light colour.
    // Default: 1
    float       godRaysIntensity;
    // For QR_SKY_TYPE_RASTERIZED_GEOMETRY (classic Quake sky texture, i.e.
    // rt_physical_sky 0) there is no directional sun light to aim the god rays
    // at. When godRaysFromSkyTexture is non-zero the shafts are cast from the
    // brightest spot of the classic sky texture instead: godRaysSkyDirection is
    // the world-space direction TOWARD that spot and godRaysSkyColor is its
    // light colour (already light-fixup'd, matching how the sun colour is
    // prepared for the same params).
    QrBool32    godRaysFromSkyTexture;
    QrFloat3D   godRaysSkyDirection;
    QrFloat3D   godRaysSkyColor;
} QrDrawFrameSkyParams;

#define QR_LIGHT_STYLE_COUNT 64

typedef struct QrDrawFrameTexturesParams
{
    // What sampler filter to use for materials with QR_MATERIAL_CREATE_DYNAMIC_SAMPLER_FILTER_BIT.
    // Should be changed infrequently, as it reloads all texture descriptors.
    QrSamplerFilter dynamicSamplerFilter;
    float           normalMapStrength;
    // Multiplier for emission map values for indirect lighting.
    float           emissionMapBoost;
    // Upper bound for emissive materials in primary albedo channel (i.e. on screen).
    float           emissionMaxScreenColor;
    float           emissionSharpMask;
    float           talSelfLitOffset;
    // Set to true, if roughness should be more perceptually linear.
    // Default: true
    QrBool32        squareInputRoughness;
    // Default: 0.0
    float           minRoughness;
    // Default: 1
    uint32_t        emissionBlendMode;
    // Default: 1.0
    float           emissionBlendStrength;
    float           lightStyleScales[QR_LIGHT_STYLE_COUNT];
} QrDrawFrameTexturesParams;

typedef enum QrDebugDrawFlagBits
{
    QR_DEBUG_DRAW_ONLY_DIFFUSE_DIRECT_BIT = 1,
    QR_DEBUG_DRAW_ONLY_DIFFUSE_INDIRECT_BIT = 2,
    QR_DEBUG_DRAW_ONLY_SPECULAR_BIT = 4,
    QR_DEBUG_DRAW_UNFILTERED_DIFFUSE_DIRECT_BIT = 8,
    QR_DEBUG_DRAW_UNFILTERED_DIFFUSE_INDIRECT_BIT = 16,
    QR_DEBUG_DRAW_UNFILTERED_SPECULAR_BIT = 32,
    QR_DEBUG_DRAW_ALBEDO_WHITE_BIT = 64,
    QR_DEBUG_DRAW_MOTION_VECTORS_BIT = 128,
    QR_DEBUG_DRAW_GRADIENTS_BIT = 256,
    // Internal: enables the new Q2RTX-style core rendering path.
    // The host sets this bit when the "rt_core_q2rtx" cvar is enabled.
    QR_DEBUG_DRAW_Q2RTX_CORE_BIT = 1024,
    // Internal: shows the raw god rays buffer (volumetric sunlight).
    QR_DEBUG_DRAW_GOD_RAYS_BIT = 2048,
    QR_DEBUG_DRAW_STATS_BIT = 4096,
    QR_DEBUG_DRAW_LUMA_BIT = 8192,
    // Enables the GPU pass timings of the renderer's own frame (the gpuPassMs of
    // QrFrameStats). Honored by the legacy render path only: a frame recorded
    // through the RHI layer marks nothing, and qrGetFrameStatsEx reports
    // gpuTimingValid = 0 for it.
    QR_DEBUG_DRAW_PASS_STATS_BIT = 16384,
} QrDebugDrawFlagBits;
typedef QrFlags QrDebugDrawFlags;

typedef struct QrDrawFrameDebugParams
{
    QrDebugDrawFlags drawFlags;
} QrDrawFrameDebugParams;

typedef struct QrDrawFrameIlluminationParams
{
    // Shadow rays are cast, if illumination bounce index is in [0, maxBounceShadows).
    uint32_t    maxBounceShadows;
    // If false, only one bounce will be cast from a primary surface.
    // If true, a bounce of that bounce will be also cast.
    // If false, reflections and indirect diffuse might appear darker,
    // since inside of them, shadowed areas are just pitch black.
    // Default: true
    QrBool32    enableSecondBounceForIndirect;
    // Size of the side of a cell for the light grid. Kept for compatibility with
    // the upstream interface: this renderer places its lights through the cluster
    // light lists, so no pass reads the field and there is no light-grid debug view.
    // Default: 1.0
    float       cellWorldSize;
    // If 0.0, then the change of illumination won't be checked, i.e. if a light source suddenly disappeared,
    // its lighting still will be visible. But if it's 1.0, then lighting will be dropped at the given screen region
    // and the accumulation will start from scratch.
    // Default: 0.5
    float       directDiffuseSensitivityToChange;
    // Default: 0.2
    float       indirectDiffuseSensitivityToChange;
    // Default: 0.5
    float       specularSensitivityToChange;
    // The higher the value, the more polygonal lights act like a spotlight. 
    // Default: 2.0
    float       polygonalLightSpotlightFactor;
    // How the Q2 ASVGF depth gradient is computed.
    // 0: magnitude of the clip-space depth gradient (legacy)
    // 1: reciprocal of the per-pixel depth change, i.e. fwidth_depth as in Q2RTX
    // Default: 1
    uint32_t    q2DepthGradMode;
    // How the Q2RTX-style polygonal light statistics (shadowing ratios per light list
    // slot) are accumulated and used.
    // 0: disabled - no accumulation and no read of the statistics
    // 1: accumulate on every NEE light sample and apply the result
    // 2: accumulate only on the first NEE light sample of a pixel
    // 3: accumulate on every sample, but do not apply the result
    // 4: accumulate without atomics on every sample (diagnostic: increments are racy)
    // Default: 1
    uint32_t    q2LightStatsMode;
    // If 1, the Q2 reflection/refraction raygen returns before loading the rest of
    // the G-buffer when the primary surface neither reflects nor refracts. Purely
    // a bandwidth optimization: such pixels write nothing either way.
    // Default: 1
    uint32_t    reflRefrEarlyOut;
    // Number of NEE light samples per pixel in the direct pass. The estimator
    // divides by this count, so any value stays unbiased: lowering it only
    // trades noise for shadow rays. Clamped to 1..2 (see the RNG salt notes in
    // RtRaygenDirect.rgen). Q2RTX traces one sample, so 1 is the default.
    // Default: 1
    uint32_t    neeLightSamples;
    // Q2RTX pt_num_bounce_rays (host cvar rt_gi_level): 0 - no indirect lighting
    // at all, 0.5 - low, 1 - medium (one indirect bounce), 2 - high (two indirect
    // bounces). Values below 0.25 disable the indirect pass entirely, and the
    // level on its own selects the bounce count above the first one.
    // Default: 1
    float       giBounceRays;
    // Q2RTX flt_enable: 1 - run the ASVGF denoiser (default), 0 - composite the
    // raw ReSTIR signal without any filtering. The denoised image is smoother,
    // but the unfiltered one reacts to lighting changes instantly.
    // Default: 1
    QrBool32    denoiserEnabled;
    // Q2RTX flt_fixed_albedo: if nonzero, the diffuse albedo is replaced with
    // that value in the final composite, giving a "no textures" mode.
    // Default: 0
    float       fixedAlbedo;
    // Q2RTX pt_sun_bounce_range: how far the sun reaches into an indirect
    // bounce, in game units. A bounce ray that travelled farther than this
    // receives no sun light and does not trace its sun shadow ray either, which
    // is what keeps the indirect sun out of dark corners. 0 disables indirect
    // sunlight.
    // Default: 2000 (upstream's default; its reference-accumulation mode uses
    // 10000)
    float       sunBounceRange;
    // Q2RTX sun_bounce: multiplier on the sun's contribution to an indirect
    // bounce, applied on top of the distance falloff above. 1 is the physical
    // value.
    // Default: 1.0
    float       sunBounceScale;
    // For which light first-person viewer shadows should be ignored.
    // E.g. first-person flashlight.
    // Null, if none.
    uint64_t    *lightUniqueIdIgnoreFirstPersonViewerShadows;
} QrDrawFrameIlluminationParams;

typedef struct QrDrawFrameVolumetricParams
{
    QrBool32    enable;
    // If true, volumetric illumination is not calculated, just
    // using simple depth-based fog with ambient color.
    QrBool32    useSimpleDepthBased;
    // Farthest distance for volumetric illumination calculation.
    // Should be minimal to have better precision around camera.
    // Default: 100.0
    float       volumetricFar;
    QrFloat3D   ambientColor;
    // Default: 0.2
    float       scaterring;
    // Volumetric directional light source parameters.
    QrFloat3D   sourceColor;
    QrFloat3D   sourceDirection;
    // g parameter [-1..1] for the Henyey�Greenstein phase function.
    // Default: 0.0 (isotropic)
    float       sourceAssymetry;
} QrDrawFrameVolumetricParams;

typedef struct QrDrawFrameLevelFogParams
{
    // Color of the classic Quake level fog (the worldspawn "fog" key and the
    // `fog` console command), mixed into the tonemapped image. The value is
    // used as-is, without an sRGB decoding, the same way the classic renderer
    // blended it and the same way the host already hands it to the sky, so a
    // solid-colored sky stays identical to the fog.
    // Default: 0, 0, 0
    QrFloat3D   color;
    // Density of the level fog, already divided by the 64 the classic renderer
    // scaled it with: the fog amount is 1 - exp(-(density * distance)^2), and
    // the distance is the same world-space ray length the fog volumes use.
    // 0 disables the fog.
    // Default: 0.0
    float       density;
    // How much of the sky the fog replaces: the level's `skyfog`, which is 0.5
    // by default and may be overridden by a worldspawn "skyfog" key. Sky texels
    // carry no distance, so this is the blend weight itself, as in the classic
    // sky shader.
    // Default: 0.0
    float       skyBlend;
} QrDrawFrameLevelFogParams;

// Maximum number of Q2RTX-style fog volumes (matches MAX_FOG_VOLUMES in the renderer).
#define QR_MAX_FOG_VOLUMES 8

typedef struct QrFogVolume
{
    // Two points on any diagonal of the axis-aligned fog box.
    QrFloat3D   pointA;
    QrFloat3D   pointB;
    // Fog color.
    QrFloat3D   color;
    // Distance at which objects inside the fog are 50% visible.
    // Non-positive value disables the volume.
    float       halfExtinctionDistance;
    // Density gradient face: 0 = none (uniform fog),
    // 1..6 = xa, xb, ya, yb, za, zb (density is zero on that face).
    uint32_t    softface;
} QrFogVolume;

typedef struct QrDrawFrameBloomParams
{
    // Negative value disables bloom pass
    float       bloomIntensity;
    float       inputThreshold;
    float       bloomEmissionMultiplier;
} QrDrawFrameBloomParams;

typedef struct QrPostEffectWipe
{
    // [0..1] where 1 is whole screen width.
    float       stripWidth;
    QrBool32    beginNow;
    float       duration;
} QrPostEffectWipe;

typedef struct QrPostEffectRadialBlur
{
    QrBool32    isActive;
    float       transitionDurationIn;
    float       transitionDurationOut;
} QrPostEffectRadialBlur;

typedef struct QrPostEffectChromaticAberration
{
    QrBool32    isActive;
    float       transitionDurationIn;
    float       transitionDurationOut;
    float       intensity;
} QrPostEffectChromaticAberration;

typedef struct QrPostEffectInverseBlackAndWhite
{
    QrBool32    isActive;
    float       transitionDurationIn;
    float       transitionDurationOut;
} QrPostEffectInverseBlackAndWhite;

typedef struct QrPostEffectHueShift
{
    QrBool32    isActive;
    float       transitionDurationIn;
    float       transitionDurationOut;
} QrPostEffectHueShift;

typedef struct QrPostEffectDistortedSides
{
    QrBool32    isActive;
    float       transitionDurationIn;
    float       transitionDurationOut;
} QrPostEffectDistortedSides;

typedef struct QrPostEffectWaves
{
    QrBool32    isActive;
    float       transitionDurationIn;
    float       transitionDurationOut;
    float       amplitude;
    float       speed;
    float       xMultiplier;
} QrPostEffectWaves;

typedef struct QrPostEffectColorTint
{
    QrBool32    isActive;
    float       transitionDurationIn;
    float       transitionDurationOut;
    float       intensity;
    QrFloat3D   color;
} QrPostEffectColorTint;

typedef struct QrPostEffectCRT
{
    QrBool32    isActive;
} QrPostEffectCRT;

typedef struct QrDrawFramePostEffectsParams
{
    // Must be null, if effectWipeIsUsed was false.
    const QrPostEffectWipe                  *pWipe;
    const QrPostEffectRadialBlur            *pRadialBlur;
    const QrPostEffectChromaticAberration   *pChromaticAberration;
    const QrPostEffectInverseBlackAndWhite  *pInverseBlackAndWhite;
    const QrPostEffectHueShift              *pHueShift;
    const QrPostEffectDistortedSides        *pDistortedSides;
    const QrPostEffectWaves                 *pWaves;
    const QrPostEffectColorTint             *pColorTint;
    const QrPostEffectCRT                   *pCRT;
} QrDrawFramePostEffectsParams;

typedef enum QrMediaType
{
    QR_MEDIA_TYPE_VACUUM,
    QR_MEDIA_TYPE_WATER,
    QR_MEDIA_TYPE_GLASS,
    QR_MEDIA_TYPE_ACID,
} QrMediaType;

typedef struct QrDrawFrameReflectRefractParams
{
    uint32_t    maxReflectRefractDepth;
    // Media type, in which camera currently is.
    QrMediaType typeOfMediaAroundCamera;
    // Default: 1.52
    float       indexOfRefractionGlass;
    // Default: 1.33
    float       indexOfRefractionWater;
    QrBool32    forceNoWaterRefraction;
    float       waterWaveSpeed;
    float       waterWaveNormalStrength;
    // Strength of the classic turbulent surface warp (lava, teleport).
    // Default: 1.0, which is the amplitude of the classic warp (8 quake units).
    float       turbWarpStrength;
    // Color at 1 meter depth.
    QrFloat3D   waterColor;
    // Color at 1 meter depth.
    QrFloat3D   acidColor;
    float       acidDensity;
    // The lower this value, the sharper water normal textures.
    // Default: 1.0
    float       waterWaveTextureDerivativesMultiplier;
    // The larger this value, the larger the area one water texture covers.
    // If equals to 0.0, then default value is used.
    // Default: 1.0
    float       waterTextureAreaScale;
    // If true, reflections are disabled for backface triangles
    // of geometry that is marked QR_GEOMETRY_UPLOAD_NO_MEDIA_CHANGE_ON_REFRACT_BIT
    QrBool32    disableBackfaceReflectionsForNoMediaChange;
    // If true, portal normal will be twirled around its 'inPosition'.
    QrBool32    portalNormalTwirl;
} QrDrawFrameReflectRefractParams;

typedef enum QrRenderUpscaleTechnique
{
    QR_RENDER_UPSCALE_TECHNIQUE_LINEAR,
    QR_RENDER_UPSCALE_TECHNIQUE_NEAREST,
    QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2,
    QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3,
    QR_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS,
} QrRenderUpscaleTechnique;

typedef enum QrRenderSharpenTechnique
{
    QR_RENDER_SHARPEN_TECHNIQUE_NONE,
    QR_RENDER_SHARPEN_TECHNIQUE_NAIVE,
    QR_RENDER_SHARPEN_TECHNIQUE_AMD_CAS,
} QrRenderSharpenTechnique;

typedef enum QrRenderResolutionMode
{
    QR_RENDER_RESOLUTION_MODE_CUSTOM,
    QR_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE,
    QR_RENDER_RESOLUTION_MODE_PERFORMANCE,
    QR_RENDER_RESOLUTION_MODE_BALANCED,
    QR_RENDER_RESOLUTION_MODE_QUALITY,
    QR_RENDER_RESOLUTION_MODE_ULTRA_QUALITY,    // with AMD_FSR, same as QUALITY
    QR_RENDER_RESOLUTION_MODE_NATIVE_AA,        // FSR3.1 Native AA (1.0x ratio, anti-aliasing only)
} QrRenderResolutionMode;

typedef struct QrDrawFrameRenderResolutionParams
{
    QrRenderUpscaleTechnique    upscaleTechnique;
    QrRenderSharpenTechnique    sharpenTechnique; 
    QrRenderResolutionMode      resolutionMode;
    // Used, if resolutionMode is QR_RENDER_RESOLUTION_MODE_CUSTOM
    QrExtent2D                  customRenderSize;
    // If not null, final image will be downscaled to this size at the very end.
    // Needed, if pixelized look is needed, but the actual rendering should
    // be done in higher resolution.
    const QrExtent2D            *pPixelizedRenderSize;
} QrDrawFrameRenderResolutionParams;

typedef struct QrDrawFrameLensFlareParams
{
    QrBlendFactor               lensFlareBlendFuncSrc;
    QrBlendFactor               lensFlareBlendFuncDst;
} QrDrawFrameLensFlareParams;

typedef enum QrDrawFrameRayCullFlagBits
{
    QR_DRAW_FRAME_RAY_CULL_WORLD_0_BIT  = 1,    // QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0
    QR_DRAW_FRAME_RAY_CULL_WORLD_1_BIT  = 2,    // QR_GEOMETRY_VISIBILITY_TYPE_WORLD_1
    QR_DRAW_FRAME_RAY_CULL_WORLD_2_BIT  = 4,    // QR_GEOMETRY_VISIBILITY_TYPE_WORLD_2
    QR_DRAW_FRAME_RAY_CULL_SKY_BIT      = 8,    // QR_GEOMETRY_VISIBILITY_TYPE_SKY
} QrDrawFrameRayCullFlagBits;
typedef QrFlags QrDrawFrameRayCullFlags;

typedef struct QrDrawFrameInfo
{
    // View matrix is column major.
    float                   view[16];
    // For additional water calculations (is the flow vertical, make extinction stronger closer to horizon).
    // If the length is close to 0.0, then (0, 1, 0) is used.
    QrFloat3D               worldUpVector;

    // Additional info for ray cones, it's used to calculate differentials for texture sampling. Also, for FSR2.
    float                   fovYRadians;
    // Near and far planes for a projection matrix.
    float                   cameraNear;
    float                   cameraFar;
    // Max value: 10000.0
    float                   rayLength;
    // What world parts to render. First-person related geometry is always enabled.
    QrDrawFrameRayCullFlags rayCullMaskWorld;

    QrBool32                disableRayTracedGeometry;
    QrBool32                disableRasterization;

    double                  currentTime;
    QrBool32                disableEyeAdaptation;
    QrBool32                forceAntiFirefly;

    // Set to null, to use default values.
    const QrDrawFrameRenderResolutionParams     *pRenderResolutionParams;
    const QrDrawFrameIlluminationParams         *pIlluminationParams;
    const QrDrawFrameVolumetricParams           *pVolumetricParams;
    const QrDrawFrameTonemappingParams          *pTonemappingParams;
    const QrDrawFrameBloomParams                *pBloomParams;
    const QrDrawFrameReflectRefractParams       *pReflectRefractParams;
    const QrDrawFrameSkyParams                  *pSkyParams;
    const QrDrawFrameTexturesParams             *pTexturesParams;
    const QrDrawFrameLensFlareParams            *pLensFlareParams;
    const QrDrawFrameLevelFogParams             *pLevelFogParams;
    const QrDrawFrameDebugParams                *pDebugParams;
    QrDrawFramePostEffectsParams                postEffectParams;

} QrDrawFrameInfo;

QRAPI QrResult QRCONV qrDrawFrame(
    QrInstance                          qrInstance,
    const QrDrawFrameInfo               *pDrawInfo);

// Set Q2RTX-style fog volumes (up to QR_MAX_FOG_VOLUMES). Used by the new
// Q2RTX core path. A zero count disables all fog volumes.
QRAPI QrResult QRCONV qrSetFogVolumes(
    QrInstance                          qrInstance,
    uint32_t                            count,
    const QrFogVolume                   *pVolumes);



QRAPI QrBool32 QRCONV qrIsRenderUpscaleTechniqueAvailable(
    QrInstance                          qrInstance,
    QrRenderUpscaleTechnique            technique);

#define QR_GPU_PASS_COUNT 18

#define QR_RAY_STATS_CATEGORY_COUNT 5

typedef struct QrFrameStats
{
    uint32_t    raysTotal;
    uint32_t    raysPerCategory[QR_RAY_STATS_CATEGORY_COUNT];
    uint32_t    fpsX10;
    QrBool32    gpuTimingValid;
    float       gpuFrameMs;
    float       gpuPassMs[QR_GPU_PASS_COUNT];
    // Number of rg* entry points the host called since qrStartFrame: the per-frame work the
    // backend pays outside of the passes, whatever the resolution or the ray budget is.
    uint32_t    apiCalls;
} QrFrameStats;

QRAPI QrResult QRCONV qrGetFrameStatsEx(
    QrInstance                          qrInstance,
    QrFrameStats                       *pStats);

QRAPI QrResult QRCONV qrGetFrameStats(
    QrInstance                          qrInstance,
    uint32_t                           *pRays,
    uint32_t                           *pFpsX10);

QRAPI const char* QRCONV qrGetGpuPassName(
    uint32_t                            passIndex);

QRAPI const char* QRCONV qrGetResultDescription(QrResult result);

#ifdef __cplusplus
}
#endif

#endif
