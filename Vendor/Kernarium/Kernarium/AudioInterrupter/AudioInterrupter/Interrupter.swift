// SPDX-License-Identifier: GPL-2.0-or-later

import AVFAudio
import MediaPlayer
import UIKit

/// Plays digital silence and, when the application in front holds the audio
/// session, takes the session from it, so that a run can watch what that
/// application does when it is interrupted (Kernarium mutes its guest's audio
/// and takes the session back when the interrupter lets go).
///
/// iOS refuses a backgrounded application a non-mixable session unless the
/// application is the Now Playing application, so this one registers as that
/// (Now Playing information and remote commands), keeps the audio
/// background mode, and never lets its own session lapse: when something takes
/// the session from it, it keeps playing silence on a mixable session, which
/// does not interrupt the other application, and tries again every
/// `retrySeconds` to switch that session to a non-mixable one. The first
/// switch that iOS accepts interrupts the other application. After
/// `holdSeconds` of owning the session the interrupter ends as `end` says.
///
/// Every decision and every refusal is printed to the console as an
/// `AUDIO_INTERRUPTER` line, which a launch with `--console` captures.
@MainActor
final class Interrupter {
    enum Mode: String {
        /// Starts as the non-mixable owner. A later interruption by the other
        /// application is answered after `retrySeconds`.
        case direct
        /// Starts on a mixable session and takes the session once the other
        /// application plays primary audio.
        case `switch`
    }

    enum End: String {
        /// Stops playing and deactivates the session without telling anyone,
        /// as a page that stops its own audio does: the other application
        /// learns of it only by asking whether other audio plays.
        case quiet
        /// Deactivates the session and notifies the applications it
        /// interrupted, which sends them a resumption recommendation.
        case notify
        /// Keeps the session until the process is terminated.
        case stay
    }

    struct Configuration {
        var mode = Mode.direct
        var holdSeconds = 60.0
        var end = End.quiet
        var maximumSeconds = 300.0
        var retrySeconds = 5.0

        init() {}

        /// `AUDIO_INTERRUPTER_MODE` (direct, switch), `_HOLD` (seconds of
        /// owning the session), `_END` (quiet, notify, stay), `_MAX` (seconds
        /// of life) and `_RETRY` (seconds between takeover attempts).
        init(environment: [String: String]) {
            mode = environment["AUDIO_INTERRUPTER_MODE"].flatMap(Mode.init) ?? mode
            end = environment["AUDIO_INTERRUPTER_END"].flatMap(End.init) ?? end
            holdSeconds = environment["AUDIO_INTERRUPTER_HOLD"].flatMap(Double.init) ?? holdSeconds
            maximumSeconds = environment["AUDIO_INTERRUPTER_MAX"].flatMap(Double.init) ?? maximumSeconds
            retrySeconds = environment["AUDIO_INTERRUPTER_RETRY"].flatMap(Double.init) ?? retrySeconds
        }
    }

    private let configuration: Configuration
    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private let silence: AVAudioPCMBuffer
    private let begun = Date()
    private var started = false
    private var connected = false
    private var timer: Timer?
    private var observers: [NSObjectProtocol] = []
    private var background = UIBackgroundTaskIdentifier.invalid

    /// The session is ours and non-mixable.
    private var owner = false
    /// Something took the session since the last takeover.
    private var interrupted = false
    private var interruptions = 0
    private var attempts = 0
    private var nextAttempt = Date.distantFuture
    private var hintSince: Date?
    private var ownedSince: Date?
    private var ended = false
    private var deactivating = false
    private var lastReport = Date.distantPast

