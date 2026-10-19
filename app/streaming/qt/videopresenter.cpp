#include "videopresenter.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QCoreApplication>
#include <QRunnable>
#include <QThread>
#include <QTimer>

struct QtVideoPresenter::State {
    std::mutex mutex;
    QtVideoPresenter* owner = nullptr; // posting/retirement serialized by mutex
    QtVideoMailbox::Producer producer;
    std::shared_ptr<QtEglVideoStatus> primary;
    std::vector<std::shared_ptr<QtEglVideoStatus>> outputs;
    std::vector<bool> required;
    size_t primaryIndex = 0;
    bool probeDelivered = false;
    std::weak_ptr<const QtVideoFrame> probeFrame;
    bool live = false;
};

namespace {
QString currentRenderNode()
{
    const auto queryDisplay = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(eglGetProcAddress("eglQueryDisplayAttribEXT"));
    const auto queryDevice = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));
    const auto display = eglGetCurrentDisplay();
    EGLAttrib device = 0;
    if (!queryDisplay || !queryDevice || display == EGL_NO_DISPLAY ||
            !queryDisplay(display, EGL_DEVICE_EXT, &device) || !device) return {};
    const auto eglDevice = reinterpret_cast<EGLDeviceEXT>(device);
    const auto extensions = QByteArray(queryDevice(eglDevice, EGL_EXTENSIONS)).split(' ');
    if (!extensions.contains("EGL_EXT_device_drm_render_node")) return {};
    return QString::fromUtf8(queryDevice(eglDevice, EGL_DRM_RENDER_NODE_FILE_EXT));
}

void wakeQualification(const std::shared_ptr<QtEglVideoStatus>& status)
{
    if (!status) return;
    std::lock_guard<std::mutex> lock(status->presentationMutex);
    status->presentationChanged.notify_all();
}
}

QtVideoPresenter::QtVideoPresenter(QObject* parent)
    : QObject(parent), m_State(std::make_shared<State>()),
      m_Mailbox([this](QtVideoFrame::Ptr frame, QtVideoMailbox::Producer producer) { deliver(std::move(frame), producer); })
{
    m_State->owner = this;
    m_State->producer = m_Mailbox.startGeneration();
    // A bounded status check, not one queued GUI signal per rendered frame.
    m_ErrorTimer.setInterval(100);
    connect(&m_ErrorTimer, &QTimer::timeout, this, [this] {
        for (const auto& output : m_Outputs) {
            if (output.window && output.window->isVisible() && output.item &&
                    output.item->status()->error.load() != QtEglVideoStatus::None) {
                // Qualification reports failure to the decoder so it can try
                // the exact software format. Only a live failure ends output.
                std::unique_lock<std::mutex> lock(m_State->mutex);
                if (!m_State->live || m_State->primary != m_DisplayedGeneration ||
                        !m_State->producer.isCurrent()) return;
                lock.unlock();
                stop();
                emit presentationFailed(QStringLiteral("Qt could not preserve the selected video format on this output."));
                return;
            }
        }
    });
    m_ErrorTimer.start();
}

QtVideoPresenter::~QtVideoPresenter()
{
    stop();
    for (auto& output : m_Outputs) delete output.item;
}

void QtVideoPresenter::addOutput(QQuickWindow* window, const QtSessionGeometry& geometry, bool primary)
{
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!m_Started && window);
    auto* item = new QtEglVideoItem(window->contentItem());
    item->setPresentation(geometry);
    item->setPrimaryOutput(primary);
    m_Outputs.push_back({window, item, geometry, primary});
    m_State->outputs.push_back(item->presentationFeedback());
    m_State->required.push_back(false);
    if (primary) {
        m_State->primary = item->presentationFeedback();
        m_State->primaryIndex = m_Outputs.size() - 1;
    }
}

void QtVideoPresenter::setGeometry(QQuickWindow* window, const QtSessionGeometry& geometry)
{
    Q_ASSERT(QThread::currentThread() == thread());
    for (auto& output : m_Outputs) {
        if (output.window == window) {
            output.geometry = geometry;
            if (output.item) output.item->setPresentation(geometry);
        }
    }
}

void QtVideoPresenter::initializeDevice()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_Started || m_Outputs.empty() || !m_State->primary) return;
    m_Started = true;
    const auto state = m_State;
    auto* window = m_Outputs.front().window.data();
    if (!window) { emit deviceReady(false, QStringLiteral("Qt presentation window was closed")); return; }
    // Device creation uses the actual Qt render context. It neither creates a
    // second window nor touches GUI objects from the render worker.
    window->scheduleRenderJob(QRunnable::create([state] {
        QString error;
        const auto node = currentRenderNode();
        auto device = QtSoftwareVideoDevice::createForCurrentContext(error);
        std::lock_guard<std::mutex> lock(state->mutex);
        auto* owner = state->owner;
        if (owner) QMetaObject::invokeMethod(owner, [owner, device, node, error] {
            owner->finishDevice(device, node, error);
        }, Qt::QueuedConnection);
    }), QQuickWindow::BeforeRenderingStage);
    window->update();
}

void QtVideoPresenter::finishDevice(QtSoftwareVideoDevice::Ptr device, QString node, QString error)
{
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        if (!m_State->owner) return;
    }
    m_SoftwareDevice = std::move(device);
    m_RenderNode = std::move(node);
    for (auto& output : m_Outputs) if (output.item) output.item->setSoftwareDevice(m_SoftwareDevice);
    emit deviceReady(m_SoftwareDevice || !m_RenderNode.isEmpty(), error);
}

