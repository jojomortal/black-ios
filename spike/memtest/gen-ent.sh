#!/bin/sh
# usage: gen-ent.sh <variant> <team> <bundle id>  -> entitlements plist on stdout
V=$1; T=$2; B=$3
EVA='<key>com.apple.developer.kernel.extended-virtual-addressing</key><true/>'
IML='<key>com.apple.developer.kernel.increased-memory-limit</key><true/>'
case $V in none) X="";; eva) X=$EVA;; iml) X=$IML;; both) X="$EVA
$IML";; *) echo "bad variant" >&2; exit 1;; esac
cat <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>application-identifier</key><string>$T.$B</string>
<key>com.apple.developer.team-identifier</key><string>$T</string>
<key>get-task-allow</key><false/>
$X
</dict></plist>
P
