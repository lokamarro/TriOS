#!/bin/sh
if command -v lxpanelctl >/dev/null; then exec lxpanelctl menu; fi
exec xfce4-popup-applicationsmenu
