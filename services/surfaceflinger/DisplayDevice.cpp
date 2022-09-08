/*
 * Copyright (C) 2007 The Android Open Source Project
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

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

// #define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <common/trace.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/DisplayColorProfileCreationArgs.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/DisplaySurface.h>
#include <compositionengine/ProjectionSpace.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/RenderSurfaceCreationArgs.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <configstore/Utils.h>
#include <ftl/concat.h>
#include <log/log.h>
#include <system/window.h>

#include <fcntl.h>
#include <termios.h>
#include <linux/kd.h>
#include <linux/vt.h>

#include "DisplayDevice.h"
#include "FrontEnd/DisplayInfo.h"
#include "HdrSdrRatioOverlay.h"
#include "Layer.h"
#include "RefreshRateOverlay.h"
#include "SurfaceFlinger.h"

// Duplicate enum values here, this avoids making SurfaceFlinger module
// depend on drm_gralloc, which is obsolete and will eventually go away.

enum {
    GRALLOC_MODULE_PERFORM_ENTER_VT                  = 0x80000005,
    GRALLOC_MODULE_PERFORM_LEAVE_VT                  = 0x80000006,
};

namespace android {

namespace hal = hardware::graphics::composer::hal;

#ifdef CONSOLE_MANAGER
class ConsoleManagerThread : public Thread {
public:
            ConsoleManagerThread(const sp<SurfaceFlinger>&, const wp<IBinder>&);
    virtual ~ConsoleManagerThread();

    status_t releaseScreen() const;

private:
    sp<SurfaceFlinger> mFlinger;
    wp<IBinder> mDisplayToken;
    int consoleFd;
    long prev_vt_num;
    vt_mode vm;
    virtual void onFirstRef();
    virtual status_t readyToRun();
    virtual void requestExit();
    virtual bool threadLoop();
    static void sigHandler(int sig);
    static pid_t sSignalCatcherPid;
};

ConsoleManagerThread::ConsoleManagerThread(const sp<SurfaceFlinger>& flinger, const wp<IBinder>& token)
    : Thread(false), mFlinger(flinger), mDisplayToken(token), consoleFd(-1)
{
    sSignalCatcherPid = 0;

    // create a new console
    char const * const ttydev = "/dev/tty0";
    int fd = open(ttydev, O_RDWR | O_SYNC);
    if (fd < 0) {
        ALOGE("Can't open %s, errno=%d (%s)", ttydev, errno, strerror(errno));
        consoleFd = -errno;
        return;
    }
    ALOGD("Open /dev/tty0 OK");

    // to make sure that we are in text mode
    int res = ioctl(fd, KDSETMODE, (void*) KD_TEXT);
    if (res < 0) {
        ALOGE("ioctl(%d, KDSETMODE, ...) failed, res %d (%s)",
                fd, res, strerror(errno));
    }

    // get the current console
    struct vt_stat vs;
    res = ioctl(fd, VT_GETSTATE, &vs);
    if (res < 0) {
        ALOGE("ioctl(%d, VT_GETSTATE, ...) failed, res %d (%s)",
                fd, res, strerror(errno));
        consoleFd = -errno;
        return;
    }

    // switch to console 7 (which is what X normaly uses)
    do {
        res = ioctl(fd, VT_ACTIVATE, ANDROID_VT);
    } while(res < 0 && errno == EINTR);
    if (res < 0) {
        ALOGE("ioctl(%d, VT_ACTIVATE, ...) failed, %d (%s) for vt %d",
                fd, errno, strerror(errno), ANDROID_VT);
        consoleFd = -errno;
        return;
    }

    do {
        res = ioctl(fd, VT_WAITACTIVE, ANDROID_VT);
    } while (res < 0 && errno == EINTR);
    if (res < 0) {
        ALOGE("ioctl(%d, VT_WAITACTIVE, ...) failed, %d %d %s for vt %d",
                fd, res, errno, strerror(errno), ANDROID_VT);
        consoleFd = -errno;
        return;
    }

    // open the new console
    close(fd);
    fd = open(ttydev, O_RDWR | O_SYNC);
    if (fd < 0) {
        ALOGE("Can't open new console %s", ttydev);
        consoleFd = -errno;
        return;
    }

    /* disable console line buffer, echo, ... */
    struct termios ttyarg;
    ioctl(fd, TCGETS , &ttyarg);
    ttyarg.c_iflag = 0;
    ttyarg.c_lflag = 0;
    ioctl(fd, TCSETS , &ttyarg);

    // set up signals so we're notified when the console changes
    // we can't use SIGUSR1 because it's used by the java-vm
    vm.mode = VT_PROCESS;
    vm.waitv = 0;
    vm.relsig = SIGUSR2;
    vm.acqsig = SIGUNUSED;
    vm.frsig = 0;

    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_handler = sigHandler;
    act.sa_flags = 0;
    sigaction(vm.relsig, &act, NULL);

    sigemptyset(&act.sa_mask);
    act.sa_handler = sigHandler;
    act.sa_flags = 0;
    sigaction(vm.acqsig, &act, NULL);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, vm.relsig);
    sigaddset(&mask, vm.acqsig);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    // switch to graphic mode
    res = ioctl(fd, KDSETMODE, (void*)KD_GRAPHICS);
    ALOGW_IF(res < 0,
            "ioctl(%d, KDSETMODE, KD_GRAPHICS) failed, res %d", fd, res);

    prev_vt_num = vs.v_active;
    consoleFd = fd;
}

