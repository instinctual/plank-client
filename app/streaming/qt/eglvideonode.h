#pragma once

#include "sessiongeometry.h"
#include "videoframe.h"
#include "softwarevideodevice.h"

#include <QQuickItem>
#include <atomic>
#include <mutex>
#include <condition_variable>

struct QtEglVideoStatus
{
    enum Error { None, WrongBackend, ImportFailed, PrecisionOrDrawFailed, FenceFailed };
    std::atomic<Error> error{None};
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> imports{0};
    std::atomic<uint64_t> synchronizations{0};
    std::atomic<uint64_t> swaps{0};
    bool visibleAtDelivery = false, exposedAtDelivery = false; // protected by presentationMutex
    std::atomic<bool> livePresented{false};
    std::mutex presentationMutex;
    std::condition_variable presentationChanged;
    QtVideoFrame::Ptr drawnFrame;
    std::weak_ptr<const QtVideoFrame> swappedFrame;
    std::chrono::steady_clock::time_point drawStarted;
};

// A root-sized underlay, behind QML controls. Qt owns its native surface and
// render thread. Call setFrame/setPresentation on the GUI thread only; the
// future session controller must coalesce decoder deliveries before this edge.
class QtEglVideoItem final : public QQuickItem
{
public:
    explicit QtEglVideoItem(QQuickItem* parent = nullptr);
    ~QtEglVideoItem() override;
    void setFrame(QtVideoFrame::Ptr frame);
    void setSoftwareDevice(QtSoftwareVideoDevice::Ptr device);
    void setPresentation(const QtSessionGeometry& geometry);
    void setPrimaryOutput(bool primary);
    void beginGeneration(std::shared_ptr<QtEglVideoStatus> status);
    std::shared_ptr<const QtEglVideoStatus> status() const { return m_Status; }
    std::shared_ptr<QtEglVideoStatus> presentationFeedback() const { return m_Status; }

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    QtVideoFrame::Ptr m_Frame;
    QtSoftwareVideoDevice::Ptr m_SoftwareDevice;
    QtSessionGeometry m_Geometry;
    std::shared_ptr<QtEglVideoStatus> m_Status = std::make_shared<QtEglVideoStatus>();
    QMetaObject::Connection m_SwapConnection;
    QMetaObject::Connection m_WindowConnection;
    bool m_PrimaryOutput = false;
    void connectPresentationFeedback();
};
