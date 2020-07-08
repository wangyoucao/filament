/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "PostProcessManager.h"

#include "details/Engine.h"

#include "fg/FrameGraph.h"
#include "fg/FrameGraphPassResources.h"

#include "RenderPass.h"

#include "details/Camera.h"
#include "details/ColorGrading.h"
#include "details/Material.h"
#include "details/MaterialInstance.h"
#include "details/Texture.h"

#include "generated/resources/materials.h"

#include <private/filament/SibGenerator.h>

#include <filament/MaterialEnums.h>

#include <math/half.h>
#include <math/mat2.h>

#include <utils/Log.h>

#include <limits>

namespace filament {

using namespace utils;
using namespace math;
using namespace backend;

static constexpr uint8_t kMaxBloomLevels = 12u;
static_assert(kMaxBloomLevels >= 3, "We require at least 3 bloom levels");

// ------------------------------------------------------------------------------------------------

PostProcessManager::PostProcessMaterial::PostProcessMaterial() noexcept {
    mEngine = nullptr;
    mData = nullptr;
    mMaterial = nullptr;            // aliased to mEngine
    mMaterialInstance = nullptr;    // aliased to mData
}

PostProcessManager::PostProcessMaterial::PostProcessMaterial(FEngine& engine,
        uint8_t const* data, int size) noexcept
        : PostProcessMaterial() {
    mEngine = &engine;
    mData = data;
    mSize = size;
}

PostProcessManager::PostProcessMaterial::PostProcessMaterial(
        PostProcessManager::PostProcessMaterial&& rhs) noexcept
        : PostProcessMaterial() {
    using namespace std;
    swap(mEngine, rhs.mEngine);
    swap(mData, rhs.mData);
    swap(mSize, rhs.mSize);
    swap(mHasMaterial, rhs.mHasMaterial);
}

PostProcessManager::PostProcessMaterial& PostProcessManager::PostProcessMaterial::operator=(
        PostProcessManager::PostProcessMaterial&& rhs) noexcept {
    using namespace std;
    swap(mEngine, rhs.mEngine);
    swap(mData, rhs.mData);
    swap(mSize, rhs.mSize);
    return *this;
}

PostProcessManager::PostProcessMaterial::~PostProcessMaterial() {
    assert(!mHasMaterial || mMaterial == nullptr);
}

void PostProcessManager::PostProcessMaterial::terminate(FEngine& engine) noexcept {
    if (mHasMaterial) {
        engine.destroy(mMaterial);
        mMaterial = nullptr;
        mMaterialInstance = nullptr;
        mHasMaterial = false;
    } else {
        mEngine = nullptr;
        mData = nullptr;
    }
}

void PostProcessManager::PostProcessMaterial::assertMaterial() const noexcept {
    if (UTILS_UNLIKELY(!mHasMaterial)) {
        // TODO: After all materials using this class have been converted to the post-process material
        //       domain, load both OPAQUE and TRANSPARENT variants here.
        mMaterial = upcast(Material::Builder().package(mData, mSize).build(*mEngine));
        mMaterialInstance = mMaterial->getDefaultInstance();
        mHasMaterial = true;
    }
}

PipelineState PostProcessManager::PostProcessMaterial::getPipelineState(uint8_t variant) const noexcept {
    assertMaterial();
    return {
            .program = mMaterial->getProgram(variant),
            .rasterState = mMaterial->getRasterState(),
            .scissor = mMaterialInstance->getScissor()
    };
}

PipelineState PostProcessManager::PostProcessMaterial::getPipelineState() const noexcept {
    return getPipelineState(0);
}

FMaterial* PostProcessManager::PostProcessMaterial::getMaterial() const {
    assertMaterial();
    return mMaterial;
}

FMaterialInstance* PostProcessManager::PostProcessMaterial::getMaterialInstance() const {
    assertMaterial();
    return mMaterialInstance;
}

// ------------------------------------------------------------------------------------------------

PostProcessManager::PostProcessManager(FEngine& engine) noexcept : mEngine(engine) {
}

#define MATERIAL(n) MATERIALS_ ## n ## _DATA, MATERIALS_ ## n ## _SIZE

void PostProcessManager::init() noexcept {
    auto& engine = mEngine;
    DriverApi& driver = engine.getDriverApi();

    mSSAO                   = { engine, MATERIAL(SAO) };
    mMipmapDepth            = { engine, MATERIAL(MIPMAPDEPTH) };
    mBilateralBlur          = { engine, MATERIAL(BILATERALBLUR) };
    mSeparableGaussianBlur  = { engine, MATERIAL(SEPARABLEGAUSSIANBLUR) };
    mBloomDownsample        = { engine, MATERIAL(BLOOMDOWNSAMPLE) };
    mBloomUpsample          = { engine, MATERIAL(BLOOMUPSAMPLE) };
    mBlit[0]                = { engine, MATERIAL(BLITLOW) };
    mBlit[1]                = { engine, MATERIAL(BLITMEDIUM) };
    mBlit[2]                = { engine, MATERIAL(BLITHIGH) };
    mColorGrading           = { engine, MATERIAL(COLORGRADING) };
    mFxaa                   = { engine, MATERIAL(FXAA) };
    mDoFDownsample          = { engine, MATERIAL(DOFDOWNSAMPLE) };
    mDoFMipmap              = { engine, MATERIAL(DOFMIPMAP) };
    mDoFTiles               = { engine, MATERIAL(DOFTILES) };
    mDoFDilate              = { engine, MATERIAL(DOFDILATE) };
    mDoF                    = { engine, MATERIAL(DOF) };
    mDoFMedian              = { engine, MATERIAL(DOFMEDIAN) };
    mDoFCombine             = { engine, MATERIAL(DOFCOMBINE) };
    if (driver.isFrameBufferFetchSupported()) {
        mColorGradingAsSubpass = { engine, MATERIAL(COLORGRADINGASSUBPASS) };
    }

    // UBO storage size.
    // The effective kernel size is (kMaxPositiveKernelSize - 1) * 4 + 1.
    // e.g.: 5 positive-side samples, give 4+1+4=9 samples both sides
    // taking advantage of linear filtering produces an effective kernel of 8+1+8=17 samples
    // and because it's a separable filter, the effective 2D filter kernel size is 17*17
    // The total number of samples needed over the two passes is 18.
    mSeparableGaussianBlurKernelStorageSize = mSeparableGaussianBlur.getMaterial()->reflect("kernel")->size;

    mDummyOneTexture = driver.createTexture(SamplerType::SAMPLER_2D, 1,
            TextureFormat::RGBA8, 0, 1, 1, 1, TextureUsage::DEFAULT);

    mDummyZeroTexture = driver.createTexture(SamplerType::SAMPLER_2D, 1,
            TextureFormat::RGBA8, 0, 1, 1, 1, TextureUsage::DEFAULT);

    PixelBufferDescriptor dataOne(driver.allocate(4), 4, PixelDataFormat::RGBA, PixelDataType::UBYTE);
    PixelBufferDescriptor dataZero(driver.allocate(4), 4, PixelDataFormat::RGBA, PixelDataType::UBYTE);
    *static_cast<uint32_t *>(dataOne.buffer) = 0xFFFFFFFF;
    *static_cast<uint32_t *>(dataZero.buffer) = 0;
    driver.update2DImage(mDummyOneTexture, 0, 0, 0, 1, 1, std::move(dataOne));
    driver.update2DImage(mDummyZeroTexture, 0, 0, 0, 1, 1, std::move(dataZero));
}

void PostProcessManager::terminate(DriverApi& driver) noexcept {
    FEngine& engine = mEngine;
    driver.destroyTexture(mDummyOneTexture);
    driver.destroyTexture(mDummyZeroTexture);
    mSSAO.terminate(engine);
    mMipmapDepth.terminate(engine);
    mBilateralBlur.terminate(engine);
    mSeparableGaussianBlur.terminate(engine);
    mBloomDownsample.terminate(engine);
    mBloomUpsample.terminate(engine);
    mBlit[0].terminate(engine);
    mBlit[1].terminate(engine);
    mBlit[2].terminate(engine);
    mColorGrading.terminate(engine);
    mColorGradingAsSubpass.terminate(engine);
    mFxaa.terminate(engine);
    mDoFDownsample.terminate(engine);
    mDoFMipmap.terminate(engine);
    mDoFTiles.terminate(engine);
    mDoFDilate.terminate(engine);
    mDoF.terminate(engine);
    mDoFMedian.terminate(engine);
    mDoFCombine.terminate(engine);
}

// ------------------------------------------------------------------------------------------------

FrameGraphId<FrameGraphTexture> PostProcessManager::structure(FrameGraph& fg,
        const RenderPass& pass, uint32_t width, uint32_t height, float scale) noexcept {

    // structure pass -- automatically culled if not used, currently used by:
    //    - ssao
    //    - contact shadows
    //    - depth-of-field
    // It consists of a mipmapped depth pass, tuned for SSAO
    struct StructurePassData {
        FrameGraphId<FrameGraphTexture> depth;
        FrameGraphRenderTargetHandle rt;
    };

    // sanitize a bit the user provided scaling factor
    width  = std::max(32u, (uint32_t)std::ceil(width * scale));
    height = std::max(32u, (uint32_t)std::ceil(height * scale));

    // We limit the lowest lod size to 32 pixels (which is where the -5 comes from)
    const size_t levelCount = FTexture::maxLevelCount(width, height) - 5;
    assert(levelCount >= 1);

    // generate depth pass at the requested resolution
    auto& structurePass = fg.addPass<StructurePassData>("Structure Pass",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.depth = builder.createTexture("Depth Buffer", {
                        .width = width, .height = height,
                        .levels = uint8_t(levelCount),
                        .format = TextureFormat::DEPTH24 });

                data.depth = builder.write(builder.read(data.depth));

                data.rt = builder.createRenderTarget("Structure Target", {
                        .attachments = {{}, data.depth },
                        .clearFlags = TargetBufferFlags::DEPTH
                });
            },
            [=](FrameGraphPassResources const& resources, auto const& data, DriverApi& driver) {
                auto out = resources.get(data.rt);
                pass.execute(resources.getPassName(), out.target, out.params);
            });

