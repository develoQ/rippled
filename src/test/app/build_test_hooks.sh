#!/bin/bash
echo '
//This file is generated by build_test_hooks.h
#ifndef SETHOOK_WASM_INCLUDED
#define SETHOOK_WASM_INCLUDED
#include <map>
#include <stdint.h>
#include <string>
#include <vector>
namespace ripple {
namespace test {
std::map<std::string, std::vector<uint8_t>> wasm = {' > SetHook_wasm.h
COUNTER="0"
cat SetHook_test.cpp | tr '\n' '\f' | 
        grep -Po 'R"\[test\.hook\](.*?)\[test\.hook\]"' | 
        sed -E 's/R"\[test\.hook\]\(//g' | 
        sed -E 's/\)\[test\.hook\]"[\f \t]*/\/*end*\//g' | 
        while read -r line
        do
            echo "/* ==== WASM: $COUNTER ==== */" >> SetHook_wasm.h
            echo -n '{ R"[test.hook](' >> SetHook_wasm.h
            cat <<< $line | sed -E 's/.{7}$//g' | tr -d '\n' | tr '\f' '\n' >> SetHook_wasm.h
            echo ')[test.hook]",' >> SetHook_wasm.h
            echo "{" >> SetHook_wasm.h
            WAT=`grep -Eo '\(module' <<< $line | wc -l`
            if [ "$WAT" -eq "0" ]
            then
                wasmcc -x c /dev/stdin -o /dev/stdout -O2 -Wl,--allow-undefined <<< `tr '\f' '\n' <<< $line` | 
                    hook-cleaner - - |
                    xxd -p -u -c 19 | 
                    sed -E 's/../0x\0U,/g' | sed -E 's/^/    /g' >> SetHook_wasm.h
            else
                wat2wasm - -o /dev/stdout <<< `tr '\f' '\n' <<< $(sed -E 's/.{7}$//g' <<< $line)` |
                    xxd -p -u -c 19 | 
                    sed -E 's/../0x\0U,/g' | sed -E 's/^/    /g' >> SetHook_wasm.h
            fi
            echo '}},' >> SetHook_wasm.h
            echo >> SetHook_wasm.h
            COUNTER=`echo $COUNTER + 1 | bc`
        done
echo '};
}
}
#endif' >> SetHook_wasm.h
