/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

import Cocoa

class MacCommon: Common {
    @objc var layer: MetalLayer?

    var presentation: Presentation?
    var timer: PreciseTimer?
    var swapTime: UInt64 = 0
    let swapLock: NSCondition = NSCondition()

    // Opt-in observation only: never use force_render-adjusted visibility.
    private let visibilityDiagnostics = ProcessInfo.processInfo.environment["HDRPLAYER_MPV_VISIBILITY"] == "1"
    private var visibilityObservers: [NSObjectProtocol] = []
    private var visibilityTimer: Timer?
    private var visibilityRecords = 0
    private let visibilityRecordLimit = 4096

    @objc init(_ vo: UnsafeMutablePointer<vo>) {
        let log = LogHelper(mp_log_new(vo, vo.pointee.log, "mac"))
        let option = OptionHelper(vo, vo.pointee.global)
        super.init(option, log)
        eventsLock.withLock { self.vo = vo }
        input = InputHelper(vo.pointee.input_ctx, option)
        presentation = Presentation(common: self)
        timer = PreciseTimer(common: self)

        DispatchQueue.main.sync {
            layer = MetalLayer(common: self)
            initMisc(vo)
        }
    }

    @objc func config(_ vo: UnsafeMutablePointer<vo>) -> Bool {
        eventsLock.withLock { self.vo = vo }

        var configured = false
        DispatchQueue.main.sync {
            guard prepareEmbeddedHost() else { return }
            let previousActiveApp = getActiveApp()
            initApp()

            let (screen, wr, forcePosition) = getInitProperties(vo)
            guard let layer = self.layer else {
                log.error("Something went wrong, no MetalLayer was initialized")
                exit(1)
            }

            if view == nil {
                initView(vo, layer)
                initWindow(vo, previousActiveApp)
                initWindowState()
            }

            if forcePosition {
                window?.updateFrame(wr, screen)
            } else if option.vo.auto_window_resize {
                window?.updateSize(wr.size)
            }

            if embeddedHost == nil && option.vo.focus_on == 2 {
                NSApp.activate(ignoringOtherApps: true)
            }

            windowDidResize()
            updateICCProfile()
            configured = true
            startVisibilityDiagnostics()
        }

        return configured
    }

    @objc func uninit(_ vo: UnsafeMutablePointer<vo>) {
        window?.waitForAnimation()

        timer?.terminate()

        DispatchQueue.main.sync {
            stopVisibilityDiagnostics()
            window?.delegate = nil
            window?.close()

            uninitCommon()
        }
    }

    @objc func swapBuffer() {
        if option.mac.macos_render_timer > RENDER_TIMER_SYSTEM {
            swapLock.lock()
            while swapTime < 1 {
                swapLock.wait()
            }
            swapTime = 0
            swapLock.unlock()
        }
    }

    @objc func fillVsync(info: UnsafeMutablePointer<vo_vsync_info>) {
        if option.mac.macos_render_timer != RENDER_TIMER_PRESENTATION_FEEDBACK { return }

        let next = presentation?.next()
        info.pointee.vsync_duration = next?.duration ?? -1
        info.pointee.skipped_vsyncs = next?.skipped ?? -1
        info.pointee.last_queue_display_time = next?.time ?? -1
    }

    @objc var surfaceSize: CGSize {
        let measure = { self.view.map { $0.convertToBacking($0.bounds).size } ?? .zero }
        return Thread.isMainThread ? measure() : DispatchQueue.main.sync(execute: measure)
    }

    @objc func isVisible() -> Bool {
        return presentationWindow?.occlusionState.contains(.visible) ?? false ||
               option.vo.force_render ||
               needsInitialDraw
    }

