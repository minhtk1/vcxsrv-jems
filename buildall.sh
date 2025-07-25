#!/usr/bin/bash

if [[ "$1" == "1" ]] ; then
source ./setenv.sh 1
elif [[ "$1" == "0" ]] ; then
source ./setenv.sh 0
else
  echo "Please pass 1 (64-bit compilation) or 0 (32-bit compilation) as first argument"
  exit
fi
if [[ "$2" == "" ]] ; then
  echo "Please pass number of parallel builds as second argument"
  exit
fi
if [[ "$3" == "" ]] ; then
  echo "Please pass build type as third argument.  D for DEBUG, R for RELEASE, A for ALL"
  exit
fi
if [[ "$3" == "A" ]] ; then
BUILDRELEASE=1
BUILDDEBUG=1
fi
if [[ "$3" == "R" ]] ; then
BUILDRELEASE=1
BUILDDEBUG=0
fi
if [[ "$3" == "D" ]] ; then
BUILDRELEASE=0
BUILDDEBUG=1
fi
BUILDDEPS=1
if [[ "$4" == "N" ]] ; then
BUILDDEPS=0
fi
function check-error {
    errorcode=$?
    if [[ $errorcode != 0 ]]; then
        echo $errorcode: $1
        exit
    fi
}

which nasm.exe > /dev/null 2>&1
check-error 'Please install nasm'
which MSBuild.exe > /dev/null 2>&1
check-error 'Please install/set environment for visual studio 2022'
which python.exe > /dev/null 2>&1
check-error 'Please install python'
which jom.exe > /dev/null 2>&1
check-error 'Please install jom'

# c:\perl should have a copy of strawberry perl portable edition
which perl.exe > /dev/null 2>&1
check-error 'Please install strawberry perl portable edition into c:\perl'

# echo script lines from now one
# set -v

echo "[INFO] IS64=$IS64 BUILDRELEASE=$BUILDRELEASE BUILDDEBUG=$BUILDDEBUG BUILDDEPS=$BUILDDEPS THREADS=$2 TYPE=$3 PATH=$PATH PWD=$(pwd)"

set -o errexit     # hoặc: set -e
set -o errtrace    # để ERR được propagate vào cả các function/subshell

error_trap() {
  local code=$?
  local line=${BASH_LINENO[0]}
  echo "[ERROR] Exit code $code tại dòng $line (${BASH_SOURCE[1]})"
  exit $code
}

trap 'error_trap' ERR

if [[ "$BUILDDEPS" == "1" ]] ; then
	if [[ "$IS64" == "1" ]]; then
		if [[ "$BUILDRELEASE" == "1" ]] ; then
			echo MSBuild.exe freetype/freetypevc10.sln -t:Build -p:Configuration="Release Multithreaded" -p:Platform=x64 -m:$2
			MSBuild.exe freetype/freetypevc10.sln -t:Build -p:Configuration="Release Multithreaded" -p:Platform=x64 -m:$2
			check-error 'Error compiling freetype'
		fi
		if [[ "$BUILDDEBUG" == "1" ]] ; then
			echo MSBuild.exe freetype/freetypevc10.sln -t:Build -p:Configuration="Debug Multithreaded" -p:Platform=x64 -m:$2
			MSBuild.exe freetype/freetypevc10.sln -t:Build -p:Configuration="Debug Multithreaded" -p:Platform=x64 -m:$2
			check-error 'Error compiling freetype'
		fi
	else
		if [[ "$BUILDRELEASE" == "1" ]] ; then
			echo MSBuild.exe freetype/freetypevc10.sln -t:Build -p:Configuration="Release Multithreaded" -p:Platform=Win32 -m:$2
			MSBuild.exe freetype/freetypevc10.sln -t:Build -p:Configuration="Release Multithreaded" -p:Platform=Win32 -m:$2
			check-error 'Error compiling freetype'
		fi
		if [[ "$BUILDDEBUG" == "1" ]] ; then
			echo MSBuild.exe freetype/freetypevc10.sln -t:Build -p:Configuration="Debug Multithreaded" -p:Platform=Win32 -m:$2
			MSBuild.exe freetype/freetypevc10.sln -t:Build -p:Configuration="Debug Multithreaded" -p:Platform=Win32 -m:$2
			check-error 'Error compiling freetype'
		fi
	fi

	# OpenSSL
	if [[ "$BUILDRELEASE" == "1" ]] ; then
		cd openssl
		if [[ "$IS64" == "1" ]]; then
			mkdir -p release64 && cd release64
			perl.exe ../Configure VC-WIN64A --release no-makedepend
		else
			mkdir -p release32 && cd release32
			perl.exe ../Configure VC-WIN32 --release no-makedepend
		fi
		check-error 'Error executing perl'
		jom.exe /J$2
		check-error 'Error compiling openssl for release'
		cd ../..
	fi

	if [[ "$BUILDDEBUG" == "1" ]] ; then
		cd openssl
		if [[ "$IS64" == "1" ]]; then
			mkdir -p debug64 && cd debug64
			perl.exe ../Configure VC-WIN64A --debug no-makedepend
		else
			mkdir -p debug32 && cd debug32
			perl.exe ../Configure VC-WIN32 --debug no-makedepend
		fi
		check-error 'Error executing perl'
		jom.exe /J$2
		check-error 'Error compiling openssl for debug'
		cd ../..
	fi

	# PThreads
	cd pthreads
	if [[ "$BUILDRELEASE" == "1" ]] ; then
		nmake.exe VC-static
		check-error 'Error compiling pthreads for release'
	fi
	if [[ "$BUILDDEBUG" == "1" ]] ; then
		nmake.exe VC-static-debug
		check-error 'Error compiling pthreads for debug'
	fi
	cd ..

