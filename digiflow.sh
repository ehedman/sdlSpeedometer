#!/bin/bash

# dpendent on https://github.com/ehedman/flowSensor

tmpf=$(mktemp /tmp/flow.XXXXXXXXXX)
dataFile="$1"

if [ -z "${dataFile}" ]; then
    exit 2
fi

wget --timeout=2 -t1 --quiet -O - 'http://digiflow/ssi.shtml' | grep -E 'totv|tnkv|fage' | grep -oP 'value="\K[^"]+' > "${tmpf}"
wget --timeout=2 -t1 --quiet -O - 'http://digiflow/ssi_xml.shtml' | grep -A1 -m1  tds | awk 'FNR==2{print $1}' >> "${tmpf}"

rval="${PIPESTATUS[0]}"

if [ "${rval}" -eq 0 ]; then
    cp "${tmpf}" "${dataFile}"
fi

rm -f "${tmpf}"

exit "${rval}"