ConsoleManagerThread::~ConsoleManagerThread()
{
    if (consoleFd >= 0) {
        int fd = consoleFd;
        int res;
        ioctl(fd, KDSETMODE, (void*)KD_TEXT);
        do {
            res = ioctl(fd, VT_ACTIVATE, prev_vt_num);
        } while(res < 0 && errno == EINTR);
        do {
            res = ioctl(fd, VT_WAITACTIVE, prev_vt_num);
        } while(res < 0 && errno == EINTR);
        close(fd);
        char const * const ttydev = "/dev/tty0";
        fd = open(ttydev, O_RDWR | O_SYNC);
        ioctl(fd, VT_DISALLOCATE, 0);
        close(fd);
    }
}

status_t ConsoleManagerThread::releaseScreen() const
{
    int err = ioctl(consoleFd, VT_RELDISP, (void*)1);
    ALOGE_IF(err < 0, "ioctl(%d, VT_RELDISP, 1) failed %d (%s)",
        consoleFd, errno, strerror(errno));
    return (err < 0) ? (-errno) : status_t(NO_ERROR);
}

void ConsoleManagerThread::onFirstRef()
{
    run("ConsoleManagerThread", PRIORITY_URGENT_DISPLAY);
}

status_t ConsoleManagerThread::readyToRun()
{
    if (consoleFd >= 0) {
        sSignalCatcherPid = gettid();

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, vm.relsig);
        sigaddset(&mask, vm.acqsig);
        sigprocmask(SIG_BLOCK, &mask, NULL);

        int res = ioctl(consoleFd, VT_SETMODE, &vm);
        if (res < 0) {
            ALOGE("ioctl(%d, VT_SETMODE, ...) failed, %d (%s)",
                    consoleFd, errno, strerror(errno));
        }
        return NO_ERROR;
    }
    return consoleFd;
}

void ConsoleManagerThread::requestExit()
{
    Thread::requestExit();
    if (sSignalCatcherPid != 0) {
        // wake the thread up
        kill(sSignalCatcherPid, SIGINT);
        // wait for it...
    }
}

