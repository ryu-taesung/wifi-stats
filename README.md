### `gcc -Wall -g -I/usr/include/libnl3 -o wifi_qos_collector wifi_qos_collector.c -lnl-3 -lnl-genl-3`
### `sudo env QOS_SOCK=/run/user/$(id -u)/wifi_qos.sock ./wifi_qos_collector -i 10000 wlan0`
### `QOS_SOCK=/run/user/$(id -u)/wifi_qos.sock node wifi-qos-listener.mjs`
