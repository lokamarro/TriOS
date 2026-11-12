// SPDX-License-Identifier: GPL-2.0-or-later
import Darwin
import Foundation
import Testing
@testable import Kernarium

/// The packaged host graphics stack, exercised inside the application process.
///
/// The guest reaches OpenGL through virglrenderer, which renders on Mesa's Zink
/// Gallium driver over the KosmicKrisp Vulkan driver. Both ride in the
/// application as embedded frameworks and are linked by the selected runtime, so
/// selecting a runtime is what brings their symbols into the process. Creating a
/// real surfaceless desktop context here is the narrowest check that the whole
/// provider chain initializes on this Metal device, independent of a guest.
///
/// The iOS Simulator has no Metal 4 device, so its stack is Mesa's software
/// renderer (llvmpipe) with no Vulkan driver and no KosmicKrisp. The same
/// checks state that contract there.
struct HostGraphicsStackTests {
    private static let eglDefaultDisplay = UnsafeMutableRawPointer(bitPattern: 0)
    private static let eglNoContext = UnsafeMutableRawPointer(bitPattern: 0)
    private static let eglNoSurface = UnsafeMutableRawPointer(bitPattern: 0)
    private static let eglPlatformSurfacelessMesa: UInt32 = 0x31DD
    private static let eglOpenGLAPI: UInt32 = 0x30A2
    private static let eglNone: Int32 = 0x3038
    private static let eglContextMajorVersion: Int32 = 0x3098
    private static let eglContextMinorVersion: Int32 = 0x30FB
    private static let eglContextOpenGLProfileMask: Int32 = 0x30FD
    private static let eglContextOpenGLCoreProfileBit: Int32 = 0x00000001
    private static let glVendor: UInt32 = 0x1F00
    private static let glRenderer: UInt32 = 0x1F01
    private static let glVersion: UInt32 = 0x1F02

#if targetEnvironment(simulator)
    /// The renderer's name in `GL_RENDERER`, and whether a Vulkan driver is
    /// packaged: the Simulator's software renderer and none.
    private static let rendererName = "llvmpipe"
    private static let packagesVulkanDriver = false
#else
    private static let rendererName = "zink"
    private static let packagesVulkanDriver = true
#endif

    private typealias GetPlatformDisplay = @convention(c) (
        UInt32, UnsafeMutableRawPointer?, UnsafePointer<Int32>?
    ) -> UnsafeMutableRawPointer?
    private typealias Initialize = @convention(c) (
        UnsafeMutableRawPointer?, UnsafeMutablePointer<Int32>?,
        UnsafeMutablePointer<Int32>?
    ) -> UInt32
    private typealias Terminate = @convention(c) (UnsafeMutableRawPointer?) -> UInt32
    private typealias BindAPI = @convention(c) (UInt32) -> UInt32
    private typealias CreateContext = @convention(c) (
        UnsafeMutableRawPointer?, UnsafeMutableRawPointer?,
        UnsafeMutableRawPointer?, UnsafePointer<Int32>?
    ) -> UnsafeMutableRawPointer?
    private typealias DestroyContext = @convention(c) (
        UnsafeMutableRawPointer?, UnsafeMutableRawPointer?
    ) -> UInt32
    private typealias MakeCurrent = @convention(c) (
        UnsafeMutableRawPointer?, UnsafeMutableRawPointer?,
        UnsafeMutableRawPointer?, UnsafeMutableRawPointer?
    ) -> UInt32
    private typealias QueryString = @convention(c) (
        UnsafeMutableRawPointer?, Int32
    ) -> UnsafePointer<CChar>?
    private typealias GetError = @convention(c) () -> Int32
    private typealias GetProcAddress = @convention(c) (
        UnsafePointer<CChar>
    ) -> UnsafeMutableRawPointer?
    private typealias GetString = @convention(c) (UInt32) -> UnsafePointer<UInt8>?

