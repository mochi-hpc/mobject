#!/bin/bash
#
# General test script utilities
#

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

if [ -z "$TIMEOUT" ] ; then
    echo expected TIMEOUT variable defined to its respective command
    exit 1
fi

function run_to()
{
    maxtime=${1}s
    shift
    $TIMEOUT --signal=9 $maxtime "$@"
}

function mobject_test_start_servers()
{
    startwait=${1:-15}
    maxtime=${2:-120}
    # storage=${3:-/dev/shm/mobject.dat}
    storage=${3:-/tmp/mobject.dat}
    
    rm -rf ${storage}
    # bake-mkpool -s 50M /dev/shm/mobject.dat
    bake-mkpool -s 50M /tmp/mobject.dat
    
    run_to $maxtime bedrock na+sm -c $SCRIPT_DIR/config.json -v trace &
    if [ $? -ne 0 ]; then
        # TODO: this doesn't actually work; can't check return code of
        # something executing in background.  We have to rely on the
        # return codes of the actual client side tests to tell if
        # everything started properly
        exit 1
    fi

    # wait for servers to start
    sleep ${startwait}
}
