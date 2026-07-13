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

// Bridges Apple's FoundationModels (Swift-only, iOS 26+) to the Objective-C
// chat UI in AboutViewController.m. Every entry point is safe to call on any
// OS version/SDK: below iOS 26, or when the framework wasn't linked, calls
// resolve to .unsupportedOSVersion / an explanatory error instead of trapping.
@objc(AOKFoundationModelsBridge)
public final class AOKFoundationModelsBridge: NSObject {

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
    @objc public static func respond(toPrompt prompt: String, instructions: String?, completion: @escaping (String?, String?) -> Void) {
        #if canImport(FoundationModels)
        if #available(iOS 26.0, *), case .available = SystemLanguageModel.default.availability {
            Task {
                do {
                    let session = LanguageModelSession(instructions: instructions)
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
    // at the end, successful or not. Both fire off the main thread.
    @objc public static func streamResponse(toPrompt prompt: String, instructions: String?, onPartial: @escaping (String) -> Void, completion: @escaping (String?, String?) -> Void) {
        #if canImport(FoundationModels)
        if #available(iOS 26.0, *), case .available = SystemLanguageModel.default.availability {
            Task {
                do {
                    let session = LanguageModelSession(instructions: instructions)
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
