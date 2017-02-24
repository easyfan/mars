#!/bin/bash
if [ $# -eq 0 ]
then
    COPY_DEST_PATH=/Users/easyfan/DTK2/dtk/src/main/jniLibs
else
    COPY_DEST_PATH=$1
fi

BUILD_FAILURE="build failure."
COPY_FROM_PATH=./mars_android_sdk/libs

ARCH_X86=x86
ARCH_ARMV7=armeabi-v7a

echo $COPY_DEST_PATH
echo $COPY_FROM_PATH
echo $ARCH_ARMV7
echo $ARCH_X86

function build() {
    return `python build_android.py 1 $1`
}

function copy_lib() {
    cp -f $COPY_FROM_PATH/$1/* $COPY_DEST_PATH/$1/.
}

function build_and_copy() {
    build $1
    if [ $? -eq -1 ]
    then
        echo $1 $BUILD_FAILURE
        return -1
    else
        copy_lib $1
    fi

}

build_and_copy $ARCH_X86
build_and_copy $ARCH_ARMV7