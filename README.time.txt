You may consider this if GPS UTC time setting is expected to work properly.
# systemctl disable systemd-timedated.service
# systemctl disable systemd-timesyncd.service

Check with:
# timedatectl

sdlSpeedometer is capable to set system time from GPS only when running as root.