bool ConsoleManagerThread::threadLoop()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, vm.relsig);
    sigaddset(&mask, vm.acqsig);

    int sig = 0;
    sigwait(&mask, &sig);

    hw_module_t const* mod;
    gralloc_module_t const* gr = NULL;
    status_t err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &mod);
    if (!err) {
        gr = reinterpret_cast<gralloc_module_t const*>(mod);
        if (!gr->perform)
            gr = NULL;
    }

    if (sig == vm.relsig) {
        if (gr)
            gr->perform(gr, GRALLOC_MODULE_PERFORM_LEAVE_VT);
        mFlinger->screenReleased(mDisplayToken.promote());
    } else if (sig == vm.acqsig) {
        mFlinger->screenAcquired(mDisplayToken.promote());
        if (gr)
            gr->perform(gr, GRALLOC_MODULE_PERFORM_ENTER_VT);
    }

    return true;
}

void ConsoleManagerThread::sigHandler(int sig)
{
    // resend the signal to our signal catcher thread
    ALOGW("received signal %d in thread %d, resending to %d",
            sig, gettid(), sSignalCatcherPid);

    // we absolutely need the delays below because without them
    // our main thread never gets a chance to handle the signal.
    usleep(10000);
    kill(sSignalCatcherPid, sig);
    usleep(10000);
}

pid_t ConsoleManagerThread::sSignalCatcherPid;
#endif

DisplayDeviceCreationArgs::DisplayDeviceCreationArgs(
        const sp<SurfaceFlinger>& flinger, HWComposer& hwComposer, const wp<IBinder>& displayToken,
        std::shared_ptr<compositionengine::Display> compositionDisplay)
      : flinger(flinger),
        hwComposer(hwComposer),
        displayToken(displayToken),
        compositionDisplay(compositionDisplay) {}

DisplayDevice::DisplayDevice(DisplayDeviceCreationArgs& args)
      : mFlinger(args.flinger),
        mHwComposer(args.hwComposer),
        mDisplayToken(args.displayToken),
        mSequenceId(args.sequenceId),
        mCompositionDisplay{args.compositionDisplay},
        mPhysicalOrientation(args.physicalOrientation),
        mPowerMode(ftl::Concat("PowerMode ", getId().value).c_str(), args.initialPowerMode),
#ifdef CONSOLE_MANAGER
        mConsoleManagerThread(0),
#endif
        mIsPrimary(args.isPrimary),
        mRequestedRefreshRate(args.requestedRefreshRate),
        mRefreshRateSelector(std::move(args.refreshRateSelector)) {
    mCompositionDisplay->editState().isSecure = args.isSecure;
    mCompositionDisplay->editState().isProtected = args.isProtected;
    mCompositionDisplay->createRenderSurface(
            compositionengine::RenderSurfaceCreationArgsBuilder()
                    .setDisplayWidth(ANativeWindow_getWidth(args.nativeWindow.get()))
                    .setDisplayHeight(ANativeWindow_getHeight(args.nativeWindow.get()))
                    .setNativeWindow(std::move(args.nativeWindow))
                    .setDisplaySurface(std::move(args.displaySurface))
                    .setMaxTextureCacheSize(
                            static_cast<size_t>(SurfaceFlinger::maxFrameBufferAcquiredBuffers))
                    .build());

    if (!mFlinger->mDisableClientCompositionCache &&
        SurfaceFlinger::maxFrameBufferAcquiredBuffers > 0) {
        mCompositionDisplay->createClientCompositionCache(
                static_cast<uint32_t>(SurfaceFlinger::maxFrameBufferAcquiredBuffers));
    }

    mCompositionDisplay->setPredictCompositionStrategy(mFlinger->mPredictCompositionStrategy);
    mCompositionDisplay->setTreat170mAsSrgb(mFlinger->mTreat170mAsSrgb);
    mCompositionDisplay->createDisplayColorProfile(
            compositionengine::DisplayColorProfileCreationArgsBuilder()
                    .setHasWideColorGamut(args.hasWideColorGamut)
                    .setHdrCapabilities(std::move(args.hdrCapabilities))
                    .setSupportedPerFrameMetadata(args.supportedPerFrameMetadata)
                    .setHwcColorModes(std::move(args.hwcColorModes))
                    .Build());

    if (!mCompositionDisplay->isValid()) {
        ALOGE("Composition Display did not validate!");
    }

    mCompositionDisplay->getRenderSurface()->initialize();

    setPowerMode(args.initialPowerMode);

    // initialize the display orientation transform.
    setProjection(ui::ROTATION_0, Rect::INVALID_RECT, Rect::INVALID_RECT);
}

