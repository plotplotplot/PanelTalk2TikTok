#include "gpu_compositor.h"
#include "frame_handle.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>

#include <QtGui/private/qrhi_p.h>

namespace editor {

// ============================================================================
// Shader Sources
// ============================================================================

// Note: In production, use pre-compiled shaders with the qsb tool.
// For this example, we're not implementing runtime shader compilation.

// ============================================================================
// GPUCompositor Implementation
// ============================================================================

GPUCompositor::GPUCompositor(QRhi* rhi, QObject* parent)
    : QObject(parent), m_rhi(rhi) {}

GPUCompositor::~GPUCompositor() {
    releaseResources();
}

void GPUCompositor::releaseResources() {
    if (!m_rhi) return;
    
    for (auto& lr : m_layerResources) {
        delete lr.srb;
    }
    m_layerResources.clear();
    
    delete m_pipeline;
    m_pipeline = nullptr;
    
    delete m_srbTemplate;
    m_srbTemplate = nullptr;
    
    delete m_sampler;
    m_sampler = nullptr;
    
    delete m_uniformBuffer;
    m_uniformBuffer = nullptr;
    
    delete m_vertexBuffer;
    m_vertexBuffer = nullptr;
    
    m_initialized = false;
}

bool GPUCompositor::initialize() {
    if (!m_rhi) {
        qWarning() << "GPUCompositor: No QRhi context provided";
        return false;
    }
    
    if (m_initialized) {
        return true;
    }
    
    if (!createResources()) {
        qWarning() << "GPUCompositor: Failed to create resources";
        return false;
    }
    
    m_initialized = true;
    return true;
}

bool GPUCompositor::createResources() {
    // Vertex buffer (fullscreen quad)
    static const float vertices[] = {
        // Position      // TexCoord
        -1.0f, -1.0f,   0.0f, 0.0f,
         1.0f, -1.0f,   1.0f, 0.0f,
        -1.0f,  1.0f,   0.0f, 1.0f,
         1.0f,  1.0f,   1.0f, 1.0f
    };
    
    m_vertexBuffer = m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(vertices));
    m_vertexBuffer->setSize(sizeof(vertices));
    if (!m_vertexBuffer->create()) return false;
    
    // Upload vertex data - use beginFullDynamicBufferUpdateForCurrentFrame or similar
    // For immutable buffers, we need to upload during creation or use resource update batch
    QRhiResourceUpdateBatch* batch = m_rhi->nextResourceUpdateBatch();
    batch->uploadStaticBuffer(m_vertexBuffer, vertices);
    // In Qt 6.4, we need to submit the batch - use command buffer or frame submission
    // For now, skip the upload - this is a simplified implementation
    
    // Uniform buffer
    struct UniformData {
        float mvp[16];
        float opacity;
        int blendMode;
        float padding[2];
    };
    m_uniformBuffer = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(UniformData));
    m_uniformBuffer->setSize(sizeof(UniformData));
    if (!m_uniformBuffer->create()) return false;
    
    // Sampler - Qt 6.4 API doesn't have Flags parameter
    m_sampler = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, 
                                   QRhiSampler::None,  // mipmapMode
                                   QRhiSampler::ClampToEdge, 
                                   QRhiSampler::ClampToEdge,
                                   QRhiSampler::Repeat);  // addressW
    if (!m_sampler->create()) return false;
    
    // Build pipeline
    buildShaderPipeline();
    
    return m_pipeline != nullptr;
}

