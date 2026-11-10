import SwiftUI

/// Les trois applications hébergées par TriOS.
enum AppID: String, CaseIterable, Identifiable, Codable {
    case henge
    case kernarium
    case linuxRunner

    var id: String { rawValue }

    var name: String {
        switch self {
        case .henge:      return "Henge"
        case .kernarium:  return "Kernarium"
        case .linuxRunner: return "LinuxRunner"
        }
    }

    var tagline: String {
        switch self {
        case .henge:      return "macOS sur iPhone / iPad"
        case .kernarium:  return "Vrai noyau Linux (User-Mode Linux)"
        case .linuxRunner: return "Linux ARM64, terminal et bureau"
        }
    }

    /// Symbole SF Symbols utilisé partout (bureau, dock, switcher).
    var symbol: String {
        switch self {
        case .henge:      return "macwindow"
        case .kernarium:  return "cpu.fill"
        case .linuxRunner: return "terminal.fill"
        }
    }

    var fallbackSymbol: String {
        switch self {
        case .henge:      return "macwindow"
        case .kernarium:  return "cpu.fill"
        case .linuxRunner: return "terminal.fill"
        }
    }

    var accent: Color {
        switch self {
        case .henge:      return Color(red: 0.18, green: 0.42, blue: 1.0)
        case .kernarium:  return Color(red: 0.98, green: 0.55, blue: 0.15)
        case .linuxRunner: return Color(red: 0.20, green: 0.80, blue: 0.45)
        }
    }

    /// Ordre de grandeur de la RAM guest configurée par défaut.
    /// Sert à l'avertissement mémoire du gestionnaire de tâches :
    /// iOS tue le PROCESSUS ENTIER (jetsam) si la somme dépasse la
    /// limite de l'appareil, donc TriOS prévient avant, il ne peut pas
    /// isoler un moteur fautif comme le ferait un vrai OS.
    var defaultGuestRAMMB: Int {
        switch self {
        case .henge:      return 4096
        case .kernarium:  return 2048
        case .linuxRunner: return 1024
        }
    }

    var license: String {
        switch self {
        case .henge:      return "GPL-2.0-or-later"
        case .kernarium:  return "GPL-2.0-or-later"
        case .linuxRunner: return "GPL-3.0"
        }
    }
}

/// État d'un moteur, indépendant de l'état de sa fenêtre.
/// Une fenêtre fermée ou réduite ne stoppe PAS le moteur :
/// c'est ce qui permet aux trois applications de tourner en même temps.
enum EngineState: Equatable {
    case stopped
    case starting
    case running
    case suspended
    case failed(String)
    /// Le code de l'interface est présent mais la bibliothèque du
    /// moteur n'a pas encore été liée (voir Docs/BUILD.md, étape 2).
    case engineNotLinked

    var label: String {
        switch self {
        case .stopped:        return "Arrêté"
        case .starting:       return "Démarrage…"
        case .running:        return "En cours"
        case .suspended:      return "Suspendu"
        case .failed(let m):  return "Échec : \(m)"
        case .engineNotLinked: return "Moteur non lié"
        }
    }

    var isAlive: Bool {
        switch self {
        case .running, .starting, .suspended: return true
        default: return false
        }
    }
}