#ifdef CONSOLE_MANAGER
DisplayDevice::~DisplayDevice() {
    if (mConsoleManagerThread != 0) {
        mConsoleManagerThread->requestExitAndWait();
        ALOGD("ConsoleManagerThread: destroy primary DisplayDevice");
    }
}
#else
DisplayDevice::~DisplayDevice() = default;
#endif

void DisplayDevice::disconnect() {
    mCompositionDisplay->disconnect();
}

int DisplayDevice::getWidth() const {
    return mCompositionDisplay->getState().displaySpace.getBounds().width;
}

int DisplayDevice::getHeight() const {
    return mCompositionDisplay->getState().displaySpace.getBounds().height;
}

void DisplayDevice::setDisplayName(const std::string& displayName) {
    if (!displayName.empty()) {
        // never override the name with an empty name
        mDisplayName = displayName;
        mCompositionDisplay->setName(displayName);
    }
}

auto DisplayDevice::getFrontEndInfo() const -> frontend::DisplayInfo {
    gui::DisplayInfo info;
    info.displayId = ui::LogicalDisplayId{static_cast<int32_t>(getLayerStack().id)};

    // The physical orientation is set when the orientation of the display panel is
    // different than the default orientation of the device. Other services like
    // InputFlinger do not know about this, so we do not need to expose the physical
    // orientation of the panel outside of SurfaceFlinger.
    const ui::Rotation inversePhysicalOrientation = ui::ROTATION_0 - mPhysicalOrientation;
    auto width = getWidth();
    auto height = getHeight();
    if (inversePhysicalOrientation == ui::ROTATION_90 ||
        inversePhysicalOrientation == ui::ROTATION_270) {
        std::swap(width, height);
    }
    const ui::Transform undoPhysicalOrientation(ui::Transform::toRotationFlags(
                                                        inversePhysicalOrientation),
                                                width, height);
    const auto& displayTransform = undoPhysicalOrientation * getTransform();
    // Send the inverse display transform to input so it can convert display coordinates to
    // logical display.
    info.transform = displayTransform.inverse();

    info.logicalWidth = getLayerStackSpaceRect().width();
    info.logicalHeight = getLayerStackSpaceRect().height();

    return {.info = info,
            .transform = displayTransform,
            .receivesInput = receivesInput(),
            .isSecure = isSecure(),
            .isPrimary = isPrimary(),
            .isVirtual = isVirtual(),
            .rotationFlags = ui::Transform::toRotationFlags(mOrientation),
            .transformHint = getTransformHint()};
}

void DisplayDevice::setPowerMode(hal::PowerMode mode) {
    if (!isVirtual() && (mode == hal::PowerMode::OFF || mode == hal::PowerMode::ON)) {
        if (mStagedBrightness && mBrightness != mStagedBrightness) {
            getCompositionDisplay()->setNextBrightness(*mStagedBrightness);
            mBrightness = *mStagedBrightness;
        }
        mStagedBrightness = std::nullopt;
        getCompositionDisplay()->applyDisplayBrightness(true);
    }

    mPowerMode = mode;

    getCompositionDisplay()->setCompositionEnabled(isPoweredOn());

#ifdef CONSOLE_MANAGER
    if (mode != hal::PowerMode::ON && mConsoleManagerThread != 0) {
        mConsoleManagerThread->releaseScreen();
    }
#endif
}

void DisplayDevice::tracePowerMode() {
    // Assign the same value for tracing.
    mPowerMode = mPowerMode.get();
}

void DisplayDevice::enableLayerCaching(bool enable) {
    getCompositionDisplay()->setLayerCachingEnabled(enable);
}

hal::PowerMode DisplayDevice::getPowerMode() const {
    return mPowerMode;
}

bool DisplayDevice::isPoweredOn() const {
    return mPowerMode != hal::PowerMode::OFF;
}

