#!/bin/sh
#
# Test Mobject using GitHub Action.
#
# This script assumes that Spack installed dependencies under $PWD/install.
#
# Author: Hyokyung Lee (hyoklee@hdfgroup.org)
# Last Update: 2022-11-17


echo "Checking cwd"
echo $PWD

export LD_LIBRARY_PATH=$PWD/install/lib:$PWD/install/lib64
echo "Checking LD_LIBRARY_PATH"
echo $LD_LIBRARY_PATH

export PATH=$PWD/install/bin:$PATH
echo "Checking PATH"
echo $PATH

echo "Creating /usr/local/tmp to check root permission"
mkdir /usr/local/tmp

echo "Running sysctl"
/usr/sbin/sysctl kernel.yama.ptrace_scope=0

echo "Testing using make check"
make check
grep  -r \\[error\\] tests/*.sh.log
ls /tmp/
export MOBJECT_CLUSTER_FILE=$PWD/mobject.ssg
bedrock na+sm -c $PWD/tests/config.json -v trace &
ior -g -a RADOS -t 64k -b 128k --rados.user=foo --rados.pool=bar --rados.conf $MOBJECT_CLUSTER_FILE