    private func startVisibilityDiagnostics() {
        guard visibilityDiagnostics, visibilityTimer == nil, visibilityRecords < visibilityRecordLimit else { return }
        let center = NotificationCenter.default
        let windowEvents: [(Notification.Name, String)] = [
            (NSWindow.didChangeOcclusionStateNotification, "occlusion"),
            (NSWindow.didBecomeKeyNotification, "became-key"),
            (NSWindow.didResignKeyNotification, "resigned-key"),
            (NSWindow.didBecomeMainNotification, "became-main"),
            (NSWindow.didResignMainNotification, "resigned-main"),
            (NSWindow.didMiniaturizeNotification, "minimize"),
            (NSWindow.didDeminiaturizeNotification, "restore"),
            (NSWindow.didChangeScreenNotification, "screen"),
            (NSWindow.didChangeBackingPropertiesNotification, "backing"),
            (NSWindow.didResizeNotification, "resize"),
            (NSWindow.willCloseNotification, "close"),
        ]
        for (name, event) in windowEvents {
            visibilityObservers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] note in
                guard let self, let observed = note.object as? NSWindow,
                      observed === self.presentationWindow else { return }
                self.recordVisibility(event)
            })
        }
        for (name, event) in [(NSApplication.didBecomeActiveNotification, "app-active"),
                              (NSApplication.didResignActiveNotification, "app-inactive"),
                              (NSApplication.didHideNotification, "app-hidden"),
                              (NSApplication.didUnhideNotification, "app-unhidden")] {
            visibilityObservers.append(center.addObserver(forName: name, object: NSApp, queue: .main) { [weak self] _ in
                self?.recordVisibility(event)
            })
        }
        recordVisibility("startup")
        guard visibilityRecords < visibilityRecordLimit else { return }
        let timer = Timer(timeInterval: 0.25, repeats: true) { [weak self] _ in self?.recordVisibility("periodic") }
        visibilityTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func recordVisibility(_ event: String) {
        guard visibilityDiagnostics, visibilityRecords < visibilityRecordLimit else { return }
        visibilityRecords += 1
        let current = presentationWindow
        let content = view.map { $0.convertToBacking($0.bounds).size } ?? .zero
        let drawable = layer?.drawableSize ?? .zero
        let state: [String: Any] = [
            "schemaVersion": 1, "event": event, "hostSeconds": CACurrentMediaTime(),
            "hostTicks": mach_absolute_time(), "sequence": visibilityRecords,
            "windowNumber": current?.windowNumber ?? -1, "embedded": embeddedHost != nil,
            "isVisible": current?.isVisible ?? false,
            "occlusionVisible": current?.occlusionState.contains(.visible) ?? false,
            "isMiniaturized": current?.isMiniaturized ?? false,
            "onActiveSpace": current?.isOnActiveSpace ?? false,
            "windowFrame": current.map { NSStringFromRect($0.frame) } ?? "unavailable",
            "isKey": current?.isKeyWindow ?? false, "isMain": current?.isMainWindow ?? false,
            "appActive": NSApp.isActive, "appHidden": NSApp.isHidden,
            "backingScale": current?.backingScaleFactor ?? 0,
            "contentWidth": content.width, "contentHeight": content.height,
            "drawableWidth": drawable.width, "drawableHeight": drawable.height,
            "screenNumber": current?.screen?.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber ?? -1,
            "forceRenderRequested": option.vo.force_render,
            "recordLimit": visibilityRecordLimit, "periodicIntervalSeconds": 0.25,
        ]
        if let data = try? JSONSerialization.data(withJSONObject: state, options: [.sortedKeys]),
           let json = String(data: data, encoding: .utf8) {
            // The explicit diagnostic flag enables these records independently
            // of ordinary mpv log-level filtering. Each write is one full line.
            fputs("HDRPLAYER_MPV_WINDOW_STATE \(json)\n", stderr)
        }
        if visibilityRecords == visibilityRecordLimit {
            visibilityTimer?.invalidate(); visibilityTimer = nil
            visibilityObservers.forEach { NotificationCenter.default.removeObserver($0) }
            visibilityObservers.removeAll()
        }
    }

    private func stopVisibilityDiagnostics() {
        guard visibilityDiagnostics else { return }
        recordVisibility("shutdown")
        visibilityTimer?.invalidate(); visibilityTimer = nil
        visibilityObservers.forEach { NotificationCenter.default.removeObserver($0) }
        visibilityObservers.removeAll()
    }

    @objc func update(alpha: Bool) {
        layer?.wantsAlpha = alpha
        DispatchQueue.main.sync {
            window?.isOpaque = !alpha
            window?.backgroundColor = alpha ? NSColor.clear : nil
        }
    }

    override func displayLinkCallback(_ displayLink: CVDisplayLink,
                                      _ inNow: UnsafePointer<CVTimeStamp>,
                                      _ inOutputTime: UnsafePointer<CVTimeStamp>,
                                      _ flagsIn: CVOptionFlags,
                                      _ flagsOut: UnsafeMutablePointer<CVOptionFlags>) -> CVReturn {
        let signalSwap = {
            self.swapLock.lock()
            self.swapTime += 1
            self.swapLock.signal()
            self.swapLock.unlock()
        }

        if option.mac.macos_render_timer > RENDER_TIMER_SYSTEM {
            if let timer = self.timer, option.mac.macos_render_timer == RENDER_TIMER_PRECISE {
                timer.scheduleAt(time: inOutputTime.pointee.hostTime, closure: signalSwap)
                return kCVReturnSuccess
            }

            signalSwap()
            return kCVReturnSuccess
        }

        if option.mac.macos_render_timer == RENDER_TIMER_PRESENTATION_FEEDBACK {
            presentation?.add(time: inOutputTime.pointee)
        }

        return kCVReturnSuccess
    }

    override func startDisplayLink(_ vo: UnsafeMutablePointer<vo>) {
        super.startDisplayLink(vo)
        timer?.updatePolicy(periodSeconds: 1 / currentFps())
    }

    override func updateDisplaylink() {
        super.updateDisplaylink()
        timer?.updatePolicy(periodSeconds: 1 / currentFps())
    }

    override func updateICCProfile() {
        flagEvents(VO_EVENT_ICC_PROFILE_CHANGED)
    }

    override func windowDidResize() {
        flagEvents(VO_EVENT_RESIZE | VO_EVENT_EXPOSE)
    }

    override func windowDidChangeScreenProfile() {
        updateICCProfile()
    }

    override func windowDidChangeBackingProperties() {
        layer?.contentsScale = presentationWindow?.backingScaleFactor ?? 1
        windowDidResize()
    }

    override func windowDidChangeOcclusionState() {
        flagEvents(VO_EVENT_EXPOSE)
    }
}
