# battile - a desktop module in three lines (spec K.6, T23a): the battery
# charge as a tile on the launcher's HOME page, once a minute.  Start it
# with "sh /usr/share/examples/gui/battile.sh &"; kill it and the tile goes.
IFS='= '
while read key soc rest </dev/power
do guimod tile battery "Battery $soc%"; sleep 60; done
