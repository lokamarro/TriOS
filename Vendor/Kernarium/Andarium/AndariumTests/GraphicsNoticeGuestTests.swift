// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The notice that Android ran out of graphics memory, on a real guest with a
/// real refusal. Runs alone with ANDARIUM_GRAPHICS_NOTICE_TESTS=1 and
/// ANDARIUM_GUEST_INSTANCE=graphics-notice, on a Mac (Designed for iPad: the
/// host GPU budget there is half the guest's RAM at the default 1536 MiB) or
/// an iPad, with an isolated guest whose Android has Chrome installed and
/// its terms accepted. The test installs nothing and deletes nothing: erase
/// the guest with an ANDARIUM_ERASE=1 launch (README).
///
/// Chrome opens a page, served by the guest's own netcat on its loopback,
/// that creates mipmapped 1024 by 1024 textures until it has taken more than
/// the budget, which the host refuses with the guest's GL_OUT_OF_MEMORY.
/// Until then the notice must not appear, while the guest boots and idles
/// and while Chrome draws within the budget; the refusal must show it once;
/// and a dismissal must take it away.
@MainActor
struct GraphicsNoticeGuestTests {
    /// A page that takes `count` textures of 5.6 MiB each, in steps, and
    /// says how far it got.
    private static func page(textures count: Int) -> String {
        """
        <canvas id=c></canvas><pre id=o></pre><script>
        var gl=document.getElementById('c').getContext('webgl2'),d=new Uint8Array(4194304),n=0;
        (function s(){if(gl.isContextLost()){o.textContent='lost '+n;return}
        for(var i=0;i<4;i++){var t=gl.createTexture();gl.bindTexture(3553,t);
        gl.texImage2D(3553,0,6408,1024,1024,0,6408,5121,d);gl.generateMipmap(3553);n++}
        o.textContent=n;if(n<\(count))setTimeout(s,30)})()
        </script>
        """
    }

    private static let port = 8081
    private static let served = "/data/local/tmp/graphics-notice.http"

    /// Makes the guest's own netcat answer every request on its loopback with
    /// `html`, so the page needs no network. Chrome in the image opens only
    /// http and https links, not data or file ones.
    private func serve(_ html: String, with operations: AdbGuestOperations) async throws {
        let body = Data(html.utf8).base64EncodedString()
        let head = "HTTP/1.0 200 OK\\r\\nContent-Type: text/html\\r\\nCache-Control: no-store\\r\\n\\r\\n"
        let result = try await operations.shell(
            "printf '\(head)' > \(Self.served) && echo '\(body)' | base64 -d >> \(Self.served)",
            limit: .seconds(30))
        try #require(result.exitCode == 0, "the page was not written: \(result.text)")
    }

    private func startServer(with operations: AdbGuestOperations) async throws {
        let result = try await operations.shell(
            "(nohup nc -L -s 127.0.0.1 -p \(Self.port) cat \(Self.served) </dev/null >/dev/null 2>&1 &)",
            limit: .seconds(30))
        try #require(result.exitCode == 0, "the page server did not start: \(result.text)")
    }

    /// Opens the served page in Chrome. A new `load` number makes a new load.
    private func open(load: Int, with operations: AdbGuestOperations) async throws {
        let result = try await operations.shell(
            "am start -a android.intent.action.VIEW -d 'http://127.0.0.1:\(Self.port)/?load=\(load)' -p com.android.chrome",
            limit: .seconds(60))
        try #require(result.exitCode == 0, "Chrome did not open the page: \(result.text)")
    }

    @Test(.timeLimit(.minutes(25)))
    func aRealRefusalShowsTheNoticeOnceAndNothingElseDoes() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_GRAPHICS_NOTICE_TESTS"] == "1" else { return }
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "graphics-notice")
        try await withAndroidGuest(cleanupCommand: "rm -f \(Self.served)") { model, operations in
            let driver = try #require(model.driver)
            let chrome = try await operations.shell("pm path com.android.chrome", limit: .seconds(30))
            try #require(
                chrome.text.contains("package:"),
                "the guest needs Chrome installed with its terms accepted (see the test's header)")

            // The guest has booted and idled: nothing was refused.
            let before = try #require(await driver.readGraphicsRefusals())
            print("GRAPHICS_NOTICE: refusals after boot: \(before)")
            #expect(before == 0)
            #expect(!model.graphicsNotice.isShowing)

            // Chrome draws within the budget: still nothing.
            try await startServer(with: operations)
            try await serve(Self.page(textures: 20), with: operations)
            try await open(load: 1, with: operations)
            try await Task.sleep(for: .seconds(30))
            let within = try #require(await driver.readGraphicsRefusals())
            print("GRAPHICS_NOTICE: refusals after 20 textures: \(within)")
            #expect(within == 0)
            #expect(!model.graphicsNotice.isShowing)

            // Chrome takes more than the budget: the host refuses, and the
            // notice shows within a few readings.
            try await serve(Self.page(textures: 600), with: operations)
            try await open(load: 2, with: operations)
            let deadline = ContinuousClock.now + .seconds(240)
            while !model.graphicsNotice.isShowing, ContinuousClock.now < deadline {
                try await Task.sleep(for: .milliseconds(500))
            }
            let after = try #require(await driver.readGraphicsRefusals())
            print("GRAPHICS_NOTICE: refusals after the stress page: \(after); notice shown: \(model.graphicsNotice.isShowing)")
            #expect(after > 0, "the host refused nothing")
            #expect(model.graphicsNotice.isShowing, "a real refusal did not show the notice")

            // The guest runs on, and the notice goes when the person says so.
            let alive = try await operations.shell("getprop sys.boot_completed", limit: .seconds(30))
            #expect(alive.text.trimmingCharacters(in: .whitespacesAndNewlines) == "1")
            model.dismissGraphicsNotice()
            #expect(!model.graphicsNotice.isShowing)
        }
    }
}