    auto depth = structurePass.getData().depth;

    /*
     * create depth mipmap chain
    */

    // The first mip already exists, so we process n-1 lods
    for (size_t level = 0; level < levelCount - 1; level++) {
        depth = mipmapPass(fg, depth, level);
    }

    fg.getBlackboard().put("structure", depth);
    return depth;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::mipmapPass(FrameGraph& fg,
        FrameGraphId<FrameGraphTexture> input, size_t level) noexcept {

    Handle<HwRenderPrimitive> fullScreenRenderPrimitive = mEngine.getFullScreenRenderPrimitive();

    struct DepthMipData {
        FrameGraphId<FrameGraphTexture> in;
        FrameGraphId<FrameGraphTexture> out;
        FrameGraphRenderTargetHandle rt;

    };

    auto& depthMipmapPass = fg.addPass<DepthMipData>("Depth Mipmap Pass",
            [&](FrameGraph::Builder& builder, auto& data) {
                const char* name = builder.getName(input);
                data.in = builder.sample(input);
                data.out = builder.write(data.in);
                data.rt = builder.createRenderTarget(name, {
                        .attachments = {{}, { data.out, uint8_t(level + 1) }}});
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {

                auto in = resources.getTexture(data.in);
                auto out = resources.get(data.rt);

                FMaterialInstance* const mi = mMipmapDepth.getMaterialInstance();
                mi->setParameter("depth", in, { .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST });
                mi->setParameter("level", uint32_t(level));
                mi->commit(driver);
                mi->use(driver);

                driver.beginRenderPass(out.target, out.params);
                driver.draw(mMipmapDepth.getPipelineState(), fullScreenRenderPrimitive);
                driver.endRenderPass();
            });

    return depthMipmapPass.getData().out;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::screenSpaceAmbientOclusion(
        FrameGraph& fg, RenderPass& pass,
        filament::Viewport const& svp, const CameraInfo& cameraInfo,
        View::AmbientOcclusionOptions const& options) noexcept {

    FEngine& engine = mEngine;
    Handle<HwRenderPrimitive> fullScreenRenderPrimitive = engine.getFullScreenRenderPrimitive();

    FrameGraphId<FrameGraphTexture> depth = fg.getBlackboard().get<FrameGraphTexture>("structure");
    assert(depth.isValid());

    const size_t levelCount = fg.getDescriptor(depth).levels;

    /*
     * Our main SSAO pass
     */

    struct SSAOPassData {
        FrameGraphId<FrameGraphTexture> depth;
        FrameGraphId<FrameGraphTexture> ssao;
        View::AmbientOcclusionOptions options;
        FrameGraphRenderTargetHandle rt;
    };

    auto& SSAOPass = fg.addPass<SSAOPassData>("SSAO Pass",
            [&](FrameGraph::Builder& builder, auto& data) {
                auto const& desc = builder.getDescriptor(depth);

                data.options = options;
                data.depth = builder.sample(depth);
                data.ssao = builder.createTexture("SSAO Buffer", {
                        .width = desc.width,
                        .height = desc.height,
                        .format = TextureFormat::RGB8
                });

                // Here we use the depth test to skip pixels at infinity (i.e. the skybox)
                // Note that we have to clear the SAO buffer because blended objects will end-up
                // reading into it even though they were not written in the depth buffer.
                // The bilateral filter in the blur pass will ignore pixels at infinity.

                data.ssao = builder.write(data.ssao);
                data.rt = builder.createRenderTarget("SSAO Target", {
                        .attachments = { data.ssao, data.depth },
                        .clearColor = { 1.0f },
                        .clearFlags = TargetBufferFlags::COLOR
                });
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                auto depth = resources.getTexture(data.depth);
                auto ssao = resources.get(data.rt);
                auto const& desc = resources.getDescriptor(data.ssao);

                // estimate of the size in pixel of a 1m tall/wide object viewed from 1m away (i.e. at z=-1)
                const float projectionScale = std::min(
                        0.5f * cameraInfo.projection[0].x * desc.width,
                        0.5f * cameraInfo.projection[1].y * desc.height);


                // Where the falloff function peaks
                const float peak = 0.1f * options.radius;
                // We further scale the user intensity by 3, for a better default at intensity=1
                const float intensity = (2.0f * F_PI * peak) * data.options.intensity * 3.0f;
                // always square AO result, as it looks much better
                const float power = data.options.power * 2.0f;

                float sampleCount = 7.0f;
                float spiralTurns = 5.0f;
                switch (data.options.quality) {
                    case View::QualityLevel::LOW:
                        sampleCount = 7.0f;
                        spiralTurns = 5.0f;
                        break;
                    case View::QualityLevel::MEDIUM:
                        sampleCount = 11.0f;
                        spiralTurns = 9.0f;
                        break;
                    case View::QualityLevel::HIGH:
                        sampleCount = 16.0f;
                        spiralTurns = 10.0f;
                        break;
                    case View::QualityLevel::ULTRA:
                        sampleCount = 32.0f;
                        spiralTurns = 14.0f;
                        break;
                }

                const auto invProjection = inverse(cameraInfo.projection);
                const float inc = (1.0f / (sampleCount - 0.5f)) * spiralTurns * 2.0f * float(math::F_PI);

                FMaterialInstance* const mi = mSSAO.getMaterialInstance();
                mi->setParameter("depth", depth, {
                        .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST
                });
                mi->setParameter("resolution",
                        float4{ desc.width, desc.height, 1.0f / desc.width, 1.0f / desc.height });
                mi->setParameter("invRadiusSquared", 1.0f / (data.options.radius * data.options.radius));
                mi->setParameter("projectionScaleRadius", projectionScale * data.options.radius);
                mi->setParameter("depthParams", float2{
                        -cameraInfo.projection[3].z, cameraInfo.projection[2].z - 1.0 } * 0.5f);
                mi->setParameter("positionParams", float2{
                        invProjection[0][0], invProjection[1][1] } * 2.0f);
                mi->setParameter("peak2", peak * peak);
                mi->setParameter("bias", data.options.bias);
                mi->setParameter("power", power);
                mi->setParameter("intensity", intensity);
                mi->setParameter("maxLevel", uint32_t(levelCount - 1));
                mi->setParameter("sampleCount", float2{ sampleCount, 1.0f / (sampleCount - 0.5f) });
                mi->setParameter("spiralTurns", spiralTurns);
                mi->setParameter("angleIncCosSin", float2{ std::cos(inc), std::sin(inc) });
                mi->setParameter("invFarPlane", 1.0f / -cameraInfo.zf);
                mi->commit(driver);
                mi->use(driver);

                PipelineState pipeline(mSSAO.getPipelineState());
                pipeline.rasterState.depthFunc = RasterState::DepthFunc::G;

                driver.beginRenderPass(ssao.target, ssao.params);
                driver.draw(pipeline, fullScreenRenderPrimitive);
                driver.endRenderPass();
            });

    FrameGraphId<FrameGraphTexture> ssao = SSAOPass.getData().ssao;

    /*
     * Final separable bilateral blur pass
     */

    const bool highQualitySampling =
            options.upsampling >= View::QualityLevel::HIGH && options.resolution < 1.0f;

    ssao = bilateralBlurPass(fg, ssao, { 1, 0 }, cameraInfo.zf,
            TextureFormat::RGB8);

    ssao = bilateralBlurPass(fg, ssao, { 0, 1 }, cameraInfo.zf,
            highQualitySampling ? TextureFormat::RGB8 : TextureFormat::R8);

    fg.getBlackboard().put("ssao", ssao);
    return ssao;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::bilateralBlurPass(
        FrameGraph& fg, FrameGraphId<FrameGraphTexture> input, math::int2 axis, float zf,
        TextureFormat format) noexcept {

    Handle<HwRenderPrimitive> fullScreenRenderPrimitive = mEngine.getFullScreenRenderPrimitive();

    struct BlurPassData {
        FrameGraphId<FrameGraphTexture> input;
        FrameGraphId<FrameGraphTexture> blurred;
        FrameGraphRenderTargetHandle rt;
    };

    auto& blurPass = fg.addPass<BlurPassData>("Separable Blur Pass",
            [&](FrameGraph::Builder& builder, auto& data) {

                auto const& desc = builder.getDescriptor(input);

                data.input = builder.sample(input);

                data.blurred = builder.createTexture("Blurred output", {
                        .width = desc.width, .height = desc.height, .format = format });

                auto depth = fg.getBlackboard().get<FrameGraphTexture>("structure");
                assert(depth.isValid());
                builder.read(depth);

                // Here we use the depth test to skip pixels at infinity (i.e. the skybox)
                // We need to clear the buffers because we are skipping pixels at infinity (skybox)
                data.blurred = builder.write(data.blurred);
                data.rt = builder.createRenderTarget("Blurred target", {
                        .attachments = { data.blurred, depth },
                        .clearColor = { 1.0f },
                        .clearFlags = TargetBufferFlags::COLOR
                });
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                auto ssao = resources.getTexture(data.input);
                auto blurred = resources.get(data.rt);
                auto const& desc = resources.getDescriptor(data.blurred);

                // TODO: "oneOverEdgeDistance" should be a user-settable parameter
                //       z-distance that constitute an edge for bilateral filtering
                FMaterialInstance* const mi = mBilateralBlur.getMaterialInstance();
                mi->setParameter("ssao", ssao, { /* only reads level 0 */ });
                mi->setParameter("axis", axis / float2{desc.width, desc.height});
                mi->setParameter("farPlaneOverEdgeDistance", -zf / 0.0625f);
                mi->commit(driver);
                mi->use(driver);

                PipelineState pipeline(mBilateralBlur.getPipelineState());
                pipeline.rasterState.depthFunc = RasterState::DepthFunc::G;

                driver.beginRenderPass(blurred.target, blurred.params);
                driver.draw(pipeline, fullScreenRenderPrimitive);
                driver.endRenderPass();
            });

    return blurPass.getData().blurred;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::generateGaussianMipmap(FrameGraph& fg,
        FrameGraphId<FrameGraphTexture> input, size_t roughnessLodCount,
        bool reinhard, size_t kernelWidth, float sigmaRatio) noexcept {
    for (size_t i = 1; i < roughnessLodCount; i++) {
        input = gaussianBlurPass(fg, input, i - 1, input, i, reinhard, kernelWidth, sigmaRatio);
        reinhard = false; // only do the reinhard filtering on the first level
    }
    return input;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::gaussianBlurPass(FrameGraph& fg,
        FrameGraphId<FrameGraphTexture> input, uint8_t srcLevel,
        FrameGraphId<FrameGraphTexture> output, uint8_t dstLevel,
        bool reinhard, size_t kernelWidth, float sigmaRatio) noexcept {

    const float sigma = (kernelWidth + 1.0f) / sigmaRatio;

    Handle<HwRenderPrimitive> fullScreenRenderPrimitive = mEngine.getFullScreenRenderPrimitive();

    auto computeGaussianCoefficients = [kernelWidth, sigma](float2* kernel, size_t size) -> size_t {
        const float alpha = 1.0f / (2.0f * sigma * sigma);

        // number of positive-side samples needed, using linear sampling
        size_t m = (kernelWidth - 1) / 4 + 1;
        // clamp to what we have
        m = std::min(size, m);

        // How the kernel samples are stored:
        //  *===*---+---+---+---+---+---+
        //  | 0 | 1 | 2 | 3 | 4 | 5 | 6 |       Gaussian coefficients (right size)
        //  *===*-------+-------+-------+
        //  | 0 |   1   |   2   |   3   |       stored coefficients (right side)

        kernel[0].x = 1.0;
        kernel[0].y = 0.0;
        float totalWeight = kernel[0].x;

        for (size_t i = 1; i < m; i++) {
            float x0 = i * 2 - 1;
            float x1 = i * 2;
            float k0 = std::exp(-alpha * x0 * x0);
            float k1 = std::exp(-alpha * x1 * x1);
            float k = k0 + k1;
            float o = k0 / k;
            kernel[i].x = k;
            kernel[i].y = o;
            totalWeight += (k0 + k1) * 2.0f;
        }
        for (size_t i = 0; i < m; i++) {
            kernel[i].x *= 1.0f / totalWeight;
        }
        return m;
    };

    struct BlurPassData {
        FrameGraphId<FrameGraphTexture> in;
        FrameGraphId<FrameGraphTexture> out;
        FrameGraphId<FrameGraphTexture> temp;
        FrameGraphRenderTargetHandle outRT;
        FrameGraphRenderTargetHandle tempRT;
    };

    const size_t kernelStorageSize = mSeparableGaussianBlurKernelStorageSize;
    auto& gaussianBlurPasses = fg.addPass<BlurPassData>("Gaussian Blur Passes",
            [&](FrameGraph::Builder& builder, auto& data) {
                auto desc = builder.getDescriptor(input);

                if (!output.isValid()) {
                    output = builder.createTexture("Blurred texture", desc);
                }

                data.in = builder.sample(input);

                data.out = builder.write(output);

                // width of the destination level (b/c we're blurring horizontally)
                desc.width = FTexture::valueForLevel(dstLevel, desc.width);
                // height of the source level (b/c it's not blurred in this pass)
                desc.height = FTexture::valueForLevel(srcLevel, desc.height);
                // only one level
                desc.levels = 1;

                data.temp = builder.createTexture("Horizontal temporary buffer", desc);
                data.temp = builder.write(builder.sample(data.temp));

                data.tempRT = builder.createRenderTarget("Horizontal temporary target", {
                        .attachments = { data.temp } });

                data.outRT = builder.createRenderTarget("Blurred target", {
                        .attachments = {{ data.out, dstLevel }} });
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {

                PostProcessMaterial const& separableGaussianBlur = mSeparableGaussianBlur;
                FMaterialInstance* const mi = separableGaussianBlur.getMaterialInstance();

                float2 kernel[64];
                size_t m = computeGaussianCoefficients(kernel,
                        std::min(sizeof(kernel) / sizeof(*kernel), kernelStorageSize));

                // horizontal pass
                auto hwTempRT = resources.get(data.tempRT);
                auto hwOutRT = resources.get(data.outRT);
                auto hwTemp = resources.getTexture(data.temp);
                auto hwIn = resources.getTexture(data.in);
                auto const& inDesc = resources.getDescriptor(data.in);
                auto const& outDesc = resources.getDescriptor(data.out);
                auto const& tempDesc = resources.getDescriptor(data.temp);

                mi->setParameter("source", hwIn, {
                        .filterMag = SamplerMagFilter::LINEAR,
                        .filterMin = SamplerMinFilter::LINEAR_MIPMAP_NEAREST
                });
                mi->setParameter("level", (float)srcLevel);
                mi->setParameter("reinhard", reinhard ? uint32_t(1) : uint32_t(0));
                mi->setParameter("resolution", float4{
                        tempDesc.width, tempDesc.height,
                        1.0f / tempDesc.width, 1.0f / tempDesc.height });
                mi->setParameter("axis",
                        float2{ 1.0f / FTexture::valueForLevel(srcLevel, inDesc.width), 0 });
                mi->setParameter("count", (int32_t)m);
                mi->setParameter("kernel", kernel, m);
                mi->commit(driver);
                mi->use(driver);

                // The framegraph only computes discard flags at FrameGraphPass boundaries
                hwTempRT.params.flags.discardEnd = TargetBufferFlags::NONE;

                driver.beginRenderPass(hwTempRT.target, hwTempRT.params);
                driver.draw(separableGaussianBlur.getPipelineState(), fullScreenRenderPrimitive);
                driver.endRenderPass();

                // vertical pass
                auto width = FTexture::valueForLevel(dstLevel, outDesc.width);
                auto height = FTexture::valueForLevel(dstLevel, outDesc.height);
                assert(width == hwOutRT.params.viewport.width);
                assert(height == hwOutRT.params.viewport.height);

                mi->setParameter("source", hwTemp, {
                        .filterMag = SamplerMagFilter::LINEAR,
                        .filterMin = SamplerMinFilter::LINEAR /* level is always 0 */
                });
                mi->setParameter("level", 0.0f);
                mi->setParameter("resolution",
                        float4{ width, height, 1.0f / width, 1.0f / height });
                mi->setParameter("axis", float2{ 0, 1.0f / tempDesc.height });
                mi->commit(driver);

                driver.beginRenderPass(hwOutRT.target, hwOutRT.params);
                driver.draw(separableGaussianBlur.getPipelineState(), fullScreenRenderPrimitive);
                driver.endRenderPass();
            });

    return gaussianBlurPasses.getData().out;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::dof(FrameGraph& fg,
        FrameGraphId<FrameGraphTexture> input,
        const View::DepthOfFieldOptions& dofOptions,
        bool translucent,
        const CameraInfo& cameraInfo) noexcept {

    FEngine& engine = mEngine;
    Handle<HwRenderPrimitive> const& fullScreenRenderPrimitive = engine.getFullScreenRenderPrimitive();

    const uint8_t variant = uint8_t(
            translucent ? PostProcessVariant::TRANSLUCENT : PostProcessVariant::OPAQUE);

    const TextureFormat format = translucent ? TextureFormat::RGBA16F
                                             : TextureFormat::R11F_G11F_B10F;

    // rotate the bokeh based on the aperture diameter (i.e. angle of the blades)
    float bokehAngle = float(F_PI) / 6.0f;
    if (dofOptions.maxApertureDiameter > 0.0f) {
        bokehAngle += float(F_PI_2) * saturate(cameraInfo.A / dofOptions.maxApertureDiameter);
    }

    const float focusDistance = std::max(cameraInfo.zn, dofOptions.focusDistance);
    auto const& desc = fg.getDescriptor<FrameGraphTexture>(input);
    const float Kc = (cameraInfo.A * cameraInfo.f) / (focusDistance - cameraInfo.f);
    const float Ks = ((float)desc.height) / FCamera::SENSOR_SIZE;
    const float2 cocParams{
            // we use 1/zn instead of (zf - zn) / (zf * zn), because in reality we're using
            // a projection with an infinite far plane
            (dofOptions.blurScale * Ks * Kc) * focusDistance / cameraInfo.zn,
            (dofOptions.blurScale * Ks * Kc) * (1.0f - focusDistance / cameraInfo.zn)
    };

    Blackboard& blackboard = fg.getBlackboard();
    auto depth = blackboard.get<FrameGraphTexture>("depth");
    assert(depth.isValid());

    // the downsampled target is multiple of 8, so we can have 4 clean mipmap levels
    constexpr const uint32_t maxMipLevels = 4u;
    constexpr const uint32_t maxMipLevelsMask = (1u << maxMipLevels) - 1u;
    auto const& colorDesc = fg.getDescriptor(input);
    const uint32_t width  = ((colorDesc.width  + maxMipLevelsMask) & ~maxMipLevelsMask) / 2;
    const uint32_t height = ((colorDesc.height + maxMipLevelsMask) & ~maxMipLevelsMask) / 2;
    const uint8_t maxLevelCount = FTexture::maxLevelCount(width, height);
    uint8_t mipmapCount = min(maxLevelCount, uint8_t(maxMipLevels));

    /*
     * Setup:
     *      - Downsample of color buffer
     *      - Separate near & far field
     *      - Generate Circle Of Confusion buffer
     */

    struct PostProcessDofDownsample {
        FrameGraphId<FrameGraphTexture> color;
        FrameGraphId<FrameGraphTexture> depth;
        FrameGraphId<FrameGraphTexture> outForeground;
        FrameGraphId<FrameGraphTexture> outBackground;
        FrameGraphId<FrameGraphTexture> outCocFgBg;
        FrameGraphRenderTargetHandle rt;
    };

    auto& ppDoFDownsample = fg.addPass<PostProcessDofDownsample>("DoF Downsample",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.color = builder.sample(input);
                data.depth = builder.sample(depth);

                data.outForeground = builder.createTexture("dof foreground output", {
                        .width  = width,
                        .height = height,
                        .levels = mipmapCount,
                        .format = format
                });
                data.outBackground = builder.createTexture("dof background output", {
                        .width  = width,
                        .height = height,
                        .levels = mipmapCount,
                        .format = format
                });
                data.outCocFgBg = builder.createTexture("dof CoC output", {
                        .width  = width,
                        .height = height,
                        .levels = mipmapCount,
                        .format = TextureFormat::RG16F
                });
                data.outForeground = builder.write(data.outForeground);
                data.outBackground = builder.write(data.outBackground);
                data.outCocFgBg    = builder.write(data.outCocFgBg);
                data.rt = builder.createRenderTarget("DoF Target", {
                        .attachments = {
                                { data.outForeground, data.outBackground, data.outCocFgBg }, {}, {}
                        }
                });
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                auto const& out = resources.get(data.rt);
                auto color = resources.getTexture(data.color);
                auto depth = resources.getTexture(data.depth);
                PostProcessMaterial& material = mDoFDownsample;
                FMaterialInstance* const mi = material.getMaterialInstance();
                mi->setParameter("color", color, { .filterMin = SamplerMinFilter::NEAREST });
                mi->setParameter("depth", depth, { .filterMin = SamplerMinFilter::NEAREST });
                mi->setParameter("cocParams", cocParams);
                mi->setParameter("uvscale", float4{ width, height, 1.0f / colorDesc.width, 1.0f / colorDesc.height });
                mi->commit(driver);
                mi->use(driver);
                PipelineState pipeline(material.getPipelineState(variant));
                driver.beginRenderPass(out.target, out.params);
                driver.draw(pipeline, fullScreenRenderPrimitive);
                driver.endRenderPass();
            });

    /*
     * Setup (Continued)
     *      - Generate mipmaps
     */

    struct PostProcessDofMipmap {
        FrameGraphId<FrameGraphTexture> inOutForeground;
        FrameGraphId<FrameGraphTexture> inOutBackground;
        FrameGraphId<FrameGraphTexture> inOutCocFgBg;
        FrameGraphRenderTargetHandle rt[3];
    };

    assert(mipmapCount - 1
           <= sizeof(PostProcessDofMipmap::rt) / sizeof(FrameGraphRenderTargetHandle));

    auto& ppDoFMipmap = fg.addPass<PostProcessDofMipmap>("DoF Mipmap",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.inOutForeground = builder.sample(ppDoFDownsample.getData().outForeground);
                data.inOutBackground = builder.sample(ppDoFDownsample.getData().outBackground);
                data.inOutCocFgBg    = builder.sample(ppDoFDownsample.getData().outCocFgBg);
                data.inOutForeground = builder.write(data.inOutForeground);
                data.inOutBackground = builder.write(data.inOutBackground);
                data.inOutCocFgBg    = builder.write(data.inOutCocFgBg);
                for (size_t i = 0; i < mipmapCount - 1u; i++) {
                    // make sure inputs are always multiple of two (should be true by construction)
                    // (this is so that we can compute clean mip levels)
                    assert((FTexture::valueForLevel(uint8_t(i), fg.getDescriptor(data.inOutForeground).width ) & 0x1) == 0);
                    assert((FTexture::valueForLevel(uint8_t(i), fg.getDescriptor(data.inOutForeground).height) & 0x1) == 0);
                    using Attachment = FrameGraphRenderTarget::Attachments::AttachmentInfo;
                    data.rt[i] = builder.createRenderTarget("DoF Target", {
                            .attachments = {
                                    {
                                        Attachment{ data.inOutForeground, uint8_t(i + 1) },
                                        Attachment{ data.inOutBackground, uint8_t(i + 1) },
                                        Attachment{ data.inOutCocFgBg,    uint8_t(i + 1) }
                                    },
                                    {},
                                    {}
                            }
                    });
                }
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {

                auto inOutForeground = resources.getTexture(data.inOutForeground);
                auto inOutBackground = resources.getTexture(data.inOutBackground);
                auto inOutCocFgBg    = resources.getTexture(data.inOutCocFgBg);

                PostProcessMaterial& material = mDoFMipmap;
                FMaterialInstance* const mi = material.getMaterialInstance();
                mi->setParameter("foreground", inOutForeground, { .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST });
                mi->setParameter("background", inOutBackground, { .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST });
                mi->setParameter("cocFgBg",    inOutCocFgBg,    { .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST });
                mi->use(driver);

                for (size_t level = 0 ; level < mipmapCount - 1u ; level++) {
                    auto const& out = resources.get(data.rt[level]);
                    mi->setParameter("mip", uint32_t(level));
                    mi->setParameter("weightScale", 0.5f / float(1u<<level));
                    mi->commit(driver);
                    PipelineState pipeline(material.getPipelineState(variant));
                    driver.beginRenderPass(out.target, out.params);
                    driver.draw(pipeline, fullScreenRenderPrimitive);
                    driver.endRenderPass();
                }
            });

    /*
     * Setup (Continued)
     *      - Generate min/max tiles for far/near fields (continued)
     */

    auto inTilesCocMaxMin = ppDoFDownsample.getData().outCocFgBg;

    // Match this with TILE_SIZE in dofDilate.mat
    const size_t tileSize = 16; // size of the tile in full resolution pixel
    const uint32_t tileBufferWidth  = ((colorDesc.width  + (tileSize - 1u)) & ~(tileSize - 1u)) / 4u;
    const uint32_t tileBufferHeight = ((colorDesc.height + (tileSize - 1u)) & ~(tileSize - 1u)) / 4u;
    const size_t tileReductionCount = std::log2(tileSize) - 1.0; // -1 because we start from half-resolution

    struct PostProcessDofTiling1 {
        FrameGraphId<FrameGraphTexture> inCocMaxMin;
        FrameGraphId<FrameGraphTexture> outTilesCocMaxMin;
        FrameGraphRenderTargetHandle rt;
    };

    for (size_t i = 0; i < tileReductionCount; i++) {
        auto& ppDoFTiling = fg.addPass<PostProcessDofTiling1>("DoF Tiling",
                [&](FrameGraph::Builder& builder, auto& data) {
                    assert((tileBufferWidth  & 1u) == 0);
                    assert((tileBufferHeight & 1u) == 0);
                    data.inCocMaxMin = builder.sample(inTilesCocMaxMin);
                    data.outTilesCocMaxMin = builder.createTexture("dof tiles output", {
                            .width  = tileBufferWidth  >> i,
                            .height = tileBufferHeight >> i,
                            .format = TextureFormat::RG16F
                    });
                    data.outTilesCocMaxMin = builder.write(data.outTilesCocMaxMin);
                    data.rt = builder.createRenderTarget("DoF Tiles Target", {
                            .attachments = { data.outTilesCocMaxMin }
                    });
                },
                [=](FrameGraphPassResources const& resources,
                        auto const& data, DriverApi& driver) {
                    auto const& inputDesc = resources.getDescriptor(data.inCocMaxMin);
                    auto const& outputDesc = resources.getDescriptor(data.outTilesCocMaxMin);
                    auto const& out = resources.get(data.rt);
                    auto inCocMaxMin = resources.getTexture(data.inCocMaxMin);
                    PostProcessMaterial& material = mDoFTiles;
                    FMaterialInstance* const mi = material.getMaterialInstance();
                    mi->setParameter("cocMaxMin", inCocMaxMin, { .filterMin = SamplerMinFilter::NEAREST });
                    mi->setParameter("uvscale", float4{ outputDesc.width, outputDesc.height, 1.0f / inputDesc.width, 1.0f / inputDesc.height });
                    mi->commit(driver);
                    mi->use(driver);
                    PipelineState pipeline(material.getPipelineState());
                    driver.beginRenderPass(out.target, out.params);
                    driver.draw(pipeline, fullScreenRenderPrimitive);
                    driver.endRenderPass();
                });
        inTilesCocMaxMin = ppDoFTiling.getData().outTilesCocMaxMin;
    }

    /*
     * Dilate tiles
     */

    // This is a small helper that does one round of dilate
    auto dilate = [&](FrameGraphId<FrameGraphTexture> input) -> FrameGraphId<FrameGraphTexture> {

        struct PostProcessDofDilate {
            FrameGraphId<FrameGraphTexture> inTilesCocMaxMin;
            FrameGraphId<FrameGraphTexture> outTilesCocMaxMin;
            FrameGraphRenderTargetHandle rt;
        };

        auto& ppDoFDilate = fg.addPass<PostProcessDofDilate>("DoF Dilate",
                [&](FrameGraph::Builder& builder, auto& data) {
                    auto const& inputDesc = fg.getDescriptor(input);
                    data.inTilesCocMaxMin = builder.sample(input);
                    data.outTilesCocMaxMin = builder.createTexture("dof dilated tiles output", inputDesc);
                    data.outTilesCocMaxMin = builder.write(data.outTilesCocMaxMin);
                    data.rt = builder.createRenderTarget("DoF Dilated Tiles Target", {
                            .attachments = { data.outTilesCocMaxMin }
                    });
                },
                [=](FrameGraphPassResources const& resources,
                        auto const& data, DriverApi& driver) {
                    auto const& out = resources.get(data.rt);
                    auto inTilesCocMaxMin = resources.getTexture(data.inTilesCocMaxMin);
                    PostProcessMaterial& material = mDoFDilate;
                    FMaterialInstance* const mi = material.getMaterialInstance();
                    mi->setParameter("tiles", inTilesCocMaxMin, { .filterMin = SamplerMinFilter::NEAREST });
                    mi->commit(driver);
                    mi->use(driver);
                    PipelineState pipeline(material.getPipelineState());
                    driver.beginRenderPass(out.target, out.params);
                    driver.draw(pipeline, fullScreenRenderPrimitive);
                    driver.endRenderPass();
                });
        return ppDoFDilate.getData().outTilesCocMaxMin;
    };

    // Tiles of 16 pixels requires two dilate rounds to accomodate our max Coc of 32 pixels
    auto dilated = dilate(inTilesCocMaxMin);
    dilated = dilate(dilated);

    /*
     * DoF blur pass
     */

    struct PostProcessDof {
        FrameGraphId<FrameGraphTexture> foreground;
        FrameGraphId<FrameGraphTexture> background;
        FrameGraphId<FrameGraphTexture> cocFgBg;
        FrameGraphId<FrameGraphTexture> tilesCocMaxMin;
        FrameGraphId<FrameGraphTexture> outForeground;
        FrameGraphId<FrameGraphTexture> outAlpha;
        FrameGraphRenderTargetHandle rt;
    };

    auto& ppDoF = fg.addPass<PostProcessDof>("DoF",
            [&](FrameGraph::Builder& builder, auto& data) {

                data.foreground     = builder.sample(ppDoFMipmap.getData().inOutForeground);
                data.background     = builder.sample(ppDoFMipmap.getData().inOutBackground);
                data.cocFgBg        = builder.sample(ppDoFMipmap.getData().inOutCocFgBg);
                data.tilesCocMaxMin = builder.sample(dilated);

                // The DoF buffer (output) doesn't need to be a multiple of 8 because it's not
                // mipmapped. We just need to adjust the uv properly.
                data.outForeground = builder.createTexture("dof color output", {
                        .width  = (colorDesc.width  + 1) / 2,
                        .height = (colorDesc.height + 1) / 2,
                        .format = fg.getDescriptor(data.foreground).format
                });
                data.outAlpha = builder.createTexture("dof alpha output", {
                        .width  = (colorDesc.width  + 1) / 2,
                        .height = (colorDesc.height + 1) / 2,
                        .format = TextureFormat::R8
                });
                data.outForeground  = builder.write(data.outForeground);
                data.outAlpha       = builder.write(data.outAlpha);
                data.rt = builder.createRenderTarget("DoF Target", {
                        .attachments = { { data.outForeground,
                                           data.outAlpha, {}, {} }, {}, {} }
                });
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                auto const& out = resources.get(data.rt);

                auto foreground     = resources.getTexture(data.foreground);
                auto background     = resources.getTexture(data.background);
                auto cocFgBg        = resources.getTexture(data.cocFgBg);
                auto tilesCocMaxMin = resources.getTexture(data.tilesCocMaxMin);

                auto const& inputDesc = resources.getDescriptor(data.cocFgBg);
                auto const& outputDesc = resources.getDescriptor(data.outForeground);
                auto const& tilesDesc = resources.getDescriptor(data.tilesCocMaxMin);

                PostProcessMaterial& material = mDoF;
                FMaterialInstance* const mi = material.getMaterialInstance();
                // it's not safe to use bilinear filtering in the general case (causes artifacts around edges)
                mi->setParameter("foreground",  foreground,     { .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST });
                mi->setParameter("background",  background,     { .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST });
                mi->setParameter("cocFgBg",     cocFgBg,        { .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST });
                mi->setParameter("tiles",       tilesCocMaxMin, { .filterMin = SamplerMinFilter::NEAREST });
                mi->setParameter("cocToTexelOffset", 0.5f / float2{ inputDesc.width, inputDesc.height });
                mi->setParameter("uvscale", float4{
                    outputDesc.width  / float(inputDesc.width),
                    outputDesc.height / float(inputDesc.height),
                    outputDesc.width  / (tileSize * 0.5f * tilesDesc.width),
                    outputDesc.height / (tileSize * 0.5f * tilesDesc.height)
                });
                mi->setParameter("bokehAngle",  bokehAngle);
                mi->commit(driver);
                mi->use(driver);
                PipelineState pipeline(material.getPipelineState(variant));
                driver.beginRenderPass(out.target, out.params);
                driver.draw(pipeline, fullScreenRenderPrimitive);
                driver.endRenderPass();
            });

    /*
     * DoF median
     */


    struct PostProcessDofMedian {
        FrameGraphId<FrameGraphTexture> inForeground;
        FrameGraphId<FrameGraphTexture> inAlpha;
        FrameGraphId<FrameGraphTexture> tilesCocMaxMin;
        FrameGraphId<FrameGraphTexture> outForeground;
        FrameGraphId<FrameGraphTexture> outAlpha;
        FrameGraphRenderTargetHandle rt;
    };

    auto& ppDoFMedian = fg.addPass<PostProcessDofMedian>("DoF Median",
            [&](FrameGraph::Builder& builder, auto& data) {

                data.inForeground   = builder.sample(ppDoF.getData().outForeground);
                data.inAlpha        = builder.sample(ppDoF.getData().outAlpha);
                data.tilesCocMaxMin = builder.sample(dilated);

                data.outForeground  = builder.createTexture("dof color output", fg.getDescriptor(data.inForeground));
                data.outAlpha       = builder.createTexture("dof alpha output", fg.getDescriptor(data.inAlpha));
                data.outForeground  = builder.write(data.outForeground);
                data.outAlpha       = builder.write(data.outAlpha);
                data.rt = builder.createRenderTarget("DoF Target", {
                        .attachments = { { data.outForeground,
                                                 data.outAlpha, {}, {} }, {}, {} }
                });
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                auto const& out = resources.get(data.rt);

                auto inForeground   = resources.getTexture(data.inForeground);
                auto inAlpha        = resources.getTexture(data.inAlpha);
                auto tilesCocMaxMin = resources.getTexture(data.tilesCocMaxMin);

                auto const& outputDesc = resources.getDescriptor(data.outForeground);
                auto const& tilesDesc = resources.getDescriptor(data.tilesCocMaxMin);

                PostProcessMaterial& material = mDoFMedian;
                FMaterialInstance* const mi = material.getMaterialInstance();
                mi->setParameter("dof",   inForeground,   { .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST });
                mi->setParameter("alpha", inAlpha,        { .filterMin = SamplerMinFilter::NEAREST_MIPMAP_NEAREST });
                mi->setParameter("tiles", tilesCocMaxMin, { .filterMin = SamplerMinFilter::NEAREST });
                mi->setParameter("uvscale", float2{
                        outputDesc.width  / (tileSize * 0.5f * tilesDesc.width),
                        outputDesc.height / (tileSize * 0.5f * tilesDesc.height)
                });
                mi->commit(driver);
                mi->use(driver);
                PipelineState pipeline(material.getPipelineState(variant));
                driver.beginRenderPass(out.target, out.params);
                driver.draw(pipeline, fullScreenRenderPrimitive);
                driver.endRenderPass();
            });


    /*
     * DoF recombine
     */

    struct PostProcessDofCombine {
        FrameGraphId<FrameGraphTexture> color;
        FrameGraphId<FrameGraphTexture> dof;
        FrameGraphId<FrameGraphTexture> alpha;
        FrameGraphId<FrameGraphTexture> tilesCocMaxMin;
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphRenderTargetHandle rt;
    };

    auto& ppDoFCombine = fg.addPass<PostProcessDofCombine>("DoF combine",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.color      = builder.sample(input);
                data.dof        = builder.sample(ppDoFMedian.getData().outForeground);
                data.alpha      = builder.sample(ppDoFMedian.getData().outAlpha);
                data.tilesCocMaxMin = builder.sample(dilated);
                auto const& inputDesc = fg.getDescriptor(data.color);
                data.output = builder.createTexture("dof output", inputDesc);
                data.output = builder.write(data.output);
                data.rt = builder.createRenderTarget("DoF Target", {
                        .attachments = { data.output }
                });
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {
                auto const& dofDesc = resources.getDescriptor(data.dof);
                auto const& tilesDesc = resources.getDescriptor(data.tilesCocMaxMin);
                auto const& out = resources.get(data.rt);

                auto color      = resources.getTexture(data.color);
                auto dof        = resources.getTexture(data.dof);
                auto alpha      = resources.getTexture(data.alpha);
                auto tilesCocMaxMin = resources.getTexture(data.tilesCocMaxMin);

                PostProcessMaterial& material = mDoFCombine;
                FMaterialInstance* const mi = material.getMaterialInstance();
                mi->setParameter("color", color, { .filterMin = SamplerMinFilter::NEAREST });
                mi->setParameter("dof",   dof,   { .filterMag = SamplerMagFilter::NEAREST });
                mi->setParameter("alpha", alpha, { .filterMag = SamplerMagFilter::NEAREST });
                mi->setParameter("tiles", tilesCocMaxMin, { .filterMin = SamplerMinFilter::NEAREST });
                mi->setParameter("uvscale", float4{
                    colorDesc.width  / (dofDesc.width    *  2.0f),
                    colorDesc.height / (dofDesc.height   *  2.0f),
                    colorDesc.width  / (tilesDesc.width  * float(tileSize)),
                    colorDesc.height / (tilesDesc.height * float(tileSize))
                });
                mi->commit(driver);
                mi->use(driver);
                PipelineState pipeline(material.getPipelineState(variant));
                driver.beginRenderPass(out.target, out.params);
                driver.draw(pipeline, fullScreenRenderPrimitive);
                driver.endRenderPass();
            });

    return ppDoFCombine.getData().output;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::bloomPass(FrameGraph& fg,
        FrameGraphId<FrameGraphTexture> input, TextureFormat outFormat,
        View::BloomOptions& bloomOptions, float2 scale) noexcept {

    Handle<HwRenderPrimitive> fullScreenRenderPrimitive = mEngine.getFullScreenRenderPrimitive();

    // Figure out a good size for the bloom buffer. We pick the major axis lower
    // power of two, and scale the minor axis accordingly taking dynamic scaling into account.
    auto const& desc = fg.getDescriptor(input);
    uint32_t width = desc.width / scale.x;
    uint32_t height = desc.height / scale.y;
    if (bloomOptions.anamorphism >= 1.0) {
        height *= bloomOptions.anamorphism;
    } else if (bloomOptions.anamorphism < 1.0) {
        width *= 1.0f / std::max(bloomOptions.anamorphism, 1.0f / 4096.0f);
    }
    uint32_t& major = width > height ? width : height;
    uint32_t& minor = width < height ? width : height;
    uint32_t newMinor = clamp(bloomOptions.resolution,
            1u << bloomOptions.levels, std::min(minor, 1u << kMaxBloomLevels));
    major = major * uint64_t(newMinor) / minor;
    minor = newMinor;

    // we might need to adjust the max # of levels
    const uint8_t maxLevels = FTexture::maxLevelCount(major);
    bloomOptions.levels = std::min(bloomOptions.levels, maxLevels);
    bloomOptions.levels = std::min(bloomOptions.levels, kMaxBloomLevels);

    if (2 * width < desc.width || 2 * height < desc.height) {
        // if we're scaling down by more than 2x, prescale the image with a blit to improve
        // performance. This is important on mobile/tilers.
        input = opaqueBlit(fg, input, {
                .width = desc.width / 2,
                .height = desc.height / 2,
                .format = outFormat
        });
    }

    struct BloomPassData {
        FrameGraphId<FrameGraphTexture> in;
        FrameGraphId<FrameGraphTexture> out;
        FrameGraphRenderTargetHandle outRT[kMaxBloomLevels];
    };

    // downsample phase
    auto& bloomDownsamplePass = fg.addPass<BloomPassData>("Bloom Downsample",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.in = builder.sample(input);
                data.out = builder.createTexture("Bloom Texture", {
                        .width = width,
                        .height = height,
                        .levels = bloomOptions.levels,
                        .format = outFormat
                });
                data.out = builder.write(builder.sample(data.out));

                for (size_t i = 0; i < bloomOptions.levels; i++) {
                    data.outRT[i] = builder.createRenderTarget("Bloom target", {
                            .attachments = {{ data.out, uint8_t(i) }} });
                }
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {

                PostProcessMaterial const& bloomDownsample = mBloomDownsample;
                FMaterialInstance* mi = bloomDownsample.getMaterialInstance();

                const PipelineState pipeline(bloomDownsample.getPipelineState());

                auto hwIn = resources.getTexture(data.in);
                auto hwOut = resources.getTexture(data.out);
                auto const& outDesc = resources.getDescriptor(data.out);

                mi->use(driver);
                mi->setParameter("source", hwIn,  {
                        .filterMag = SamplerMagFilter::LINEAR,
                        .filterMin = SamplerMinFilter::LINEAR /* level is always 0 */
                });
                mi->setParameter("level", 0.0f);
                mi->setParameter("threshold", bloomOptions.threshold ? 1.0f : 0.0f);

                for (size_t i = 0; i < bloomOptions.levels; i++) {
                    auto hwOutRT = resources.get(data.outRT[i]);

                    auto w = FTexture::valueForLevel(i, outDesc.width);
                    auto h = FTexture::valueForLevel(i, outDesc.height);
                    mi->setParameter("resolution", float4{ w, h, 1.0f / w, 1.0f / h });
                    mi->commit(driver);

                    hwOutRT.params.flags.discardStart = TargetBufferFlags::COLOR;
                    hwOutRT.params.flags.discardEnd = TargetBufferFlags::NONE;
                    driver.beginRenderPass(hwOutRT.target, hwOutRT.params);
                    driver.draw(pipeline, fullScreenRenderPrimitive);
                    driver.endRenderPass();

                    // prepare the next level
                    mi->setParameter("source", hwOut,  {
                            .filterMag = SamplerMagFilter::LINEAR,
                            .filterMin = SamplerMinFilter::LINEAR_MIPMAP_NEAREST
                    });
                    mi->setParameter("level", float(i));
                }
            });

    input = bloomDownsamplePass.getData().out;

    // upsample phase
    auto& bloomUpsamplePass = fg.addPass<BloomPassData>("Bloom Upsample",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.in = builder.sample(input);
                data.out = builder.write(input);

                for (size_t i = 0; i < bloomOptions.levels; i++) {
                    data.outRT[i] = builder.createRenderTarget("Bloom target", {
                            .attachments = {{ data.out, uint8_t(i) }} });
                }
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {

                auto hwIn = resources.getTexture(data.in);
                auto const& outDesc = resources.getDescriptor(data.out);

                PostProcessMaterial const& bloomUpsample = mBloomUpsample;
                FMaterialInstance* mi = bloomUpsample.getMaterialInstance();
                PipelineState pipeline(bloomUpsample.getPipelineState());
                pipeline.rasterState.blendFunctionSrcRGB = BlendFunction::ONE;
                pipeline.rasterState.blendFunctionDstRGB = BlendFunction::ONE;

                mi->use(driver);

                for (size_t i = bloomOptions.levels - 1; i >= 1; i--) {
                    auto hwDstRT = resources.get(data.outRT[i - 1]);
                    hwDstRT.params.flags.discardStart = TargetBufferFlags::NONE; // because we'll blend
                    hwDstRT.params.flags.discardEnd = TargetBufferFlags::NONE;

                    auto w = FTexture::valueForLevel(i - 1, outDesc.width);
                    auto h = FTexture::valueForLevel(i - 1, outDesc.height);
                    mi->setParameter("resolution", float4{ w, h, 1.0f / w, 1.0f / h });
                    mi->setParameter("source", hwIn, {
                            .filterMag = SamplerMagFilter::LINEAR,
                            .filterMin = SamplerMinFilter::LINEAR_MIPMAP_NEAREST
                    });
                    mi->setParameter("level", float(i));
                    mi->commit(driver);

                    driver.beginRenderPass(hwDstRT.target, hwDstRT.params);
                    driver.draw(pipeline, fullScreenRenderPrimitive);
                    driver.endRenderPass();
                }
            });

    return bloomUpsamplePass.getData().out;
}

static float4 getVignetteParameters(View::VignetteOptions options, uint32_t width, uint32_t height) {
    if (options.enabled) {
        // Vignette params
        // From 0.0 to 0.5 the vignette is a rounded rect that turns into an oval
        // From 0.5 to 1.0 the vignette turns from oval to circle
        float oval = min(options.roundness, 0.5f) * 2.0f;
        float circle = (max(options.roundness, 0.5f) - 0.5f) * 2.0f;
        float roundness = (1.0f - oval) * 6.0f + oval;

        // Mid point varies during the oval/rounded section of roundness
        // We also modify it to emphasize feathering
        float midPoint = (1.0f - options.midPoint) * mix(2.2f, 3.0f, oval)
                         * (1.0f - 0.1f * options.feather);

        // Radius of the rounded corners as a param to pow()
        float radius = roundness *
                mix(1.0f + 4.0f * (1.0f - options.feather), 1.0f, std::sqrtf(oval));

        // Factor to transform oval into circle
        float aspect = mix(1.0f, float(width) / float(height), circle);

        return float4{midPoint, radius, aspect, options.feather};
    }

    // Set half-max to show disabled
    return float4{std::numeric_limits<half>::max()};
}

void PostProcessManager::colorGradingPrepareSubpass(DriverApi& driver,
        const FColorGrading* colorGrading, View::VignetteOptions vignetteOptions, bool fxaa,
        bool dithering, uint32_t width, uint32_t height) noexcept {

    float4 vignetteParameters = getVignetteParameters(vignetteOptions, width, height);

    FMaterialInstance* mi = mColorGradingAsSubpass.getMaterialInstance();
    mi->setParameter("lut", colorGrading->getHwHandle(), {
            .filterMag = SamplerMagFilter::LINEAR,
            .filterMin = SamplerMinFilter::LINEAR
    });
    mi->setParameter("vignette", vignetteParameters);
    mi->setParameter("vignetteColor", vignetteOptions.color);
    mi->setParameter("dithering", dithering);
    mi->setParameter("fxaa", fxaa);
    mi->commit(driver);
}

void PostProcessManager::colorGradingSubpass(DriverApi& driver,  bool translucent) noexcept {
    FEngine& engine = mEngine;
    Handle<HwRenderPrimitive> const& fullScreenRenderPrimitive = engine.getFullScreenRenderPrimitive();

    mColorGradingAsSubpass.getMaterialInstance()->use(driver);
    const uint8_t variant = uint8_t(translucent ?
            PostProcessVariant::TRANSLUCENT : PostProcessVariant::OPAQUE);

    driver.nextSubpass();
    driver.draw(mColorGradingAsSubpass.getPipelineState(variant), fullScreenRenderPrimitive);
}

FrameGraphId<FrameGraphTexture> PostProcessManager::colorGrading(FrameGraph& fg,
        FrameGraphId<FrameGraphTexture> input, const FColorGrading* colorGrading,
        TextureFormat outFormat, bool translucent, bool fxaa, float2 scale,
        View::BloomOptions bloomOptions, View::VignetteOptions vignetteOptions, bool dithering) noexcept {

    FEngine& engine = mEngine;
    Handle<HwRenderPrimitive> const& fullScreenRenderPrimitive = engine.getFullScreenRenderPrimitive();

    struct PostProcessColorGrading {
        FrameGraphId<FrameGraphTexture> input;
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphId<FrameGraphTexture> bloom;
        FrameGraphId<FrameGraphTexture> dirt;
        FrameGraphRenderTargetHandle rt;
    };

    FrameGraphId<FrameGraphTexture> bloomBlur;
    FrameGraphId<FrameGraphTexture> bloomDirt;

    float bloom = 0.0f;
    if (bloomOptions.enabled) {
        bloom = clamp(bloomOptions.strength, 0.0f, 1.0f);
        bloomBlur = bloomPass(fg, input, TextureFormat::R11F_G11F_B10F, bloomOptions, scale);
        if (bloomOptions.dirt) {
            FTexture* fdirt = upcast(bloomOptions.dirt);
            FrameGraphTexture frameGraphTexture { .texture = fdirt->getHwHandle() };
            bloomDirt = fg.import("dirt", {
                    .width = (uint32_t)fdirt->getWidth(0u),
                    .height = (uint32_t)fdirt->getHeight(0u),
                    .format = fdirt->getFormat()
            }, frameGraphTexture);
        }
    }

    auto& ppColorGrading = fg.addPass<PostProcessColorGrading>("colorGrading",
            [&](FrameGraph::Builder& builder, auto& data) {
                auto const& inputDesc = fg.getDescriptor(input);
                data.input = builder.sample(input);
                data.output = builder.createTexture("colorGrading output", {
                        .width = inputDesc.width,
                        .height = inputDesc.height,
                        .format = outFormat
                });
                data.output = builder.write(data.output);
                data.rt = builder.createRenderTarget("colorGrading Target", {
                        .attachments = { data.output } });

                if (bloomBlur.isValid()) {
                    data.bloom = builder.sample(bloomBlur);
                }
                if (bloomDirt.isValid()) {
                    data.dirt = builder.sample(bloomDirt);
                }
            },
            [=](FrameGraphPassResources const& resources, auto const& data, DriverApi& driver) {
                Handle<HwTexture> colorTexture = resources.getTexture(data.input);

                Handle<HwTexture> bloomTexture =
                        data.bloom.isValid() ? resources.getTexture(data.bloom) : getZeroTexture();

                Handle<HwTexture> dirtTexture =
                        data.dirt.isValid() ? resources.getTexture(data.dirt) : getOneTexture();

                FMaterialInstance* mi = mColorGrading.getMaterialInstance();
                mi->setParameter("lut", colorGrading->getHwHandle(), {
                        .filterMag = SamplerMagFilter::LINEAR,
                        .filterMin = SamplerMinFilter::LINEAR
                });
                mi->setParameter("colorBuffer", colorTexture, { /* shader uses texelFetch */ });
                mi->setParameter("bloomBuffer", bloomTexture, {
                        .filterMag = SamplerMagFilter::LINEAR,
                        .filterMin = SamplerMinFilter::LINEAR /* always read base level in shader */
                });
                mi->setParameter("dirtBuffer", dirtTexture, {
                        .filterMag = SamplerMagFilter::LINEAR,
                        .filterMin = SamplerMinFilter::LINEAR
                });

                // Bloom params
                float4 bloomParameters{
                    bloom / float(bloomOptions.levels),
                    1.0f,
                    (bloomOptions.enabled && bloomOptions.dirt) ? bloomOptions.dirtStrength : 0.0f,
                    0.0f
                };
                if (bloomOptions.blendMode == View::BloomOptions::BlendMode::INTERPOLATE) {
                    bloomParameters.y = 1.0f - bloomParameters.x;
                }

                auto const& output = resources.getDescriptor(data.output);
                float4 vignetteParameters = getVignetteParameters(
                        vignetteOptions, output.width, output.height);

                mi->setParameter("dithering", dithering);
                mi->setParameter("bloom", bloomParameters);
                mi->setParameter("vignette", vignetteParameters);
                mi->setParameter("vignetteColor", vignetteOptions.color);
                mi->setParameter("fxaa", fxaa);
                mi->commit(driver);
                mi->use(driver);

                const uint8_t variant = uint8_t(translucent ?
                            PostProcessVariant::TRANSLUCENT : PostProcessVariant::OPAQUE);

                auto const& target = resources.get(data.rt);
                driver.beginRenderPass(target.target, target.params);
                driver.draw(mColorGrading.getPipelineState(variant), fullScreenRenderPrimitive);
                driver.endRenderPass();
            }
    );

    return ppColorGrading.getData().output;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::fxaa(FrameGraph& fg,
        FrameGraphId<FrameGraphTexture> input,
        TextureFormat outFormat, bool translucent) noexcept {

    FEngine& engine = mEngine;
    Handle<HwRenderPrimitive> const& fullScreenRenderPrimitive = engine.getFullScreenRenderPrimitive();

    struct PostProcessFXAA {
        FrameGraphId<FrameGraphTexture> input;
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphRenderTargetHandle rt;
    };

    auto& ppFXAA = fg.addPass<PostProcessFXAA>("fxaa",
            [&](FrameGraph::Builder& builder, auto& data) {
                auto const& inputDesc = fg.getDescriptor(input);
                data.input = builder.sample(input);
                data.output = builder.createTexture("fxaa output", {
                        .width = inputDesc.width,
                        .height = inputDesc.height,
                        .format = outFormat
                });
                data.output = builder.write(data.output);
                data.rt = builder.createRenderTarget("FXAA Target", {
                        .attachments = { data.output } });
            },
            [=](FrameGraphPassResources const& resources,
                auto const& data, DriverApi& driver) {
                auto const& texture = resources.getTexture(data.input);

                FMaterialInstance* mi = mFxaa.getMaterialInstance();
                mi->setParameter("colorBuffer", texture, {
                        .filterMag = SamplerMagFilter::LINEAR,
                        .filterMin = SamplerMinFilter::LINEAR
                });

                mi->commit(driver);
                mi->use(driver);

                const uint8_t variant = uint8_t(translucent ?
                    PostProcessVariant::TRANSLUCENT : PostProcessVariant::OPAQUE);

                auto const& target = resources.get(data.rt);
                driver.beginRenderPass(target.target, target.params);
                driver.draw(mFxaa.getPipelineState(variant), fullScreenRenderPrimitive);
                driver.endRenderPass();
            });

    return ppFXAA.getData().output;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::opaqueBlit(FrameGraph& fg,
        FrameGraphId<FrameGraphTexture> input, FrameGraphTexture::Descriptor outDesc,
        SamplerMagFilter filter) noexcept {

    struct PostProcessScaling {
        FrameGraphId<FrameGraphTexture> input;
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphRenderTargetHandle srt;
        FrameGraphRenderTargetHandle drt;
    };

    auto& ppBlit = fg.addPass<PostProcessScaling>("blit scaling",
            [&](FrameGraph::Builder& builder, auto& data) {
                auto const& inputDesc = fg.getDescriptor(input);

                // we currently have no use for this case, so we just assert. This is better for now to trap
                // cases that we might not intend.
                assert(inputDesc.samples <= 1);

                // FIXME: here we use sample() instead of read() because this forces the
                //      backend to use a texture (instead of a renderbuffer). We need this because
                //      "implicit resolve" renderbuffers are currently not supported -- and
                //      implicit resolves are needed when taking the blit path.
                //      (we do this only when the texture does not request multisampling, since
                //      these are not sampleable).
                data.input = (inputDesc.samples > 1) ? builder.read(input) : builder.sample(input);

                data.srt = builder.createRenderTarget(builder.getName(data.input), {
                        .attachments = { data.input },
                        // We must set the sample count (as opposed to leaving to 0) to express
                        // the fact that we want a new rendertarget (as opposed to match one
                        // that might exist with multisample enabled). This is because sample
                        // count is only matched if specified.
                        .samples = std::max(uint8_t(1), inputDesc.samples)
                });

                data.output = builder.createTexture("scaled output", outDesc);
                data.output = builder.write(data.output);
                data.drt = builder.createRenderTarget("Scaled Target", {
                        .attachments = { data.output } });
            },
            [=](FrameGraphPassResources const& resources, auto const& data, DriverApi& driver) {
                auto in = resources.get(data.srt);
                auto out = resources.get(data.drt);
                driver.blit(TargetBufferFlags::COLOR,
                        out.target, out.params.viewport, in.target, in.params.viewport, filter);
            });

    // we rely on automatic culling of unused render passes
    return ppBlit.getData().output;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::blendBlit(
        FrameGraph& fg, bool translucent, View::QualityLevel quality,
        FrameGraphId<FrameGraphTexture> input,
        FrameGraphTexture::Descriptor outDesc) noexcept {

    Handle<HwRenderPrimitive> fullScreenRenderPrimitive = mEngine.getFullScreenRenderPrimitive();

    struct QuadBlitData {
        FrameGraphId<FrameGraphTexture> input;
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphRenderTargetHandle drt;
    };

    auto& ppQuadBlit = fg.addPass<QuadBlitData>("quad scaling",
            [&](FrameGraph::Builder& builder, auto& data) {
                data.input = builder.sample(input);
                data.output = builder.createTexture("scaled output", outDesc);
                data.output = builder.write(data.output);
                data.drt = builder.createRenderTarget("Scaled Target",{
                        .attachments = { data.output } });
            },
            [=](FrameGraphPassResources const& resources,
                    auto const& data, DriverApi& driver) {

                auto color = resources.getTexture(data.input);
                auto out = resources.get(data.drt);
                auto const& desc = resources.getDescriptor(data.input);

                unsigned index = std::min(2u, (unsigned)quality);
                PostProcessMaterial& material = mBlit[index];
                FMaterialInstance* const mi = material.getMaterialInstance();
                mi->setParameter("color", color, {
                        .filterMag = SamplerMagFilter::LINEAR,
                        .filterMin = SamplerMinFilter::LINEAR
                });
                mi->setParameter("resolution",
                        float4{ desc.width, desc.height, 1.0f / desc.width, 1.0f / desc.height });
                mi->commit(driver);
                mi->use(driver);

                PipelineState pipeline(material.getPipelineState());
                if (translucent) {
                    pipeline.rasterState.blendFunctionSrcRGB   = BlendFunction::ONE;
                    pipeline.rasterState.blendFunctionSrcAlpha = BlendFunction::ONE;
                    pipeline.rasterState.blendFunctionDstRGB   = BlendFunction::ONE_MINUS_SRC_ALPHA;
                    pipeline.rasterState.blendFunctionDstAlpha = BlendFunction::ONE_MINUS_SRC_ALPHA;
                }
                driver.beginRenderPass(out.target, out.params);
                driver.draw(pipeline, fullScreenRenderPrimitive);
                driver.endRenderPass();
            });

    // we rely on automatic culling of unused render passes
    return ppQuadBlit.getData().output;
}

FrameGraphId<FrameGraphTexture> PostProcessManager::resolve(FrameGraph& fg,
        const char* outputBufferName, FrameGraphId<FrameGraphTexture> input) noexcept {

    // Don't do anything if we're not a MSAA buffer
    auto desc = fg.getDescriptor(input);
    if (desc.samples <= 1) {
        return input;
    }

    struct ResolveData {
        FrameGraphId<FrameGraphTexture> output;
        FrameGraphRenderTargetHandle srt;
        FrameGraphRenderTargetHandle drt;
    };

    auto& ppResolve = fg.addPass<ResolveData>("resolve",
            [&](FrameGraph::Builder& builder, auto& data) {
                auto outputDesc = desc;
                input = builder.read(input);
                data.srt = builder.createRenderTarget(builder.getName(input), {
                        .attachments = { input }, .samples = desc.samples });

                outputDesc.levels = 1;
                outputDesc.samples = 0;
                data.output = builder.createTexture(outputBufferName, outputDesc);
                data.output = builder.write(data.output);
                data.drt = builder.createRenderTarget(outputBufferName, {
                        .attachments = { data.output } });
            },
            [](FrameGraphPassResources const& resources, auto const& data, DriverApi& driver) {
                auto in = resources.get(data.srt);
                auto out = resources.get(data.drt);
                driver.blit(TargetBufferFlags::COLOR,
                        out.target, out.params.viewport, in.target, in.params.viewport,
                        SamplerMagFilter::NEAREST);
            });
    return ppResolve.getData().output;
}

} // namespace filament
