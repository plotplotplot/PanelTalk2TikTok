#pragma once

#include "qt_compat.h"  // Qt 6.4/GCC 13 compatibility
#include <QObject>
#include <QMatrix4x4>
#include <QColor>
#include <memory>
#include <vector>

// QRhi forward declarations
class QRhi;
class QRhiTexture;
class QRhiSwapChain;
class QRhiTextureRenderTarget;
class QRhiBuffer;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;
class QRhiResourceUpdateBatch;
class QRhiSampler;

namespace editor {

class FrameHandle;

// ============================================================================
// CompositorLayer - Single layer in the composition
// ============================================================================
struct CompositorLayer {
    QRhiTexture* texture = nullptr;
    QMatrix4x4 transform;
    float opacity = 1.0f;
    
    // Blend mode
    enum class BlendMode {
        Normal,     // Standard alpha over
        Add,        // Additive
        Multiply,   // Multiplicative
        Screen,     // Screen blend
        Overlay     // Overlay
    };
    BlendMode blendMode = BlendMode::Normal;
    
    // Source frame info for validation
    QString sourcePath;
    int64_t frameNumber = -1;
};

// ============================================================================
// GPUCompositor - GPU-based frame compositing using QRhi
// 
// Composites multiple video layers with transforms, opacity, and blend modes.
// All operations happen on the GPU - no CPU-GPU transfer during composition.
// ============================================================================
class GPUCompositor : public QObject {
    Q_OBJECT
public:
    explicit GPUCompositor(QRhi* rhi, QObject* parent = nullptr);
    ~GPUCompositor();
    
    bool initialize();
    bool isInitialized() const { return m_initialized; }
    
    // Layer management
    void clearLayers();
    void setLayer(int index, const FrameHandle& frame, 
                  const QMatrix4x4& transform = QMatrix4x4(),
                  float opacity = 1.0f,
                  CompositorLayer::BlendMode blend = CompositorLayer::BlendMode::Normal);
    void setLayer(int index, QRhiTexture* texture,
                  const QMatrix4x4& transform = QMatrix4x4(),
                  float opacity = 1.0f,
                  CompositorLayer::BlendMode blend = CompositorLayer::BlendMode::Normal);
    
    int layerCount() const { return static_cast<int>(m_layers.size()); }
    
    // Rendering
    void renderToSwapChain(QRhiSwapChain* swapChain, const QColor& clearColor = QColor(20, 24, 28));
    void renderToTexture(QRhiTextureRenderTarget* target, const QColor& clearColor = QColor(20, 24, 28));
    
    // Render target size
    void setOutputSize(const QSize& size) { m_outputSize = size; }
    QSize outputSize() const { return m_outputSize; }
    
    // Resource cleanup
    void releaseResources();
    
    // Statistics
    int frameCount() const { return m_frameCount; }
    double averageRenderTimeMs() const;

signals:
    void renderError(const QString& message);

private:
    bool createResources();
    void updateUniformBuffer(QRhiResourceUpdateBatch* batch);
    void buildShaderPipeline();
    
    // Shader code (would use pre-compiled QShader in production)
    // static QShader loadShader(const QString& filename);
    
    QRhi* m_rhi = nullptr;
    bool m_initialized = false;
    QSize m_outputSize;
    
    // Resources
    QRhiBuffer* m_vertexBuffer = nullptr;
    QRhiBuffer* m_uniformBuffer = nullptr;
    QRhiSampler* m_sampler = nullptr;
    QRhiShaderResourceBindings* m_srbTemplate = nullptr;
    QRhiGraphicsPipeline* m_pipeline = nullptr;
    
    // Per-layer resources
    struct LayerResources {
        QRhiShaderResourceBindings* srb = nullptr;
        QRhiTexture* texture = nullptr;  // Not owned
    };
    std::vector<LayerResources> m_layerResources;
    
    // Layer data
    std::vector<CompositorLayer> m_layers;
    
    // Statistics
    int m_frameCount = 0;
    qint64 m_totalRenderTimeUs = 0;
};

} // namespace editor
