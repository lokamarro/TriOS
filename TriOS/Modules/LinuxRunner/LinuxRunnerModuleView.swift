
import SwiftUI

/// Module LinuxRunner : reprend l'organisation d'origine
/// (Accueil / Distributions / Terminal / Bureau / Réglages,
/// ManagerView.swift) dans une fenêtre TriOS.
struct LinuxRunnerModuleView: View {
    @ObservedObject var instance: AppInstance
    @State private var tab = 0
    @State private var distros: [LinuxDistro] = TriOSStorage.load([LinuxDistro].self, file: "distros.json", for: .linuxRunner) ?? LinuxDistro.catalog
    @State private var activeRoot: String = "Debian"

    var body: some View {
        VStack(spacing: 0) {
            if !instance.adapter.isLinked { EngineNotLinkedBanner(app: .linuxRunner) }
            Picker("", selection: $tab) {
                Text("Accueil").tag(0); Text("Distributions").tag(1); Text("Terminal").tag(2); Text("Bureau").tag(4); Text("Journal").tag(3)
            }
            .pickerStyle(.segmented).padding(12)

            switch tab {
            case 0: home
            case 1: distributions
            case 2: LinuxTerminalView(instance: instance)
            case 4: LinuxDesktopHostView(instance: instance)
            default: EngineLogView(instance: instance)
            }
        }
        .foregroundStyle(.white)
        .onChange(of: distros) { _, v in TriOSStorage.save(v, file: "distros.json", for: .linuxRunner) }
    }

    private var home: some View {
        VStack(spacing: 14) {
            RoundedRectangle(cornerRadius: 12)
                .fill(LinearGradient(colors: [.black, Color(red: 0.05, green: 0.25, blue: 0.12)], startPoint: .top, endPoint: .bottom))
                .aspectRatio(16/10, contentMode: .fit)
                .overlay {
                    VStack(spacing: 8) {
                        Image(systemName: "terminal.fill").font(.largeTitle).foregroundStyle(AppID.linuxRunner.accent)
                        Text("Environnement actif : \(activeRoot)").font(.headline)
                        Text(instance.engineState.label).font(.caption).foregroundStyle(.secondary)
                        Text("Bureau Linux, DeX mode sur écran externe et GPU Metal (virglrenderer → ANGLE → Metal) vivent ici une fois le moteur lié.")
                            .font(.caption2).foregroundStyle(.secondary)
                            .multilineTextAlignment(.center).padding(.horizontal, 30)
                    }.foregroundStyle(.white.opacity(0.9))
                }
                .padding(.horizontal, 16)
            HStack(spacing: 10) {
                Button {
                    (instance.adapter as? LinuxRunnerAdapter)?.activeRootName = activeRoot
                    Task { await instance.startEngine() }
                } label: { Label("Démarrer Linux", systemImage: "play.fill") }
                    .disabled(instance.engineState.isAlive)
                Button { tab = 2 } label: { Label("Terminal", systemImage: "terminal") }
                Button { Task { await instance.stopEngine() } } label: { Label("Arrêter", systemImage: "stop.fill") }
                    .disabled(!instance.engineState.isAlive)
            }
            .buttonStyle(.borderedProminent).tint(AppID.linuxRunner.accent)
            Spacer()
        }
        .padding(.top, 8)
    }

    private var distributions: some View {
        List {
            ForEach($distros) { $distro in
                HStack {
                    VStack(alignment: .leading) {
                        Text(distro.name).font(.headline)
                        Text("\(distro.packageManager) • \(distro.desktop)")
                            .font(.caption).foregroundStyle(.secondary)
                        Text(distro.status).font(.caption2).foregroundStyle(.secondary)
                    }
                    Spacer()
                    if distro.installed {
                        Button(activeRoot == distro.name ? "Actif" : "Activer") { activeRoot = distro.name }
                            .buttonStyle(.bordered).controlSize(.small)
                    } else {
                        Button("Installer") { distro.installed = true }
                            .buttonStyle(.bordered).controlSize(.small)
                            .disabled(!instance.adapter.isLinked)
                    }
                }
                .listRowBackground(Color.white.opacity(0.06))
            }
        }
        .scrollContentBackground(.hidden)
    }
}

