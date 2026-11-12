#!/bin/sh
# VS Code 1.141.2 tarball, fresh profile; no sandbox-weakening flags.
# Extended session: two tabs, edits saved, new untitled file, integrated
# terminal, extension host, quit, then relaunch and verify the saved edits.
# Replace @TAG@ with a fresh run tag; compile vscode_session_driver.c in the guest
# into /home/mobile/Downloads; run with the passive probe (passive_session_probe.m)
# and LR_PASSIVE_MARKER=home/<guest-user>/lr-code-<tag>-done.
set -u
tag=@TAG@
p="$HOME/.config/lr-code-$tag"; e="$HOME/.local/share/lr-code-$tag-ext"; ws="$HOME/lr-code-$tag"
test ! -e "$p" || exit 2
mkdir -p "$p/User" "$e" "$ws"
# Fresh-profile preferences: no welcome page, in-window dialogs, files opened
# without a folder, hot exit for untitled files. VS Code 1.141 still shows the
# modal workspace-trust prompt for loose files when the terminal starts; with
# the prompt up, keys meant for the terminal answer it instead (seen as the
# terminal "losing" Return and "echo " in 885-887). Trust is off for this
# fresh test profile only.
cat > "$p/User/settings.json" <<'EOF'
{ "workbench.startupEditor": "none", "window.dialogStyle": "custom", "files.hotExit": "onExit",
  "telemetry.telemetryLevel": "off", "update.mode": "none", "extensions.autoUpdate": false,
  "chat.disableAIFeatures": true, "workbench.welcomePage.walkthroughs.openOnInstall": false,
  "security.workspace.trust.enabled": false }
EOF
printf 'alpha file\n' > "$ws/a.txt"; printf 'beta file\n' > "$ws/b.txt"
code=/opt/linuxrunner-fixture-apps-874/code/VSCode-linux-arm64/code
driver=/home/mobile/Downloads/vscode_session_driver
"$code" --enable-logging=stderr --user-data-dir="$p" --extensions-dir="$e" "$ws/a.txt" "$ws/b.txt" &
pid=$!
"$driver" "$ws" "$pid"; first=$?
wait "$pid"; echo "BROWSER_EXIT status=$?"
sleep 20
"$code" --enable-logging=stderr --user-data-dir="$p" --extensions-dir="$e" "$ws/a.txt" &
pid=$!
"$driver" "$ws" "$pid" --reopen; second=$?
wait "$pid"; echo "BROWSER_EXIT status=$?"
echo "VSCODE_EXTENDED first=$first reopen=$second"
sleep 20
touch "$HOME/lr-code-$tag-done"
[ "$first" = 0 ] && [ "$second" = 0 ]