bool DisplayDevice::isRefreshable() const {
    return mPowerMode == hal::PowerMode::DOZE || mPowerMode == hal::PowerMode::ON;
}

ui::Dataspace DisplayDevice::getCompositionDataSpace() const {
    return mCompositionDisplay->getState().dataspace;
}

void DisplayDevice::setLayerFilter(LayerFilter filter) {
    mCompositionDisplay->setLayerFilter(filter);
    if (mRefreshRateSelector) {
        mRefreshRateSelector->setLayerFilter(filter);
    }
    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->setLayerStack(filter.layerStack);
    }
    if (mHdrSdrRatioOverlay) {
        mHdrSdrRatioOverlay->setLayerStack(filter.layerStack);
    }
}

void DisplayDevice::setFlags(uint32_t flags) {
    mFlags = flags;
}

void DisplayDevice::setDisplaySize(ui::Size size) {
    mCompositionDisplay->setDisplaySize(size);
    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->setViewport(size);
    }
    if (mHdrSdrRatioOverlay) {
        mHdrSdrRatioOverlay->setViewport(size);
    }
}

void DisplayDevice::setProjection(ui::Rotation orientation, Rect layerStackSpaceRect,
                                  Rect orientedDisplaySpaceRect) {
    mIsOrientationChanged = mOrientation != orientation;
    mOrientation = orientation;

#ifdef CONSOLE_MANAGER
    if (isPrimary()) {
        mConsoleManagerThread = sp<ConsoleManagerThread>::make(mFlinger, mDisplayToken);
    }
#endif

    // We need to take care of display rotation for globalTransform for case if the panel is not
    // installed aligned with device orientation.
    const auto transformOrientation = orientation + mPhysicalOrientation;

    const auto& state = getCompositionDisplay()->getState();

    // If the layer stack and destination frames have never been set, then configure them to be the
    // same as the physical device, taking into account the total transform.
    if (!orientedDisplaySpaceRect.isValid()) {
        ui::Size bounds = state.displaySpace.getBounds();
        bounds.rotate(transformOrientation);
        orientedDisplaySpaceRect = Rect(bounds);
    }
    if (layerStackSpaceRect.isEmpty()) {
        ui::Size bounds = state.framebufferSpace.getBounds();
        bounds.rotate(transformOrientation);
        layerStackSpaceRect = Rect(bounds);
    }
    getCompositionDisplay()->setProjection(transformOrientation, layerStackSpaceRect,
                                           orientedDisplaySpaceRect);
}

void DisplayDevice::stageBrightness(float brightness) {
    mStagedBrightness = brightness;
}

void DisplayDevice::persistBrightness(bool needsComposite) {
    if (mStagedBrightness && mBrightness != mStagedBrightness) {
        if (needsComposite) {
            getCompositionDisplay()->setNextBrightness(*mStagedBrightness);
        }
        mBrightness = *mStagedBrightness;
    }
    mStagedBrightness = std::nullopt;
}

std::optional<float> DisplayDevice::getStagedBrightness() const {
    return mStagedBrightness;
}

void DisplayDevice::dump(utils::Dumper& dumper) const {
    using namespace std::string_view_literals;

    dumper.dump("name"sv, '"' + mDisplayName + '"');
    dumper.dump("powerMode"sv, mPowerMode);
    dumper.dump("optimizationPolicy"sv, mOptimizationPolicy);

    if (mRefreshRateSelector) {
        mRefreshRateSelector->dump(dumper);
    }
}

bool DisplayDevice::hasRenderIntent(ui::RenderIntent intent) const {
    return mCompositionDisplay->getDisplayColorProfile()->hasRenderIntent(intent);
}

DisplayId DisplayDevice::getId() const {
    return mCompositionDisplay->getId();
}

bool DisplayDevice::isVirtual() const {
    return mCompositionDisplay->isVirtual();
}

bool DisplayDevice::isSecure() const {
    return mCompositionDisplay->isSecure();
}

