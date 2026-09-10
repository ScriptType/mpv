// Native optional-ABI smoke. Build from HDR Player checkout with the command in
// docs/picture-in-picture.md; all libmpv calls run off AppKit's main thread.
import AppKit
import AVFoundation
import CoreVideo
import Foundation
import QuartzCore

struct ProbeError: Error { let message: String }
final class ExportProbe: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    var readyToQuit = false
    func applicationDidFinishLaunching(_ notification: Notification) {
        window = NSWindow(contentRect: NSRect(x: 200, y: 180, width: 480, height: 288),
            styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        let view = NSView(frame: NSRect(x: 0, y: 0, width: 480, height: 288))
        window.contentView = view; window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        let pointer = Int64(Int(bitPattern: Unmanaged.passUnretained(view).toOpaque()))
        DispatchQueue.global(qos: .userInitiated).async { self.run(pointer) }
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        readyToQuit ? .terminateNow : .terminateCancel
    }
    func run(_ view: Int64) {
        var checks: [String: Bool] = [:], events: [[String: Any]] = []
        var errors: [String] = []
        var client: OpaquePointer? = mpv_create(), exporter: OpaquePointer?
        var retained: [OpaquePointer] = []
        func check(_ name: String, _ passed: Bool) throws {
            checks[name] = passed
            if !passed { throw ProbeError(message: name) }
        }
        func command(_ args: [String]) throws {
            let strings = args.map { strdup($0) }
            defer { strings.forEach { free($0) } }
            var pointers = strings.map { UnsafePointer<CChar>($0) } + [nil]
            let result = pointers.withUnsafeMutableBufferPointer { mpv_command(client, $0.baseAddress) }
            guard result >= 0 else { throw ProbeError(message: "command \(args): \(String(cString: mpv_error_string(result)))") }
        }
        func state() -> [String: Any] {
            guard let text = mpv_get_property_string(client, "enhancement-state") else { return [:] }
            defer { mpv_free(text) }
            return (try? JSONSerialization.jsonObject(with: Data(String(cString: text).utf8))) as? [String: Any] ?? [:]
        }
        func poll(_ after: UInt64 = 0) -> (mpv_hdr_status, mpv_hdr_snapshot, OpaquePointer?) {
            var snapshot = mpv_hdr_snapshot()
            snapshot.struct_size = UInt32(MemoryLayout<mpv_hdr_snapshot>.size)
            snapshot.abi_version = UInt32(MPV_HDR_EXPORT_ABI_VERSION)
            var frame: OpaquePointer?
            let result = mpv_hdr_export_poll(exporter, after, &snapshot, &frame)
            return (result, snapshot, frame)
        }
        func next(_ after: UInt64 = 0) throws -> (mpv_hdr_snapshot, OpaquePointer) {
            let deadline = CACurrentMediaTime() + 20
            var reason: UInt32 = 0
            while CACurrentMediaTime() < deadline {
                let (status, snapshot, frame) = poll(after)
                reason = snapshot.reason
                if status == MPV_HDR_FRAME_READY, let frame { return (snapshot, frame) }
                _ = mpv_wait_event(client, 0.005)
            }
            throw ProbeError(message: "No supported frame within20s; reason=\(reason), state=\(state())")
        }
        func pixel(_ frame: OpaquePointer) -> CVPixelBuffer {
            Unmanaged<CVPixelBuffer>.fromOpaque(mpv_hdr_frame_get(frame)!.pointee.pixel_buffer!).takeUnretainedValue()
        }
        do {
            try check("created", client != nil)
            let args = CommandLine.arguments
            guard args.count == 4 else { throw ProbeError(message: "usage: probe SOURCE MODEL REPORT") }
            let options = ["config": "no", "vo": "gpu-next", "gpu-api": "vulkan", "gpu-context": "macvk",
                "wid": String(view), "hwdec": "videotoolbox", "ao": "coreaudio", "mute": "yes", "pause": "yes",
                "sid": "no", "secondary-sid": "no", "keep-open": "yes", "target-colorspace-hint": "yes",
                "osc": "no", "osd-level": "0", "msg-level": "all=warn",
                "vf": "@enhance:metal-hdr=model=%\(args[2].utf8.count)%\(args[2]):processing-width=32:processing-height=24:strength=1:colour-strength=1:maximum-luminance-ratio=2:reference-white=203:policy=adaptive"]
            for (key, value) in options { try check("option-\(key)", mpv_set_option_string(client, key, value) >= 0) }
            try check("initialized", mpv_initialize(client) >= 0)
            try check("invalid-capacity-rejected", mpv_hdr_export_open(client, 0, &exporter) == MPV_HDR_INVALID)
            try check("export-open", mpv_hdr_export_open(client, 2, &exporter) == MPV_HDR_FRAME_READY)
            var duplicate: OpaquePointer?
            try check("second-exporter-rejected", mpv_hdr_export_open(client, 2, &duplicate) == MPV_HDR_FULL)
            try command(["loadfile", args[1]])
            let (first, frame) = try next(); retained.append(frame)
            try check("neural-float", first.content_kind == 2 && first.pixel_format == kCVPixelFormatType_64RGBAHalf)
            try check("clock-valid", first.clock_valid == 1 && first.host_ticks > 0 && first.rate == 0)
            let host = CMClockMakeHostTimeFromSystemUnits(first.host_ticks).seconds
            let hostError = abs(CMClockGetTime(CMClockGetHostTimeClock()).seconds - host)
            try check("clock-host-epoch", hostError < 0.1)
            events.append(["phase": "clock-anchor", "hostErrorSeconds": hostError,
                "sampleSpanSeconds": CMClockMakeHostTimeFromSystemUnits(first.clock_sample_span_ticks).seconds,
                "mediaSeconds": first.media_seconds, "rate": first.rate])
            let buffer = pixel(frame)
            try check("producer-linear-tag", CVBufferCopyAttachment(buffer, kCVImageBufferTransferFunctionKey, nil) as? String == kCVImageBufferTransferFunction_Linear as String)
            try check("producer-bt2020-tag", CVBufferCopyAttachment(buffer, kCVImageBufferColorPrimariesKey, nil) as? String == kCVImageBufferColorPrimaries_ITU_R_2020 as String)
            try check("source-path", String(cString: mpv_hdr_frame_get(frame)!.pointee.source_path) == args[1])
            let (_, _, second) = poll(); try check("second-lease", second != nil); retained.append(second!)
            let (full, cap, extra) = poll(); try check("lease-cap", full == MPV_HDR_FULL && extra == nil && cap.outstanding_leases == 2)
            let (same, _, repeated) = poll(first.revision)
            try check("unchanged-no-lease", same == MPV_HDR_UNCHANGED && repeated == nil)
            mpv_hdr_frame_release(retained.removeLast())
            let before = state()["submitted-frames"] as? Int
            try command(["vf-command", "enhance", "compare", "original"])
            let originalDeadline = CACurrentMediaTime() + 2
            var original = first
            repeat { (_, original, _) = poll(first.revision); Thread.sleep(forTimeInterval: 0.005) }
            while original.revision == first.revision && CACurrentMediaTime() < originalDeadline
            try check("original-invalidates", original.revision > first.revision && original.supported == 0 && mpv_hdr_frame_is_current(frame) == 0)
            try command(["vf-command", "enhance", "compare", "enhanced"])
            let (enhanced, compared) = try next(original.revision); retained.append(compared)
            try check("compare-exact-pts", enhanced.source_pts.value == first.source_pts.value && enhanced.source_pts.timescale == first.source_pts.timescale)
            try check("compare-no-inference", state()["submitted-frames"] as? Int == before)
            mpv_hdr_frame_release(retained.removeFirst())
            try command(["set", "sid", "1"])
            let subtitleDeadline = CACurrentMediaTime() + 2
            var subtitleState = enhanced
            repeat { (_, subtitleState, _) = poll(enhanced.revision); Thread.sleep(forTimeInterval: 0.005) }
            while subtitleState.reason != UInt32(MPV_HDR_REASON_SUBTITLES.rawValue) && CACurrentMediaTime() < subtitleDeadline
            try check("subtitles-explicitly-unsupported", subtitleState.reason == UInt32(MPV_HDR_REASON_SUBTITLES.rawValue) && subtitleState.supported == 0)
            try check("subtitle-change-invalidates", mpv_hdr_frame_is_current(compared) == 0)
            try command(["set", "sid", "no"])
            let (resumed, resumedFrame) = try next(subtitleState.revision)
            try check("subtitle-disable-revises", resumed.revision > subtitleState.revision && resumed.supported == 1)
            mpv_hdr_frame_release(resumedFrame)
            try command(["seek", "0.7", "absolute+exact"])
            let (sought, seekFrame) = try next(enhanced.revision); retained.append(seekFrame)
            try check("seek-invalidates", mpv_hdr_frame_is_current(compared) == 0 && sought.generation != enhanced.generation && sought.stream_epoch != enhanced.stream_epoch)
            try check("seek-correct-pts", Double(sought.source_pts.value) / Double(sought.source_pts.timescale) >= 0.695)
            mpv_hdr_frame_release(retained.removeFirst())
            events.append(["phase": "paused-seek", "revision": sought.revision, "epoch": sought.stream_epoch,
                "generation": sought.generation, "pts": sought.source_pts.value, "timescale": sought.source_pts.timescale,
                "sourceOffset": sought.source_to_player_seconds, "clockSource": sought.clock_source])
            try command(["set", "pause", "no"])
            mpv_hdr_frame_release(retained.removeLast())
            DispatchQueue.main.sync { self.window.miniaturize(nil) }
            let end = CACurrentMediaTime() + 3
            var revision = sought.revision, selected = 0, audioClocks = 0
            var mediaTimes: [Double] = [], progressingClocks = 0
            var maxPending: UInt32 = 0, maxLeases: UInt32 = 0
            while CACurrentMediaTime() < end {
                let (_, s, f) = poll(revision)
                maxPending = max(maxPending, s.producer_pending_frames); maxLeases = max(maxLeases, s.outstanding_leases)
                if s.clock_valid != 0 && s.clock_source == 1 {
                    audioClocks += 1; mediaTimes.append(s.media_seconds)
                    if s.rate > 0 { progressingClocks += 1 }
                }
                if let f { selected += 1; revision = s.revision; mpv_hdr_frame_release(f) }
                _ = mpv_wait_event(client, 0.005)
            }
            events.append(["phase": "minimized-playback", "newSelectedFrames": selected, "audioClockSamples": audioClocks,
                "maxPending": maxPending, "maxLeases": maxLeases, "progressingClockSamples": progressingClocks,
                "mediaAdvanceSeconds": (mediaTimes.last ?? 0) - (mediaTimes.first ?? 0)])
            try check("minimized-selection-advances", selected >= 3)
            try check("actual-audio-clock", audioClocks > 3)
            try check("audio-clock-advances", progressingClocks > 0 && (mediaTimes.last ?? 0) - (mediaTimes.first ?? 0) > 0.1)
            DispatchQueue.main.sync { self.window.deminiaturize(nil) }
            try command(["set", "pause", "yes"])
            let (_, final) = try next(); retained.append(final)
            let (_, _, finalExtra) = poll(); try check("two-before-reopen", finalExtra != nil)
            retained.append(finalExtra!)
            mpv_hdr_export_close(exporter); exporter = nil
            try check("close-invalidates", mpv_hdr_frame_is_current(final) == 0)
            try check("export-reopen", mpv_hdr_export_open(client, 2, &exporter) == MPV_HDR_FRAME_READY)
            var (reopened, reopenedState, _) = poll()
            let reopenDeadline = CACurrentMediaTime() + 2
            while reopened == MPV_HDR_NO_FRAME && CACurrentMediaTime() < reopenDeadline {
                Thread.sleep(forTimeInterval: 0.005)
                (reopened, reopenedState, _) = poll()
            }
            try check("reopen-preserves-global-cap", reopened == MPV_HDR_FULL && reopenedState.outstanding_leases == 2)
            mpv_hdr_frame_release(retained.removeLast())
            let (_, reopenedFrame) = try next(); retained.append(reopenedFrame)
            mpv_hdr_export_close(exporter); exporter = nil
            let width = CVPixelBufferGetWidth(pixel(final))
            mpv_terminate_destroy(client); client = nil
            try check("lease-survives-client-destroy", CVPixelBufferGetWidth(pixel(final)) == width && width > 0)
        } catch { errors.append(String(describing: error)) }
        if let exporter { mpv_hdr_export_close(exporter) }
        if let client { mpv_terminate_destroy(client) }
        retained.forEach { mpv_hdr_frame_release($0) }
        let report: [String: Any] = ["checks": checks, "events": events, "errors": errors,
            "passed": errors.isEmpty && checks.values.allSatisfy { $0 },
            "scope": "selected-frame API, exact timing/compare/seek, bounded leases and minimized audio-clock progression; no physical scanout or AVKit colour qualification"]
        let data = try! JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys])
        try? data.write(to: URL(fileURLWithPath: CommandLine.arguments.last!))
        print(String(data: data, encoding: .utf8)!)
        DispatchQueue.main.async { self.readyToQuit = true; NSApp.terminate(nil) }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = ExportProbe()
app.delegate = delegate
app.run()
