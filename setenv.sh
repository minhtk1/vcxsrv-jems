#!/bin/bash

if [[ "$1" == "1" ]] ; then
    relese="Release64"
else
    relese="Release"
fi

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

rm -f commands.sh
python setenv.py $1 > commands.sh
chmod +x commands.sh
source commands.sh
if [ -z "${CYGWIN}" ]; then
    export PATH=/usr/local/bin:/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games:/usr/local/games:/usr/lib/wsl/lib:${DIR}/tools/mhmake/${release}:/mnt/c/nasm:$PATH:/mnt/c/gnuwin32/bin:/mnt/c/perl/perl/bin
    export MHMAKECONF=${DIR}
    export PYTHON3=/mnt/c/Python39/python.exe
    export WSLENV="${WSLENV}:MHMAKECONF/l:PYTHON3/l:IS64/l:CFLAGS/l"
else
    export PATH=${DIR}/tools/mhmake/${release}:$PATH:/mnt/c/Python39
    if [ -z "${MHMAKECONF}" ] ; then
        export MHMAKECONF=$(cygpath -w ${DIR})
        export PYTHON3=python.exe
    fi
fi
rm -f commands.sh
export CFLAGS="/FS"
export IS64=$1