void DisplayDevice::setSecure(bool secure) {
    mCompositionDisplay->setSecure(secure);
}

gui::ISurfaceComposer::OptimizationPolicy DisplayDevice::getOptimizationPolicy() const {
    return mOptimizationPolicy;
}

void DisplayDevice::setOptimizationPolicy(
        gui::ISurfaceComposer::OptimizationPolicy optimizationPolicy) {
    mOptimizationPolicy = optimizationPolicy;
}

const Rect DisplayDevice::getBounds() const {
    return mCompositionDisplay->getState().displaySpace.getBoundsAsRect();
}

const Region& DisplayDevice::getUndefinedRegion() const {
    return mCompositionDisplay->getState().undefinedRegion;
}

ui::LayerStack DisplayDevice::getLayerStack() const {
    return mCompositionDisplay->getState().layerFilter.layerStack;
}

ui::Transform::RotationFlags DisplayDevice::getTransformHint() const {
    return mCompositionDisplay->getTransformHint();
}

const ui::Transform& DisplayDevice::getTransform() const {
    return mCompositionDisplay->getState().transform;
}

const Rect& DisplayDevice::getLayerStackSpaceRect() const {
    return mCompositionDisplay->getState().layerStackSpace.getContent();
}

const Rect& DisplayDevice::getOrientedDisplaySpaceRect() const {
    return mCompositionDisplay->getState().orientedDisplaySpace.getContent();
}

bool DisplayDevice::hasWideColorGamut() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasWideColorGamut();
}

bool DisplayDevice::hasHDR10PlusSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHDR10PlusSupport();
}

bool DisplayDevice::hasHDR10Support() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHDR10Support();
}

bool DisplayDevice::hasHLGSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHLGSupport();
}

bool DisplayDevice::hasDolbyVisionSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasDolbyVisionSupport();
}

int DisplayDevice::getSupportedPerFrameMetadata() const {
    return mCompositionDisplay->getDisplayColorProfile()->getSupportedPerFrameMetadata();
}

void DisplayDevice::overrideHdrTypes(const std::vector<ui::Hdr>& hdrTypes) {
    mOverrideHdrTypes = hdrTypes;
}

HdrCapabilities DisplayDevice::getHdrCapabilities() const {
    const HdrCapabilities& capabilities =
            mCompositionDisplay->getDisplayColorProfile()->getHdrCapabilities();
    std::vector<ui::Hdr> hdrTypes = capabilities.getSupportedHdrTypes();
    if (!mOverrideHdrTypes.empty()) {
        hdrTypes = mOverrideHdrTypes;
    }
    return HdrCapabilities(hdrTypes, capabilities.getDesiredMaxLuminance(),
                           capabilities.getDesiredMaxAverageLuminance(),
                           capabilities.getDesiredMinLuminance());
}

void DisplayDevice::enableHdrSdrRatioOverlay(bool enable) {
    if (!enable) {
        ALOGD("Disabling HdrSdrRatioOverlay");
        mHdrSdrRatioOverlay.reset();
        return;
    }

    ALOGD("Enabling HdrSdrRatioOverlay");
    mHdrSdrRatioOverlay = HdrSdrRatioOverlay::create();
    if (mHdrSdrRatioOverlay) {
        mHdrSdrRatioOverlay->setLayerStack(getLayerStack());
        mHdrSdrRatioOverlay->setViewport(getSize());
        updateHdrSdrRatioOverlayRatio(mHdrSdrRatio);
    }
}

void DisplayDevice::updateHdrSdrRatioOverlayRatio(float currentHdrSdrRatio) {
    SFTRACE_CALL();
    mHdrSdrRatio = currentHdrSdrRatio;
    if (mHdrSdrRatioOverlay) {
        mHdrSdrRatioOverlay->changeHdrSdrRatio(currentHdrSdrRatio);
    }
}

