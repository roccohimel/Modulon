#!/bin/sh

mkdir -p bin
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Iinclude -o bin/mlonc src/*.c