    private static func symbol<T>(_ name: String, as type: T.Type) throws -> T {
        let handle = try #require(
            dlsym(UnsafeMutableRawPointer(bitPattern: -2), name),
            "the packaged graphics stack does not export \(name)"
        )
        return unsafeBitCast(handle, to: type)
    }

    /// Loads the runtime that links the graphics frameworks into this process.
    private static func selectRuntime() throws {
        let selected = kernarium_runtime_loader_select(
            UInt32(KERNARIUM_RUNTIME_GUEST_LINUX)
        )
        try #require(selected == 0, "no bundled 4 KiB Linux runtime loaded")
    }

    @Test
    func vulkanDriverEntryPointIsPackagedOnADeviceOnly() throws {
        try Self.selectRuntime()
        // KosmicKrisp is linked directly rather than discovered through a
        // Vulkan loader, so its own entry points must be in the process. The
        // Simulator embeds no KosmicKrisp, so none may be.
        for entryPoint in [
            "vkGetInstanceProcAddr",
            "vk_icdGetInstanceProcAddr",
            "vk_icdNegotiateLoaderICDInterfaceVersion",
        ] {
            #expect(
                (dlsym(UnsafeMutableRawPointer(bitPattern: -2), entryPoint)
                    != nil) == Self.packagesVulkanDriver,
                "\(entryPoint) (Vulkan driver packaged: \(Self.packagesVulkanDriver))"
            )
        }
    }

    @Test
    func surfacelessDesktopContextRendersOnTheHostProvider() throws {
        try Self.selectRuntime()

        let getPlatformDisplay = try Self.symbol(
            "eglGetPlatformDisplay", as: GetPlatformDisplay.self
        )
        let initialize = try Self.symbol("eglInitialize", as: Initialize.self)
        let terminate = try Self.symbol("eglTerminate", as: Terminate.self)
        let bindAPI = try Self.symbol("eglBindAPI", as: BindAPI.self)
        let createContext = try Self.symbol(
            "eglCreateContext", as: CreateContext.self
        )
        let destroyContext = try Self.symbol(
            "eglDestroyContext", as: DestroyContext.self
        )
        let makeCurrent = try Self.symbol("eglMakeCurrent", as: MakeCurrent.self)
        let queryString = try Self.symbol("eglQueryString", as: QueryString.self)
        let getError = try Self.symbol("eglGetError", as: GetError.self)
        let getProcAddress = try Self.symbol(
            "eglGetProcAddress", as: GetProcAddress.self
        )

        let display = try #require(
            getPlatformDisplay(Self.eglPlatformSurfacelessMesa, nil, nil),
            "Mesa did not provide a surfaceless EGL display"
        )
        var major: Int32 = 0
        var minor: Int32 = 0
        try #require(
            initialize(display, &major, &minor) != 0,
            "eglInitialize failed with 0x\(String(getError(), radix: 16))"
        )
        defer { _ = terminate(display) }

        let extensions = try #require(queryString(display, 0x3055))
        let available = String(cString: extensions)
        // A desktop core context with no surface is what the host GL provider
        // itself uses, so both extensions are part of the packaged contract.
        #expect(available.contains("EGL_KHR_surfaceless_context"))
        #expect(available.contains("EGL_KHR_no_config_context"))

        try #require(bindAPI(Self.eglOpenGLAPI) != 0, "no desktop OpenGL API")
        let attributes: [Int32] = [
            Self.eglContextMajorVersion, 4,
            Self.eglContextMinorVersion, 6,
            Self.eglContextOpenGLProfileMask,
            Self.eglContextOpenGLCoreProfileBit,
            Self.eglNone,
        ]
        let context = try #require(
            attributes.withUnsafeBufferPointer {
                createContext(display, nil, Self.eglNoContext, $0.baseAddress)
            },
            "no OpenGL 4.6 core context: 0x\(String(getError(), radix: 16))"
        )
        defer { _ = destroyContext(display, context) }
        try #require(
            makeCurrent(display, Self.eglNoSurface, Self.eglNoSurface, context)
                != 0,
            "eglMakeCurrent failed with 0x\(String(getError(), radix: 16))"
        )
        defer {
            _ = makeCurrent(display, Self.eglNoSurface, Self.eglNoSurface, nil)
        }

        let getString = unsafeBitCast(
            try #require(getProcAddress("glGetString")), to: GetString.self
        )
        func string(_ name: UInt32) -> String {
            guard let value = getString(name) else { return "" }
            return String(cString: value)
        }
        let renderer = string(Self.glRenderer)
        let vendor = string(Self.glVendor)
        let version = string(Self.glVersion)
        // The identity of the provider this mode actually reached is the
        // evidence the GPU lane reads back, so record it in the test log.
        print("host GL provider: \(vendor) | \(renderer) | \(version)")
        // Zink names the Vulkan driver it renders on, which must be the
        // packaged KosmicKrisp driver rather than any ambient loader. The
        // Simulator's provider is llvmpipe, which names no driver.
        #expect(
            renderer.lowercased().contains(Self.rendererName),
            "renderer: \(renderer)"
        )
        #expect(vendor.lowercased().contains("mesa"), "vendor: \(vendor)")
        #expect(version.hasPrefix("4.6"), "version: \(version)")
    }
}
