#!/bin/sh
# Browser multi-tab gate workload (browser-network probe): one browser, a fresh
# profile, firefox_performance.html. Instantiate with sed, replacing
# __BROWSER__ (brave|firefox|chromium) and __PROFILE__ (a new profile name).
# Firefox's first run otherwise opens onboarding tabs (data-reporting privacy
# notice, about:welcome) that change the tab count the probe drives; the
# preferences below only skip those notices.
set -u
browser=__BROWSER__
case "$browser" in
 brave) profile="$HOME/.config/__PROFILE__";;
 chromium) profile="$HOME/.config/__PROFILE__";;
 firefox) profile="$HOME/.mozilla/__PROFILE__";;
 *) echo "BROWSER_GATE unknown browser"; exit 2;;
esac
test ! -e "$profile" || { echo "BROWSER_GATE profile exists: use a new name"; exit 2; }
mkdir -p "$profile"
page=file:///home/mobile/Downloads/firefox_performance.html
case "$browser" in
 brave) /opt/linuxrunner-fixture-apps-874/brave/brave --enable-logging=stderr --user-data-dir="$profile" "$page";;
 chromium) chromium --enable-logging=stderr --no-first-run --user-data-dir="$profile" "$page";;
 firefox)
  cat > "$profile/user.js" <<'EOF'
user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);
user_pref("datareporting.policy.firstRunURL", "");
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
EOF
  /usr/lib/firefox-esr/firefox-esr --no-remote --new-instance --profile "$profile" "$page";;
esac
status=$?
echo "BROWSER_EXIT status=$status"
sleep 20
exit "$status"
