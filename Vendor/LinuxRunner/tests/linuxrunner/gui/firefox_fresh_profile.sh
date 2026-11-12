export MOZ_ENABLE_WAYLAND=0 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe
exec /usr/bin/firefox-esr --no-remote --profile __LR_FIREFOX_PROFILE__ file:///home/mobile/Downloads/firefox_performance.html