    init(configuration: Configuration) {
        self.configuration = configuration
        let format = AVAudioFormat(standardFormatWithSampleRate: 48_000, channels: 2)!
        let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: 4_800)!
        buffer.frameLength = buffer.frameCapacity
        for channel in 0..<Int(format.channelCount) {
            buffer.floatChannelData![channel].update(repeating: 0, count: Int(buffer.frameLength))
        }
        silence = buffer
        engine.attach(player)
    }

    func start() {
        guard !started else { return }
        started = true
        log("event=started pid=\(ProcessInfo.processInfo.processIdentifier) mode=\(configuration.mode.rawValue) "
            + "hold=\(configuration.holdSeconds) end=\(configuration.end.rawValue) "
            + "max=\(configuration.maximumSeconds) retry=\(configuration.retrySeconds)")
        registerNowPlaying()
        observe()
        switch configuration.mode {
        case .direct:
            if establish(mixable: false) {
                markOwned()
            } else {
                nextAttempt = Date()
            }
        case .switch:
            _ = establish(mixable: true)
            // The tick takes the session three seconds after other audio
            // starts, not before the first interruption.
            nextAttempt = Date()
        }
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
    }

    // MARK: Session and playback

    /// Activates the session and starts the silence; true when both worked.
    @discardableResult
    private func establish(mixable: Bool) -> Bool {
        do {
            try activate(mixable: mixable)
            try play()
            return true
        } catch {
            log("event=refused mixable=\(mixable) \(describe(error))")
            return false
        }
    }

    private func activate(mixable: Bool) throws {
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playback, mode: .default, options: mixable ? [.mixWithOthers] : [])
        try session.setActive(true)
    }

    private func play() throws {
        if !connected {
            try engine.connectNode(player, to: engine.mainMixerNode, format: silence.format)
            connected = true
        }
        if !engine.isRunning { try engine.start() }
        player.stop()
        player.scheduleBuffer(silence, at: nil, options: .loops)
        try player.playAudio()
        MPNowPlayingInfoCenter.default().playbackState = .playing
    }

    /// Takes the session as the owner and starts the hold.
    private func markOwned() {
        owner = true
        interrupted = false
        if ownedSince == nil { ownedSince = Date() }
        log("event=owned attempt=\(attempts) interruptions=\(interruptions) \(audioState())")
    }

    /// Switches to the non-mixable session; the other application is
    /// interrupted if iOS accepts it.
    private func takeOver() {
        attempts += 1
        nextAttempt = Date().addingTimeInterval(configuration.retrySeconds)
        do {
            try activate(mixable: false)
            try play()
            markOwned()
        } catch {
            log("event=takeover-refused attempt=\(attempts) \(describe(error)) \(audioState())")
            // Stay alive on a mixable session so the process is not suspended.
            _ = establish(mixable: true)
        }
    }

    private func finish() {
        ended = true
        log("event=ending end=\(configuration.end.rawValue) \(audioState())")
        guard configuration.end != .stay else { return }
        deactivating = true
        player.stop()
        engine.stop()
        MPNowPlayingInfoCenter.default().playbackState = .stopped
        do {
            try AVAudioSession.sharedInstance().setActive(
                false, options: configuration.end == .notify ? [.notifyOthersOnDeactivation] : [])
            log("event=ended end=\(configuration.end.rawValue)")
        } catch {
            log("event=end-refused \(describe(error))")
        }
        owner = false
        deactivating = false
    }

    // MARK: Observation

    private func registerNowPlaying() {
        let commands = MPRemoteCommandCenter.shared()
        commands.playCommand.isEnabled = true
        commands.playCommand.addTarget { [weak self] _ in
            MainActor.assumeIsolated { self?.nextAttempt = Date() }
            return .success
        }
        commands.pauseCommand.isEnabled = true
        commands.pauseCommand.addTarget { _ in .success }
        MPNowPlayingInfoCenter.default().nowPlayingInfo = [
            MPMediaItemPropertyTitle: "Audio interrupter (test support)",
            MPNowPlayingInfoPropertyPlaybackRate: 1.0,
        ]
    }

    private func observe() {
        let center = NotificationCenter.default
        // The names are spelled out: iOS 27 added two for the old
        // interruption notification, and the log should show whichever iOS
        // posts.
        for name in [
            "AVAudioSessionDidBecomeInactiveNotification",
            "AVAudioSessionInterruptionNotification",
            "AVAudioSessionResumptionRecommendationNotification",
            "AVAudioSessionRouteChangeNotification",
            "AVAudioEngineConfigurationChangeNotification",
        ] {
            observers.append(center.addObserver(
                forName: Notification.Name(name), object: nil, queue: .main
            ) { [weak self] notification in
                MainActor.assumeIsolated { self?.notified(name, notification) }
            })
        }
    }

    private func notified(_ name: String, _ notification: Notification) {
        let short = name.replacingOccurrences(of: "AVAudio", with: "")
            .replacingOccurrences(of: "Notification", with: "")
        let keys = (notification.userInfo ?? [:]).keys.map { "\($0)" }.sorted().joined(separator: ",")
        log("event=notification name=\(short) keys=\(keys) deactivating=\(deactivating) \(audioState())")
        let began: Bool
        switch name {
        case "AVAudioSessionDidBecomeInactiveNotification":
            began = true
        case "AVAudioSessionInterruptionNotification":
            // AVAudioSessionInterruptionType.began; the type is deprecated in iOS 27.
            began = (notification.userInfo?[AVAudioSessionInterruptionTypeKey] as? NSNumber)?.uintValue == 1
        default:
            began = false
        }
        // Our own end of the hold deactivates the session and posts the same notification.
        if began, !ended, !deactivating, owner || !interrupted {
            owner = false
            interrupted = true
            interruptions += 1
            nextAttempt = Date().addingTimeInterval(configuration.retrySeconds)
            MPNowPlayingInfoCenter.default().playbackState = .interrupted
            log("event=interrupted by=other interruptions=\(interruptions)")
            if background == .invalid {
                background = UIApplication.shared.beginBackgroundTask(withName: "interrupted") { [weak self] in
                    MainActor.assumeIsolated { self?.releaseBackgroundTask() }
                }
            }
            // Keep running without interrupting the other application.
            _ = establish(mixable: true)
        }
    }

    private func releaseBackgroundTask() {
        guard background != .invalid else { return }
        UIApplication.shared.endBackgroundTask(background)
        background = .invalid
    }

    private func tick() {
        let now = Date()
        if now.timeIntervalSince(begun) > configuration.maximumSeconds {
            log("event=exit reason=maximum")
            exit(0)
        }
        if now.timeIntervalSince(lastReport) >= 2 {
            lastReport = now
            log("event=state owner=\(owner) interrupted=\(interrupted) ended=\(ended) \(audioState())")
        }
        if ended { return }
        if let since = ownedSince, now.timeIntervalSince(since) >= configuration.holdSeconds {
            releaseBackgroundTask()
            finish()
            return
        }
        if owner {
            releaseBackgroundTask()
            // Replays, as the page did: a stopped engine starts again.
            if !engine.isRunning || !player.isPlaying { try? play() }
            return
        }
        let hint = AVAudioSession.sharedInstance().secondaryAudioShouldBeSilencedHint
        switch configuration.mode {
        case .direct:
            if now >= nextAttempt { takeOver() }
        case .switch:
            if hint {
                if hintSince == nil { hintSince = now }
                if now.timeIntervalSince(hintSince!) >= 3, now >= nextAttempt { takeOver() }
            } else {
                hintSince = nil
                if interrupted, now >= nextAttempt { takeOver() }
            }
        }
    }

    // MARK: Logging

    private func audioState() -> String {
        let session = AVAudioSession.sharedInstance()
        let application: String
        switch UIApplication.shared.applicationState {
        case .active: application = "active"
        case .inactive: application = "inactive"
        case .background: application = "background"
        @unknown default: application = "unknown"
        }
        return "app=\(application) hint=\(session.secondaryAudioShouldBeSilencedHint) "
            + "other=\(session.isOtherAudioPlaying) engine=\(engine.isRunning) player=\(player.isPlaying)"
    }

    private func describe(_ error: Error) -> String {
        let nsError = error as NSError
        let code = nsError.code
        var four = ""
        if code > 0x2020_2020 {
            let value = UInt32(truncatingIfNeeded: code)
            let bytes = [UInt8(value >> 24 & 255), UInt8(value >> 16 & 255), UInt8(value >> 8 & 255), UInt8(value & 255)]
            four = String(bytes: bytes, encoding: .ascii).map { " fourcc='\($0)'" } ?? ""
        }
        return "domain=\(nsError.domain) code=\(code)\(four) text=\"\(nsError.localizedDescription)\""
    }

    private func log(_ text: String) {
        let milliseconds = Int(Date().timeIntervalSince(begun) * 1000)
        let line = "AUDIO_INTERRUPTER t=\(milliseconds) \(text)\n"
        FileHandle.standardOutput.write(Data(line.utf8))
    }
}