struct LinuxDistro: Codable, Identifiable, Equatable {
    var id: String { name }
    var name: String
    var packageManager: String
    var desktop: String
    var status: String
    var installed: Bool = false

    /// Catalogue repris du README de LinuxRunner 1.1.1.
    static let catalog: [LinuxDistro] = [
        .init(name: "Debian", packageManager: "apt, dpkg", desktop: "LinuxRunner Desktop, LXQt, KDE Plasma expérimental", status: "Recommandée, le plus large support d'applications", installed: true),
        .init(name: "Arch Linux ARM", packageManager: "pacman", desktop: "LXQt, Enlightenment, Hyprland expérimental (8 Go RAM)", status: "Recommandée pour les paquets récents"),
        .init(name: "Fedora", packageManager: "dnf, rpm", desktop: "GTK/X11 léger, KDE Plasma", status: "Image userspace officielle (Fedora 44 ARM64)"),
        .init(name: "Kali Linux", packageManager: "apt, dpkg", desktop: "Xfce", status: "Outils sécurité — usage légitime uniquement, pas de mode monitor Wi-Fi"),
        .init(name: "Alpine", packageManager: "apk", desktop: "Terminal uniquement", status: "Petit environnement terminal (Alpine 3.24)"),
        .init(name: "SteamOS ARM64", packageManager: "pacman (inspection)", desktop: "Plasma X11", status: "Expérimental, image de restauration Valve ~4 Go, client Steam non supporté"),
    ]
}

/// Terminal local de démonstration de l'interface.
/// Avec le moteur lié, il est remplacé par le vrai terminal
/// (TerminalViewController / xterm de l'app d'origine) branché sur
/// le PTY du runtime. Ici, quelques commandes sont simulées pour que
/// l'interface soit testable sans le moteur.
struct LinuxTerminalView: View {
    @ObservedObject var instance: AppInstance
    @State private var lines: [String] = ["LinuxRunner (aperçu interface) — tape 'aide'", ""]
    @State private var input = ""

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(Array(lines.enumerated()), id: \.offset) { _, l in
                        Text(l).font(.system(.caption, design: .monospaced))
                            .foregroundStyle(.green.opacity(0.92))
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }.padding(10)
            }
            .background(.black)
            HStack {
                Text("➜ ~").font(.system(.caption, design: .monospaced)).foregroundStyle(AppID.linuxRunner.accent)
                TextField("commande", text: $input)
                    .font(.system(.caption, design: .monospaced))
                    .autocorrectionDisabled().textInputAutocapitalization(.never)
                    .onSubmit(run)
            }
            .padding(8).background(Color.white.opacity(0.07))
        }
        .padding(.horizontal, 12).padding(.bottom, 12)
    }

    private func run() {
        let cmd = input.trimmingCharacters(in: .whitespaces); input = ""
        lines.append("➜ ~ \(cmd)")
        switch cmd {
        case "aide", "help": lines.append("Commandes d'aperçu : uname -a, cat /etc/os-release, ls, clear. Le vrai shell arrive avec le moteur lié.")
        case "uname -a", "uname -m": lines.append("Linux trios 6.x arm64 (aperçu — moteur non lié)")
        case "cat /etc/os-release": lines.append("PRETTY_NAME=\"Debian GNU/Linux 13 (aperçu)\"")
        case "ls": lines.append("Documents  Shared  (dossier partagé : /mnt/LinuxRunnerShared)")
        case "clear": lines.removeAll()
        case "": break
        default: lines.append("aperçu : '\(cmd)' serait exécuté par le runtime LinuxRunner lié.")
        }
    }
}
