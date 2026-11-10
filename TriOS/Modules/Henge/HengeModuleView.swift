
import SwiftUI

/// Module Henge : reprend la structure de l'app d'origine
/// (onglets Macs / Install / Settings de son RootView) mais vit dans
/// une fenêtre TriOS et partage son état avec le bureau.
struct HengeModuleView: View {
    @ObservedObject var instance: AppInstance
    @State private var macs: [VirtualMac] = TriOSStorage.load([VirtualMac].self, file: "macs.json", for: .henge) ?? []
    @State private var selected: VirtualMac?
    @State private var tab = 0

    var body: some View {
        VStack(spacing: 0) {
            if !instance.adapter.isLinked { EngineNotLinkedBanner(app: .henge) }
            Picker("", selection: $tab) {
                Text("Macs").tag(0); Text("Session").tag(1); Text("Installer").tag(2); Text("Journal").tag(3)
            }
            .pickerStyle(.segmented).padding(12)

            switch tab {
            case 0: macsList
            case 1: session
            case 2: install
            default: EngineLogView(instance: instance)
            }
        }
        .foregroundStyle(.white)
    }

    private var macsList: some View {
        List {
            if macs.isEmpty {
                ContentUnavailableView("Aucun Mac", systemImage: "macwindow",
                    description: Text("Crée un Mac virtuel, puis installe macOS depuis l'onglet Installer (image de restauration Apple téléchargée par le moteur, comme dans Henge d'origine : ~17 Go de téléchargement, ~26 Go installés)."))
            }
            ForEach(macs) { mac in
                HStack {
                    Image(systemName: "macwindow").foregroundStyle(AppID.henge.accent).font(.title3)
                    VStack(alignment: .leading) {
                        Text(mac.name).font(.headline)
                        Text("macOS \(mac.macOSVersion) • \(mac.ramMB / 1024) Go RAM • \(mac.cpus) CPU • \(mac.diskGB) Go")
                            .font(.caption).foregroundStyle(.secondary)
                        Text(mac.installed ? "Installé" : "Non installé").font(.caption2)
                            .foregroundStyle(mac.installed ? .green : .orange)
                    }
                    Spacer()
                    Button(selected?.id == mac.id ? "Sélectionné" : "Choisir") { selected = mac }
                        .buttonStyle(.bordered).controlSize(.small)
                }
                .listRowBackground(Color.white.opacity(0.06))
            }
            .onDelete { idx in macs.remove(atOffsets: idx); persist() }
        }
        .scrollContentBackground(.hidden)
        .toolbar {
            Button { macs.append(VirtualMac()); persist() } label: { Label("Nouveau Mac", systemImage: "plus") }
        }
    }

    private var session: some View {
        VStack(spacing: 14) {
            // Affichage réel via henge_vm_snapshot + entrées trackpad /
            // touches spéciales (HengeDisplayView). Le chemin Metal
            // plein débit d'origine peut le remplacer, voir son en-tête.
            HengeDisplayView(instance: instance)
                .padding(.horizontal, 16)
            HStack(spacing: 10) {
                Button {
                    if let selected, let adapter = instance.adapter as? HengeAdapter {
                        adapter.mac = selected
                        adapter.ramMB = UInt64(selected.ramMB)
                        adapter.cpus = Int32(selected.cpus)
                    }
                    Task { await instance.startEngine() }
                } label: { Label("Démarrer", systemImage: "play.fill") }
                .disabled(selected == nil || instance.engineState.isAlive)
                Button { Task { await instance.stopEngine() } } label: { Label("Éteindre", systemImage: "power") }
                    .disabled(!instance.engineState.isAlive)
            }
            .buttonStyle(.borderedProminent).tint(AppID.henge.accent)
            Text("Trackpad : glisser = déplacer, toucher maintenu = glisser-déposer, deux doigts = défiler. Les touches Esc/Tab/Ctrl/Option/Cmd sont au-dessus du clavier, comme dans Henge d'origine (Trackpad.swift / Input.swift).")
                .font(.caption2).foregroundStyle(.secondary)
                .multilineTextAlignment(.center).padding(.horizontal, 24)
            Spacer()
        }
        .padding(.top, 8)
    }

    private var install: some View {
        Form {
            Section("Installer macOS") {
                Text("Versions proposées par Henge 0.2 : Sequoia 15.6.1 (testée), Tahoe 26.6.2 et macOS 27.2 (non testées). L'installation démarre la ROM en mode DFU et restaure l'IPSW via henge_vm_install — elle exige le moteur lié, ~43 Go libres et l'appareil branché.")
                    .font(.caption).foregroundStyle(.secondary)
                Button("Télécharger et installer Sequoia 15.6.1") {}
                    .disabled(!instance.adapter.isLinked)
            }
            Section("Prérequis JIT") {
                Text("Le JIT est obligatoire pour Henge : StikDebug, TrollStore/TrollStore Lite, jailbreak, ou l'assistant StikJIT intégré (iOS 26+). Sans JIT, le moteur refuse de démarrer.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .scrollContentBackground(.hidden)
    }

    private func persist() { TriOSStorage.save(macs, file: "macs.json", for: .henge) }
}
