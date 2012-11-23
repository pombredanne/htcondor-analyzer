#!/bin/bash

path=$(dirname "$0")
plugin="$path/plugin.so"

if test -e "$plugin" ; then
    exec clang++ -Xclang -load -Xclang "$plugin" \
	-Xclang -add-plugin -Xclang htcondor-analysis "$@"
else
    echo "error: could not find Clang plugin for script $0" 2>&1
    exit 15
fi
