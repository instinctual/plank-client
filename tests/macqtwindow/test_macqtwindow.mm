// Runs only on a disposable hosted builder. No network, audio, input injection
// or permission requests. Exercises the actual Cocoa window/render worker.
#include "metalsessionshell.h"
#include "metalvideonode.h"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <QtTest>
#include <QElapsedTimer>
#include <atomic>
#include <functional>
#include <memory>

namespace {
CAMetalLayer* findMetalLayer(CALayer* layer)
{
    if ([layer isKindOfClass:[CAMetalLayer class]]) return (CAMetalLayer*)layer;
    for (CALayer* child in layer.sublayers)
        if (auto* result = findMetalLayer(child)) return result;
    return nil;
}
bool until(const std::function<bool()>& ready, int timeout = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QTest::qWait(5);
    }
    return ready();
}
struct Window {
    QtMetalSessionShell shell;
    ~Window()
    {
        shell.hide();
        shell.stopRendering();
        // Never destroy a worker while it still owns the scene graph.
        if (!until([this] { return shell.renderingStopped(); })) qFatal("Qt Metal shutdown did not finish");
    }
};
struct FullscreenCompletion {
    bool entered = false, exited = false;
    id enterObserver = nil, exitObserver = nil;
    explicit FullscreenCompletion(NSWindow* window)
    {
        auto* state = this;
        enterObserver = [[NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidEnterFullScreenNotification
            object:window queue:nil usingBlock:^(NSNotification*) { state->entered = true; }] retain];
        exitObserver = [[NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidExitFullScreenNotification
            object:window queue:nil usingBlock:^(NSNotification*) { state->exited = true; }] retain];
    }
    ~FullscreenCompletion()
    {
        [NSNotificationCenter.defaultCenter removeObserver:enterObserver];
        [NSNotificationCenter.defaultCenter removeObserver:exitObserver];
        [enterObserver release]; [exitObserver release];
    }
};
QtVideoFrame::Ptr sample()
{
    AVFrame* raw = av_frame_alloc();
    if (!raw) return {};
    raw->format = AV_PIX_FMT_GBRP10LE;
    raw->width = 320; raw->height = 180;
    raw->colorspace = AVCOL_SPC_RGB; raw->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(raw,64) < 0) { av_frame_free(&raw); return {}; }
    for (int p=0;p<3;++p) for (int y=0;y<raw->height;++y) {
        auto* row = reinterpret_cast<uint16_t*>(raw->data[p]+y*raw->linesize[p]);
        for (int x=0;x<raw->width;++x) row[x] = (x*3+p*97)%1024;
    }
    QString error;
    auto result = QtVideoFrame::software(raw,AV_PIX_FMT_GBRP10LE,error);
    av_frame_free(&raw);
    return result;
}
}