fi # End of BUILDDEPS block

# Build mhmake and vcxsrv
if [[ "$IS64" == "1" ]]; then
	if [[ "$BUILDDEPS" == "1" ]]; then
		if [[ "$BUILDRELEASE" == "1" ]]; then
			MSBuild.exe tools/mhmake/mhmakevc10.sln -t:Build -p:Configuration=Release -p:Platform=x64 -m:$2
			wait
			check-error 'Error compiling mhmake for release'
		fi
		if [[ "$BUILDDEBUG" == "1" ]]; then
			MSBuild.exe tools/mhmake/mhmakevc10.sln -t:Build -p:Configuration=Debug -p:Platform=x64 -m:$2
			wait
			check-error 'Error compiling mhmake for debug'
		fi
	fi

	if [[ "$BUILDRELEASE" == "1" ]]; then
		tools/mhmake/Release64/mhmake.exe -P$2 -C xorg-server MAKESERVER=1
		check-error 'Error compiling vcxsrv for release'
	fi
	if [[ "$BUILDDEBUG" == "1" ]]; then
		tools/mhmake/Debug64/mhmake.exe -P$2 -C xorg-server MAKESERVER=1 DEBUG=1
		check-error 'Error compiling vcxsrv for debug'
	fi

	cd xorg-server/installer
	# ./packageall.bat nox86
else # IS32
	if [[ "$BUILDDEPS" == "1" ]]; then
		if [[ "$BUILDRELEASE" == "1" ]]; then
		MSBuild.exe tools/mhmake/mhmakevc10.sln -t:Build -p:Configuration=Release -p:Platform=Win32 -m:$2
		wait
		check-error 'Error compiling mhmake for release'
		fi
		if [[ "$BUILDDEBUG" == "1" ]]; then
		MSBuild.exe tools/mhmake/mhmakevc10.sln -t:Build -p:Configuration=Debug -p:Platform=Win32 -m:$2
		wait
		check-error 'Error compiling mhmake for debug'
		fi
	fi

	if [[ "$BUILDRELEASE" == "1" ]]; then
		tools/mhmake/Release/mhmake.exe -P$2 -C xorg-server MAKESERVER=1
		check-error 'Error compiling vcxsrv for release'
	fi
	if [[ "$BUILDDEBUG" == "1" ]]; then
		tools/mhmake/Release/mhmake.exe -P$2 -C xorg-server MAKESERVER=1 DEBUG=1
		check-error 'Error compiling vcxsrv for debug'
	fi

	cd xorg-server/installer
	#   ./packageall.bat nox64
fi