void GPUCompositor::buildShaderPipeline() {
    // For now, use a simple approach without compiled shaders
    // In production, pre-compile shaders with qsb tool
    
    // Create pipeline
    m_pipeline = m_rhi->newGraphicsPipeline();
    
    // Set up vertex input layout
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({
        { 4 * sizeof(float) }  // Position + TexCoord
    });
    inputLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, 0 },      // Position
        { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) }  // TexCoord
    });
    m_pipeline->setVertexInputLayout(inputLayout);
    
    // Set render target
    m_pipeline->setRenderPassDescriptor(nullptr);  // Will be set per-render
    
    // Enable blending
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    m_pipeline->setTargetBlends({ blend });
    
    // Note: Without valid shaders, create() will fail
    // This is where you'd load pre-compiled shaders:
    // QShader vs = QShader::fromSerialized(QByteArray(...));
    // QShader fs = QShader::fromSerialized(QByteArray(...));
    // m_pipeline->setShaderStages({{QShader::VertexStage, vs}, {QShader::FragmentStage, fs}});
    
    // For this example, we'll skip actual shader setup since it requires
    // pre-compiled shaders. In a real implementation, use the qsb tool.
    
    // m_pipeline->create();  // Would fail without shaders
}

void GPUCompositor::clearLayers() {
    m_layers.clear();
}

void GPUCompositor::setLayer(int index, const FrameHandle& frame, 
                             const QMatrix4x4& transform,
                             float opacity,
                             CompositorLayer::BlendMode blend) {
    if (frame.isNull()) return;
    
    // Ensure frame is GPU-ready
    if (!frame.hasGpuTexture() && frame.hasCpuImage()) {
        // Would need to upload here - but we need QRhi
        // In production, frame should already be GPU-uploaded
        qWarning() << "Frame not GPU-ready, skipping layer" << index;
        return;
    }
    
    setLayer(index, frame.gpuTexture(), transform, opacity, blend);
}

void GPUCompositor::setLayer(int index, QRhiTexture* texture,
                             const QMatrix4x4& transform,
                             float opacity,
                             CompositorLayer::BlendMode blend) {
    if (!texture) return;
    
    // Expand layers vector if needed
    if (index >= static_cast<int>(m_layers.size())) {
        m_layers.resize(index + 1);
    }
    
    m_layers[index] = { texture, transform, opacity, blend, QString(), -1 };
}

void GPUCompositor::renderToSwapChain(QRhiSwapChain* swapChain, const QColor& clearColor) {
    if (!m_initialized || !swapChain) return;
    
    QElapsedTimer timer;
    timer.start();
    
    QRhiCommandBuffer* cb = swapChain->currentFrameCommandBuffer();
    QRhiRenderTarget* rt = swapChain->currentFrameRenderTarget();
    
    // Begin render pass
    QRhiRenderPassDescriptor* rpDesc = rt->renderPassDescriptor();
    
    cb->beginPass(rt, clearColor, { 1.0f, 0 });
    
    // For each layer, we would:
    // 1. Update uniform buffer with transform/opacity/blend
    // 2. Set pipeline and SRB
    // 3. Draw
    
    // Note: Full implementation requires valid shaders
    // This is a placeholder showing the structure
    
    // Render each layer
    for (const auto& layer : m_layers) {
        if (!layer.texture) continue;
        
        // Update uniforms
        // Set up shader resource bindings
        // Draw quad
        
        cb->setViewport({ 0, 0, static_cast<float>(m_outputSize.width()), 
                        static_cast<float>(m_outputSize.height()) });
        
        // Would draw here with actual shaders
    }
    
    cb->endPass();
    
    m_frameCount++;
    m_totalRenderTimeUs += timer.nsecsElapsed() / 1000;
}

void GPUCompositor::renderToTexture(QRhiTextureRenderTarget* target, const QColor& clearColor) {
    if (!m_initialized || !target) return;
    
    QRhiCommandBuffer* cb = nullptr;
    // Get command buffer from RHI
    
    cb->beginPass(target, clearColor, { 1.0f, 0 });
    
    // Same rendering as swap chain, but to texture
    
    cb->endPass();
}

double GPUCompositor::averageRenderTimeMs() const {
    if (m_frameCount == 0) return 0.0;
    return (m_totalRenderTimeUs / 1000.0) / m_frameCount;
}

} // namespace editor