class MacQtWindowTest final : public QObject
{
    Q_OBJECT
private slots:
    void nativeWindowResizeHideAndShutdown()
    {
        QVERIFY(QGuiApplication::platformName() == QStringLiteral("cocoa"));
        for (int cycle=0;cycle<3;++cycle) {
            std::atomic_uint swaps{0};
            Window window;
            auto& shell = window.shell;
            QSignalSpy errors(&shell,&QtMetalSessionShell::renderingFailed);
            QObject::connect(&shell,&QtMetalSessionShell::renderingFailed,&shell,[](const QString& error) { qWarning().noquote() << error; });
            auto& scene = shell.surface();
            scene.setLiveSession(true);
            scene.setPinned(true);
            QObject::connect(&scene,&QQuickWindow::frameSwapped,&scene,[&] { ++swaps; },Qt::DirectConnection);
            shell.prepareForSession({3840,2160},false);
            QVERIFY2(errors.isEmpty(), "Cocoa Metal preparation failed");
            shell.showSession(false);
            QVERIFY(until([&] { return swaps.load()>0 || !errors.isEmpty(); }));
            QVERIFY(errors.isEmpty());
            auto* view = (NSView*)shell.winId();
            auto* layer = findMetalLayer(view.layer);
            QVERIFY(layer);
            QCOMPARE(layer.pixelFormat,MTLPixelFormatRGB10A2Unorm);
            QVERIFY(view.window.styleMask & NSWindowStyleMaskTitled);
            QVERIFY(view.window.styleMask & NSWindowStyleMaskResizable);

            auto* item = new QtMetalVideoItem(scene.contentItem());
            item->setPrimaryOutput(true);
            const auto picture = sample();
            QVERIFY(picture);
            for (QSize size : {QSize(800,450),QSize(1040,585),QSize(800,450)}) {
                shell.setProperty("plankMetalTransfer",QStringLiteral("bt709"));
                shell.resize(size);
                QVERIFY(until([&] { return scene.size()==size; }));
                const QSize pixels = size*shell.devicePixelRatio();
                const QtSessionGeometry geometry{picture->size(),QRectF(QPointF(),picture->size()),
                    QRectF(QPointF(),pixels),pixels,size};
                scene.setPresentation(geometry);
                item->setPresentation(geometry);
                item->setFrame(picture);
                const unsigned previous = swaps.load();
                scene.update();
                // A resize/toolbar swap can finish before the newly delivered
                // video reaches the scene graph. Require video feedback too;
                // an unrelated pending swap is not presentation of this item.
                QVERIFY(until([&] {
                    return (swaps.load()>previous && item->status()->livePresented.load()) ||
                        !errors.isEmpty() || item->status()->error.load() != QtVideoStatus::None;
                }));
                QVERIFY(errors.isEmpty());
                QCOMPARE(item->status()->error.load(),QtVideoStatus::None);
                QVERIFY(item->status()->livePresented);
                auto expectedSpace = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
                const bool layerMatches = layer.colorspace && CFEqual(layer.colorspace,expectedSpace);
                const bool windowMatches = view.window.colorSpace.CGColorSpace && CFEqual(view.window.colorSpace.CGColorSpace,expectedSpace);
                CGColorSpaceRelease(expectedSpace);
                QVERIFY(layerMatches);
                QVERIFY(windowMatches);
            }
            shell.hide();
            QTest::qWait(30);
            const unsigned previous = swaps.load();
            shell.showSession(false);
            scene.update();
            QVERIFY(until([&] { return swaps.load()>previous || !errors.isEmpty(); }));
            QVERIFY(errors.isEmpty());
            if (cycle == 0) {
                const QSize normalSize = shell.size();
                QCOMPARE(QSize(qRound(view.bounds.size.width), qRound(view.bounds.size.height)), normalSize);
                FullscreenCompletion completed(view.window);
                shell.showSession(true);
                // Qt's requested state/style mask precedes Cocoa's completed
                // transition. Observe the native completion, not a timer.
                QVERIFY(until([&] { return completed.entered && (view.window.styleMask & NSWindowStyleMaskFullScreen) &&
                    scene.size() == shell.size(); }));
                shell.showSession(false);
                const bool restored = until([&] { return completed.exited && !(view.window.styleMask & NSWindowStyleMaskFullScreen) &&
                    shell.size() == normalSize && scene.size() == normalSize; });
                if (!restored) qWarning() << "Fullscreen restore:" << completed.entered << completed.exited
                    << normalSize << shell.size() << scene.size() << shell.windowState();
                QVERIFY(restored);
                QVERIFY(errors.isEmpty());
            }
            shell.stopRendering();
            QVERIFY(until([&] { return shell.renderingStopped(); }));
            delete item;
        }
    }
};

int main(int argc,char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
    QGuiApplication app(argc,argv);
    app.setQuitOnLastWindowClosed(false);
    MacQtWindowTest test;
    return QTest::qExec(&test,argc,argv);
}
#include "test_macqtwindow.moc"