void DisplayDevice::enableRefreshRateOverlay(bool enable, bool setByHwc, Fps refreshRate,
                                             Fps renderFps, bool showSpinner, bool showRenderRate,
                                             bool showInMiddle) {
    if (!enable) {
        ALOGD("Disabling RefreshRateOverlay");
        mRefreshRateOverlay.reset();
        return;
    }
    ALOGD("Enabling RefreshRateOverlay");

    ftl::Flags<RefreshRateOverlay::Features> features;
    if (showSpinner) {
        features |= RefreshRateOverlay::Features::Spinner;
    }

    if (showRenderRate) {
        features |= RefreshRateOverlay::Features::RenderRate;
    }

    if (showInMiddle) {
        features |= RefreshRateOverlay::Features::ShowInMiddle;
    }

    if (setByHwc) {
        features |= RefreshRateOverlay::Features::SetByHwc;
    }

    const auto fpsRange = mRefreshRateSelector->getSupportedRefreshRateRange();
    mRefreshRateOverlay = RefreshRateOverlay::create(fpsRange, features);
    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->setLayerStack(getLayerStack());
        mRefreshRateOverlay->setViewport(getSize());
        updateRefreshRateOverlayRate(refreshRate, renderFps, setByHwc);
    }
}

void DisplayDevice::updateRefreshRateOverlayRate(Fps refreshRate, Fps renderFps, bool setByHwc) {
    SFTRACE_CALL();
    if (mRefreshRateOverlay) {
        if (!mRefreshRateOverlay->isSetByHwc() || setByHwc) {
            if (mRefreshRateSelector->isVrrDisplay() && !mRefreshRateOverlay->isSetByHwc()) {
                refreshRate = renderFps;
            }
            mRefreshRateOverlay->changeRefreshRate(refreshRate, renderFps);
        } else {
            mRefreshRateOverlay->changeRenderRate(renderFps);
        }
    }
}

bool DisplayDevice::onKernelTimerChanged(std::optional<DisplayModeId> desiredModeId,
                                         bool timerExpired) {
    if (mRefreshRateSelector && mRefreshRateOverlay) {
        const auto newMode =
                mRefreshRateSelector->onKernelTimerChanged(desiredModeId, timerExpired);
        if (newMode) {
            updateRefreshRateOverlayRate(newMode->modePtr->getVsyncRate(), newMode->fps);
            return true;
        }
    }

    return false;
}

void DisplayDevice::onVrrIdle(bool idle) {
    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->onVrrIdle(idle);
    }
}

void DisplayDevice::animateRefreshRateOverlay() {
    if (mRefreshRateOverlay) {
        mRefreshRateOverlay->animate();
    }
}

void DisplayDevice::animateHdrSdrRatioOverlay() {
    if (!mHdrSdrRatioOverlay) {
        return;
    }

    // hdr sdr ratio is designed to be on the top right of the screen,
    // therefore, we need to re-calculate the display's width and height
    if (mIsOrientationChanged) {
        auto width = getWidth();
        auto height = getHeight();
        if (mOrientation == ui::ROTATION_90 || mOrientation == ui::ROTATION_270) {
            std::swap(width, height);
        }
        mHdrSdrRatioOverlay->setViewport({width, height});
    }
    mHdrSdrRatioOverlay->animate();
}

void DisplayDevice::adjustRefreshRate(Fps pacesetterDisplayRefreshRate) {
    using fps_approx_ops::operator<=;
    if (mRequestedRefreshRate <= 0_Hz) {
        return;
    }

    using fps_approx_ops::operator>;
    if (mRequestedRefreshRate > pacesetterDisplayRefreshRate) {
        mAdjustedRefreshRate = pacesetterDisplayRefreshRate;
        return;
    }

    unsigned divisor = static_cast<unsigned>(
            std::floor(pacesetterDisplayRefreshRate.getValue() / mRequestedRefreshRate.getValue()));
    if (divisor == 0) {
        mAdjustedRefreshRate = 0_Hz;
        return;
    }

    mAdjustedRefreshRate = pacesetterDisplayRefreshRate / divisor;
}

std::atomic<int32_t> DisplayDeviceState::sNextSequenceId(1);

}  // namespace android

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"