std::function<QtDecodedVideoDelivery()> QtVideoPresenter::deliveryFactory() const
{
    const std::weak_ptr<State> weak = m_State;
    return [weak] {
        QtDecodedVideoDelivery delivery;
        const auto state = weak.lock();
        if (!state) return delivery;
        QtVideoMailbox::Producer producer;
        std::shared_ptr<QtEglVideoStatus> status;
        std::shared_ptr<QtEglVideoStatus> retired;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->owner || !state->primary) return delivery;
            producer = state->producer.restart();
            state->producer = producer;
            state->live = false;
            state->probeDelivered = false;
            state->probeFrame.reset();
            retired = std::move(state->primary);
            for (auto& output : state->outputs) output = std::make_shared<QtEglVideoStatus>();
            std::fill(state->required.begin(), state->required.end(), false);
            status = state->primary = state->outputs[state->primaryIndex];
        }
        wakeQualification(retired);
        delivery.submit = [producer, weak](QtVideoFrame::Ptr frame) {
            if (const auto state = weak.lock()) {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!producer.isCurrent()) return false;
                state->live = true;
                state->probeFrame.reset();
            } else return false;
            return producer.submit(std::move(frame));
        };
        delivery.discard = [producer, status] {
            producer.discard();
            wakeQualification(status);
        };
        delivery.fail = [producer, weak](QString reason) {
            const auto state = weak.lock();
            if (!state) return;
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->owner || !producer.isCurrent()) return;
            auto* owner = state->owner;
            QMetaObject::invokeMethod(owner, [owner, producer, reason] {
                if (!producer.isCurrent()) return;
                owner->stop();
                emit owner->presentationFailed(reason);
            }, Qt::QueuedConnection);
        };
        delivery.qualify = [producer, status, weak](QtVideoFrame::Ptr frame, QString& error) {
            if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
                error = QStringLiteral("Decoder qualification must not block Qt's GUI thread"); return false;
            }
            const auto state = weak.lock();
            if (!state || !frame || !producer.isCurrent()) return false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->probeFrame = frame;
            }
            if (!producer.submit(frame)) return false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                bool accepted = true, failed = false;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!producer.isCurrent() || !state->owner) break;
                    accepted = state->probeDelivered;
                    for (size_t index = 0; index < state->outputs.size(); ++index) {
                        if (!state->required[index]) continue;
                        const auto& output = state->outputs[index];
                        std::lock_guard<std::mutex> feedbackLock(output->presentationMutex);
                        failed |= output->error.load() != QtEglVideoStatus::None;
                        accepted &= output->swappedFrame.lock() == frame;
                    }
                }
                if (failed) break;
                if (accepted) return true;
                // Each output owns its feedback lock. Poll their bounded set
                // only during startup qualification, never during playback.
                std::unique_lock<std::mutex> lock(status->presentationMutex);
                status->presentationChanged.wait_for(lock, std::chrono::milliseconds(10));
            }
            error = QStringLiteral("Qt exact-format draw on a visible output was cancelled, failed or exceeded two seconds");
            return false;
        };
        return delivery;
    };
}

void QtVideoPresenter::deliver(QtVideoFrame::Ptr frame, const QtVideoMailbox::Producer& producer)
{
    bool probe;
    std::shared_ptr<QtEglVideoStatus> generation;
    std::vector<std::shared_ptr<QtEglVideoStatus>> outputs;
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        // The mailbox may have claimed an old frame immediately before the
        // decoder restarted it. Never relabel that frame as the new generation.
        if (!m_State->owner || !producer.isCurrent()) return;
        probe = m_State->probeFrame.lock() == frame;
        generation = m_State->primary;
        outputs = m_State->outputs;
        if (probe) {
            m_State->probeDelivered = true;
            for (size_t index = 0; index < m_Outputs.size(); ++index)
                m_State->required[index] = m_Outputs[index].primary ||
                    (m_Outputs[index].window && m_Outputs[index].window->isVisible());
        }
    }
    for (size_t index = 0; index < m_Outputs.size(); ++index) {
        auto& output = m_Outputs[index];
        if (!output.item || !output.window) continue;
        if (generation != m_DisplayedGeneration) {
            output.item->beginGeneration(outputs[index]);
        }
        auto geometry = output.geometry;
        if (probe) {
            // The fixed qualification frame is not a remote topology change.
            // Fill this output only for the probe; restore the session's exact
            // viewport/crop on the next live frame.
            geometry.streamSize = frame->size();
            geometry.sourcePixels = QRectF(QPointF(), frame->size());
            geometry.logicalSize = output.window->size();
            geometry.drawableSize = geometry.logicalSize * output.window->effectiveDevicePixelRatio();
            geometry.destinationPixels = QRectF(QPointF(), geometry.drawableSize);
        }
        output.item->setPresentation(geometry);
        output.item->setFrame(frame);
    }
    m_DisplayedGeneration = generation;
}

void QtVideoPresenter::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_ErrorTimer.stop();
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        m_State->owner = nullptr;
    }
    m_Mailbox.retire();
    wakeQualification(m_State->primary);
    for (auto& output : m_Outputs) if (output.item) output.item->setFrame({});
}
