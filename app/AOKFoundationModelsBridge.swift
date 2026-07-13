import Foundation

#if canImport(FoundationModels)
import FoundationModels
#endif

// ObjC-visible availability codes -- FoundationModels' own
// SystemLanguageModel.Availability.UnavailableReason has associated values
// and isn't representable as an @objc enum, so it's flattened here.
@objc public enum AOKFoundationModelAvailability: Int {
    case available = 0
    case deviceNotEligible = 1
    case appleIntelligenceNotEnabled = 2
    case modelNotReady = 3
    case unsupportedOSVersion = 4
    case unknown = 5
}

#if canImport(FoundationModels)
// The "run_shell" tool. Executing a command means reaching back into the
// (Objective-C) guest-shell + confirmation-dialog machinery in
// AboutViewController.m, which this Swift file has no direct access to --
// so the actual work is delegated through AOKFoundationModelsBridge's
// shellCommandHandler, which AboutViewController installs before starting a
// tool-enabled session.
@available(iOS 26.0, *)
struct AOKShellTool: Tool {
    let name = "run_shell"
    let description = "Run a shell command in the iSH guest Linux environment and return its combined stdout+stderr. Use this to list/read files, check installed tools, or run quick scripts. Each call runs one command and may require the user's confirmation before it executes."

    @Generable
    struct Arguments {
        @Guide(description: "The shell command to execute, e.g. \"ls -la /root\"")
        var command: String
    }

    func call(arguments: Arguments) async throws -> String {
        guard let handler = AOKFoundationModelsBridge.shellCommandHandler else {
            return "The shell tool is not available right now."
        }
        return await withCheckedContinuation { continuation in
            handler(arguments.command) { output in
                continuation.resume(returning: output)
            }
        }
    }
}
#endif

// Bridges Apple's FoundationModels (Swift-only, iOS 26+) to the Objective-C
// chat UI in AboutViewController.m. Every entry point is safe to call on any
// OS version/SDK: below iOS 26, or when the framework wasn't linked, calls
// resolve to .unsupportedOSVersion / an explanatory error instead of trapping.
@objc(AOKFoundationModelsBridge)
public final class AOKFoundationModelsBridge: NSObject {

    // Installed by AboutViewController before a tools-enabled request. Called
    // with the requested command and a completion the handler must invoke
    // exactly once, on any thread, with the text to feed back to the model
    // (command output, or an explanation if declined/unavailable).
    @objc public static var shellCommandHandler: ((String, @escaping (String) -> Void) -> Void)?

    @objc public static func currentAvailability() -> AOKFoundationModelAvailability {
        #if canImport(FoundationModels)
        if #available(iOS 26.0, *) {
            switch SystemLanguageModel.default.availability {
            case .available:
                return .available
            case .unavailable(let reason):
                switch reason {
                case .deviceNotEligible: return .deviceNotEligible
                case .appleIntelligenceNotEnabled: return .appleIntelligenceNotEnabled
                case .modelNotReady: return .modelNotReady
                @unknown default: return .unknown
                }
            }
        }
        #endif
        return .unsupportedOSVersion
    }

    @objc public static func availabilityDescription() -> String {
        switch currentAvailability() {
        case .available:
            return "Apple Foundation Models is available on this device."
        case .deviceNotEligible:
            return "This device does not support Apple Intelligence."
        case .appleIntelligenceNotEnabled:
            return "Apple Intelligence is turned off. Enable it in Settings \u{2192} Apple Intelligence & Siri."
        case .modelNotReady:
            return "The on-device model is still downloading or preparing. Try again shortly."
        case .unsupportedOSVersion:
            return "Apple Foundation Models requires iOS/iPadOS 26 or later."
        case .unknown:
            return "Apple Foundation Models is unavailable for an unknown reason."
        }
    }

    // Non-streaming request/response. completion is always called, exactly
    // once, off the main thread -- callers must hop back to main themselves.
    @objc public static func respond(toPrompt prompt: String, instructions: String?, toolsEnabled: Bool, completion: @escaping (String?, String?) -> Void) {
        #if canImport(FoundationModels)
        if #available(iOS 26.0, *), case .available = SystemLanguageModel.default.availability {
            Task {
                do {
                    let session = LanguageModelSession(tools: toolsEnabled ? [AOKShellTool()] : [], instructions: instructions)
                    let response = try await session.respond(to: prompt)
                    completion(response.content, nil)
                } catch {
                    completion(nil, error.localizedDescription)
                }
            }
            return
        }
        #endif
        completion(nil, availabilityDescription())
    }

    // Streaming variant. onPartial is called repeatedly with the cumulative
    // response text so far (not a delta); completion is called exactly once
    // at the end, successful or not. Both fire off the main thread. When a
    // tool call happens mid-stream, FoundationModels runs it internally
    // (invoking shellCommandHandler) before resuming generation -- there is
    // no separate callback for tool-call start/finish, only the eventual
    // text.
    @objc public static func streamResponse(toPrompt prompt: String, instructions: String?, toolsEnabled: Bool, onPartial: @escaping (String) -> Void, completion: @escaping (String?, String?) -> Void) {
        #if canImport(FoundationModels)
        if #available(iOS 26.0, *), case .available = SystemLanguageModel.default.availability {
            Task {
                do {
                    let session = LanguageModelSession(tools: toolsEnabled ? [AOKShellTool()] : [], instructions: instructions)
                    var last = ""
                    for try await snapshot in session.streamResponse(to: prompt) {
                        last = snapshot.content
                        onPartial(last)
                    }
                    completion(last, nil)
                } catch {
                    completion(nil, error.localizedDescription)
                }
            }
            return
        }
        #endif
        completion(nil, availabilityDescription())
    }
}
