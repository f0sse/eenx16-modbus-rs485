
# Modbus

I detta träd finns ritningar och kod för en Raspberry Pi 4 HAT ämnad att prata
Modbus över RS485.

## Raspberry Pi 4

Lägg till följande i filen `/boot/config.txt`, under taggen `[pi4]`:

	dtoverlay=disable-bt
	dtoverlay=uart4,ctsrts
	dtparam=uart0=off
	dtparam=uart1=off
	enable_uart=0

Kör `systemctl disable hciuart` som root.

I filen `/boot/cmdline.txt`, se till att ta bort alla boot consoles som
refererar till `ttyAMA*`, och se till att raden innehåller
`plymouth.ignore-serial-consoles`.

För att dubbelkolla att hardware flow control är på för uart4, kör
`raspi-gpio get 8-11` och kolla att följande tabell stämmer.

| Funktion | GPIO | Pin |
|---------:|:----:|:---:|
|       TX |   8  |  24 |
|       RX |   9  |  21 |
|      CTS |  10  |  19 |
|      RTS |  11  |  23 |

## HAT

För att samla in data från de tre elmätarna i labbet, använder vi oss av en
egen-designad Raspberry Pi-HAT, som sköter seriell kommunikation med en[MAX483]
(https://www.analog.com/en/products/max483.html) och temperaturläsningar med
en [DS18B20](https://www.analog.com/en/products/ds18b20.html) (monterad på en
extern modul).

För att prata Modbus användes [libmodbus](https://libmodbus.org) och ett eget
program skrivet i C för att samla in momentan och total effekt för samtliga
faser på de tre elmätarna i interval om 5 sekunder, och skicka dessa data till
en tidsseriedatabas.

![3D-rendering av kretskortet](hat.png?raw=true "3D-rendering av kretskortet")

Alla design-filer finns under `/hat`.

## Misc

MAX483 gör det möjligt för oss att kommunicera enligt RS-485, men chippet kräver
att man sätter pinnarna 2-3 höga vid Tx och låga vid Rx.

Problemet med RPI var att en vanlig UART endast har pinnar för Rx/Tx, och inga
möjligheter att automatiskt kontrollera de nödvändiga pinnarna på något sätt.

Lyckligtvis har RPI 4 möjlgheter till hardware flow control, så den pinne man
sätter som Request To Send (RTS) kan användas till att sätta pinnarna 2-3
istället för det tänkta användningsområdet. Libmodbus var också skrivet runt
denna tanken för dess RTU-driver. På så vis kan man använda RPI  4 och uppåt,
och prata Modbus med endast dess UART.

`influx.c` innehåller ett mindre polerat bibliotek för att skriva mätdata till
[InfluxDB](https://www.influxdata.com) genom biblioteket [CURL](https://curl.se).

`modbus.c` använder libmodbus för att fråga tre olika
elmätare om fasvärden för spänning, ström, aktiv effekt och så vidare. För mer
information om vilka register som används, se manualen som ligger i mappen
`doc`.
